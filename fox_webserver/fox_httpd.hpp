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
// plain adb shell (input/screencap RPC verbs) without the server.
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

	// True when the token is the user-configured remote password (which must
	// not be echoed to logs/console). False for the random per-boot fallback.
	static bool AuthIsPassword();

	// Best-effort first non-loopback IPv4 address, for printing the URL.
	static std::string LocalAddress();

	// Remote-control plane (screen mirroring + input injection over HTTP).
	// Independent of the server lifecycle: the flag can be set before or while
	// the server runs. When disabled, the /screen.png, /stream.mjpeg and
	// /api/input/* endpoints return 403; the REST/job control plane is
	// unaffected. The direct input/screencap RPC verbs are not gated by this
	// (they require adb shell access already). Enabled by default.
	static void SetControlEnabled(bool on);
	static bool ControlEnabled();

	// HTTPS-redirect mode for the plaintext listener. When enabled, plaintext
	// (non-TLS) connections are answered with a 301 to the HTTPS endpoint and
	// nothing else — no auth, no static, no API. The preference is persisted so
	// a reboot honours it. Off by default.
	static void SetRedirectEnabled(bool on);
	static bool RedirectEnabled();

	// The HTTPS port that plaintext redirects should target. Defaults to 443;
	// reflects Fox_Httpsd's actual port when it is running.
	static int HttpsPortForRedirect();
	static void SetHttpsPortForRedirect(int port);

	// Cross-origin policy. The default allow-list is the device-local origins
	// (its WLAN IP, localhost) plus the official orangefox.tech and
	// orangefox.download domains and their subdomains. SetCorsOverride installs
	// a developer override ("*" = any origin; "" = restore the default).
	// ResolveCorsOrigin maps a request's Origin header to the value echoed back
	// in Access-Control-Allow-Origin ("" = send no CORS header).
	static void SetCorsOverride(const std::string& origin);
	static std::string CorsOverride();
	static std::string ResolveCorsOrigin(const std::string& request_origin);
};

#endif // _FOX_HTTPD_HPP
