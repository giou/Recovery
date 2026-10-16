#pragma once

#include <string>
#include <vector>

/*
 * WpaCtrl — a minimal, self-contained wpa_supplicant control-interface client.
 *
 * It speaks the wpa_supplicant UNIX-domain ctrl protocol directly over POSIX
 * sockets, so it pulls in NO supplicant sources (wpa_ctrl.c/os_unix.c are
 * vendor-proprietary and awkward to compile into the system recovery binary).
 *
 * Two roles, matching how wpa_cli works internally:
 *   - a command channel: Open() only, then Request() for command/reply.
 *   - a monitor channel:  Open() + Attach(), then WaitEvent() for unsolicited
 *     CTRL-EVENT-* messages.
 *
 * Replaces the previous fork+exec-per-command (wpa_cli) transport and the
 * fixed sleep-poll loops in scan/connect with event-driven waits.
 */
class WpaCtrl {
public:
	WpaCtrl();
	~WpaCtrl();

	// Connect a datagram socket to the per-interface ctrl socket
	// (e.g. /tmp/recovery/sockets/wlan0). Closes any previous connection.
	bool Open(const std::string& server_path);
	void Close();
	bool IsOpen() const { return mFd >= 0; }

	// Register/unregister this channel as an unsolicited-event monitor.
	bool Attach();
	bool Detach();

	// Send a command and read its reply. Returns false only on transport
	// failure (socket error / timeout), matching wpa_cli's "command delivered"
	// semantics — a supplicant "FAIL" reply is still a successful transport.
	bool Request(const std::string& cmd, std::string& reply, int timeout_ms = 2000);

	// Block until an unsolicited event containing any of `any_of` arrives, or
	// `timeout_ms` elapses. On match, `matched` holds the full event line.
	// Requires Attach(). Returns false on timeout / not attached.
	bool WaitEvent(const std::vector<std::string>& any_of, int timeout_ms, std::string& matched);

private:
	int mFd;
	std::string mLocalPath;  // our bound client socket path (unlinked on close)
};
