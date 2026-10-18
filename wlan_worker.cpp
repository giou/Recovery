#include "wlan_worker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>

#include "data.hpp"
#include "wlan.hpp"
#include "fox_adbd.hpp"
#include "gui/pages.hpp"  // gui_toast

/*
 * State machine published through the `wlan_state` DataManager variable:
 *
 *   "disabled"    radio off (initial / after Disable)
 *   "enabling"    bring-up in progress (cnss cold-boot + supplicant start)
 *   "disabling"   tear-down in progress
 *   "enabled"     radio up, not connected
 *   "connected"   associated with an IP
 *   "error"       last enable failed
 *
 * The transient "scanning"/"connecting" feedback stays driven by the existing
 * wlan_refreshing / wlan_connect_* vars. A user-initiated scan/connect still runs
 * synchronously on the GUI ActionThread; this worker also drives scans (auto-scan
 * after enable, autostart). Both paths are serialized against each other by the
 * recursive operation lock in wlan.cpp, so they never interleave.
 *
 * The legacy flags tw_wlan_enabled / tw_wlan_connected and the wlan_connect_*
 * progress vars are still maintained by the underlying Wlan methods, so old XML
 * keeps working; new XML binds to wlan_state for richer feedback.
 */

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<WlanWorker::Cmd> g_queue;
std::atomic<bool> g_busy{false};
bool g_thread_started = false;

// The command currently being executed by Handle(), tracked under g_mutex so
// Post() can coalesce a duplicate against the in-flight command, not just the
// ones still queued.
bool g_has_current = false;
WlanWorker::Cmd g_current;

// Background status refresh cadence while the radio is up and idle. Keeps the
// GUI status fresh without the UI having to poll wpa_cli itself.
const int kIdleRefreshSeconds = 2;

void SetState(const char* s) {
	DataManager::SetValue("wlan_state", s);
}

bool IsEnabledNow() {
	return DataManager::GetIntValue("tw_wlan_enabled") == 1;
}

bool IsConnectedNow() {
	return DataManager::GetIntValue("tw_wlan_connected") == 1;
}

// The resting state once a transient operation finishes.
const char* SettledState() {
	if (!IsEnabledNow())
		return "disabled";
	return IsConnectedNow() ? "connected" : "enabled";
}

bool ScanFoundNetworks() {
	struct stat st;
	return stat("/tmp/wlan/list.txt", &st) == 0 && st.st_size > 0;
}

// Bring the radio's network list up to date: settle ~2s, scan, surface results
// on the wlan page, and toast if nothing was found.
void SettleAndScan() {
	std::this_thread::sleep_for(std::chrono::seconds(2));
	DataManager::SetValue("wlan_refreshing", 1);
	Wlan::Scan();
	DataManager::SetValue("wlan_refreshing", 0);
	Wlan::RefreshWlanPageIfShown();
	if (!ScanFoundNetworks())
		gui_run_on_main([]() { gui_toast("[warning]No networks found", 120); });
}

void Handle(WlanWorker::Cmd cmd) {
	switch (cmd) {
	case WlanWorker::CMD_ENABLE:
		SetState("enabling");
		Wlan::Enable();
		if (IsEnabledNow()) {
			SetState(SettledState());
			// Wait ~2s for the radio to settle, then auto-scan (and toast if
			// nothing was found). The spinner indicated "enabling" until now.
			SettleAndScan();
		} else {
			SetState("error");
			gui_run_on_main([]() { gui_toast("[error]Failed to enable WiFi", 150); });
		}
		break;

	case WlanWorker::CMD_DISABLE:
		SetState("disabling");
		Wlan::Disable();
		SetState("disabled");
		break;

	case WlanWorker::CMD_INFO:
		Wlan::Info();
		break;

	case WlanWorker::CMD_AUTOSTART:
		// Boot-time auto-connect. The of_wlan_auto_enable toggle gates the
		// normal path; of_adbd_auto ("Network ADB automatically") gates the
		// pre-decrypt path on its own, since saved credentials on encrypted
		// /data are unreadable when this first runs.
		if (DataManager::GetIntValue("of_wlan_auto_enable") != 1
		    && DataManager::GetIntValue("of_adbd_auto") != 1)
			break;
		if (!IsEnabledNow()) {
			SetState("enabling");
			Wlan::Enable();
		}
		if (!IsEnabledNow()) {
			SetState("error");
			gui_run_on_main([]() { gui_toast("[error]Failed to enable WiFi", 150); });
			break;
		}
		SetState(SettledState());
		SettleAndScan();
		if (DataManager::GetIntValue("of_wlan_auto_connect") == 1 && !IsConnectedNow()) {
			std::string ssid = DataManager::GetStrValue("of_wlan_last_ssid");
			if (!ssid.empty()) {
				DataManager::SetValue("wlanselectedid", ssid);
				Wlan::ConnectSaved();
				SetState(SettledState());
				Wlan::RefreshWlanPageIfShown();
			}
		}
		// Pre-decrypt fallback for of_adbd_auto: ConnectSaved() above needs
		// Fox_Secrets on encrypted /data, so when there is still no link,
		// join with the persisted PSK instead. Post-decrypt this is a no-op
		// (already connected) and the saved path stays authoritative.
		if (DataManager::GetIntValue("of_adbd_auto") == 1 && !IsConnectedNow()) {
			std::string ssid = DataManager::GetStrValue("of_wlan_last_ssid");
			std::string psk = DataManager::GetStrValue("of_wlan_last_psk");
			if (!ssid.empty() && !psk.empty()) {
				if (Wlan::ConnectInline(ssid, psk)) {
					SetState(SettledState());
					Wlan::RefreshWlanPageIfShown();
				}
			}
		}
		// Hands-off sessions: with of_adbd_auto the network ADB comes up with
		// the WLAN at boot, so a cable-less recovery (e.g. HDD on OTG) is
		// reachable without touching the UI. Authorized devices persist in
		// Fox_Secrets, so no re-pairing is needed after the first time.
		if (DataManager::GetIntValue("of_adbd_auto") == 1) {
			if (!IsConnectedNow()) {
				gui_run_on_main([]() { gui_toast("[error]Network ADB needs a WLAN connection", 150); });
			} else if (!Fox_Adbd::StartSecure(5555)) {
				gui_run_on_main([]() { gui_toast("[error]Failed to start network ADB", 150); });
			}
		}
		break;
	}
}

void WorkerLoop() {
	for (;;) {
		WlanWorker::Cmd cmd;
		{
			std::unique_lock<std::mutex> lk(g_mutex);
			if (g_queue.empty()) {
				// While the radio is up, wake periodically to refresh status;
				// while it is down there is nothing to poll, so sleep until a
				// command arrives.
				if (IsEnabledNow())
					g_cv.wait_for(lk, std::chrono::seconds(kIdleRefreshSeconds));
				else
					g_cv.wait(lk);
			}

			if (g_queue.empty()) {
				// Woke without work: do a light background status refresh so the
				// GUI reflects link changes (e.g. an async (dis)connect) without
				// polling from the UI thread. Non-blocking: yields rather than
				// stalling for seconds behind a foreground scan/connect.
				lk.unlock();
				if (IsEnabledNow())
					Wlan::RefreshInfoIfIdle();
				continue;
			}

			cmd = g_queue.front();
			g_queue.pop_front();
			g_has_current = true;
			g_current = cmd;
		}

		g_busy.store(true);
		Handle(cmd);
		g_busy.store(false);
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_has_current = false;
		}
	}
}

// All dispatched commands are idempotent, so an identical one already queued
// can be dropped — this is what keeps rapid toggle taps or repeated page-load
// status refreshes from piling up.
bool Coalescable(WlanWorker::Cmd c) {
	return c == WlanWorker::CMD_INFO || c == WlanWorker::CMD_ENABLE ||
	       c == WlanWorker::CMD_DISABLE || c == WlanWorker::CMD_AUTOSTART;
}

}  // namespace

void WlanWorker::Post(Cmd cmd) {
	std::unique_lock<std::mutex> lk(g_mutex);

	if (!g_thread_started) {
		g_thread_started = true;
		std::thread(WorkerLoop).detach();
	}

	if (Coalescable(cmd)) {
		if (g_has_current && g_current == cmd)
			return;  // identical command already executing
		for (Cmd queued : g_queue) {
			if (queued == cmd)
				return;  // identical command already pending
		}
	}

	g_queue.push_back(cmd);
	g_cv.notify_one();
}

bool WlanWorker::Busy() {
	return g_busy.load();
}
