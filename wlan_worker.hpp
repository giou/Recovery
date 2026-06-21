#pragma once

/*
 * WlanWorker — serializes every WLAN operation onto a single dedicated
 * background thread so the GUI action thread never blocks on radio bring-up,
 * scan, association or DHCP latency.
 *
 * Before this existed, the WLAN GUI actions ran on OrangeFox's single shared
 * ActionThread and were fully synchronous: enabling the radio blocked it for
 * ~10-20s (cnss cold-boot + supplicant start), scan for ~5s, connect for ~15s.
 * While that thread was busy, every other threaded action — including the
 * wlan page's own status refresh — was rejected with "Another threaded action
 * is already running", so the toggle appeared frozen and the network list
 * could not be refreshed.
 *
 * Now the GUI actions merely Post() a command and return immediately. The
 * worker runs the operation and publishes progress through the `wlan_state`
 * DataManager variable (see wlan_worker.cpp for the state machine), which the
 * GUI binds to reactively.
 */
class WlanWorker {
public:
	// Only the toggle + status operations are dispatched here. Scan, connect
	// and test stay synchronous on the GUI ActionThread because their modal
	// overlays require the work to block within the action batch.
	enum Cmd {
		CMD_ENABLE,
		CMD_DISABLE,
		CMD_INFO,
		CMD_AUTOSTART,  // boot: enable + (optionally) re-join the last network
	};

	// Enqueue a command for asynchronous execution. Safe to call from any
	// thread (GUI / action thread). Starts the worker thread on first use and
	// returns immediately. Identical idempotent commands already queued are
	// coalesced so rapid taps cannot pile up.
	static void Post(Cmd cmd);

	// True while the worker is executing a long operation (used by the GUI to
	// show a spinner / disable the toggle during transitions).
	static bool Busy();
};
