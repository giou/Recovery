/*
	Copyright (C) 2024-2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _FOX_HTTPD_HPP
#define _FOX_HTTPD_HPP

#include <string>

// Fox_Httpd is a tiny embedded HTTP server that exposes the recovery engine as
// a remote-control dashboard over the WLAN interface:
//
//   - REST control plane over the async job layer (Fox_Jobs)
//   - MJPEG screen stream (Fox_Screen)
//   - touch/key injection (Fox_Remote_Input)
//
// The server (the network listener + REST/job control plane) and the
// remote-control plane (screen mirroring + input injection) are toggled
// independently, so the dashboard can run without exposing screen/input
// ("dashboard but no remote control"), and screen/input can be driven over a
// plain adb shell (`fox input` / `fox screencap`) without the server. The CLI
// surfaces this as `fox webserver` and `fox control`.
//
// It is always built but started explicitly, and every data endpoint requires a
// per-session bearer token, because the server grants unauthenticated root with
// decryption powers otherwise. Bind only on trusted networks.
class Fox_Httpd
{
public:
	// Start the listener on 'port' (default 80). Generates a fresh auth
	// token and initialises the uinput device. Idempotent. Returns false on
	// bind/listen failure.
	static bool Start(int port = 80);

	// Stop the listener and tear down the uinput device.
	static void Stop();

	static bool IsRunning();
	static int Port();

	// The current session token (empty if not running).
	static std::string Token();

	// Best-effort first non-loopback IPv4 address, for printing the URL.
	static std::string LocalAddress();

	// Remote-control plane (screen mirroring + input injection over HTTP).
	// Independent of the server lifecycle: the flag can be set before or while
	// the server runs. When disabled, the /screen.png, /stream.mjpeg and
	// /api/input/* endpoints return 403; the REST/job control plane is
	// unaffected. The CLI `fox input` / `fox screencap` commands are not gated
	// by this (they require adb shell access already). Enabled by default.
	static void SetControlEnabled(bool on);
	static bool ControlEnabled();
};

#endif // _FOX_HTTPD_HPP
