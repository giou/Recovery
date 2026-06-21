/*
	Copyright (C) 2026 OrangeFox Recovery Project
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

#ifndef _FOX_HTTPSD_HPP
#define _FOX_HTTPSD_HPP

// Fox_Httpsd is the HTTPS (TLS) twin of Fox_Httpd: it owns its own listener
// socket on a separate port (default 443) and accepts connections, performing
// the TLS handshake before handing the established SSL* to the same request
// handler the plaintext server uses (Fox_Http::HandleConnection, via the
// Fox_Io::Conn wrapper). The cert/key come from Fox_Tls_Cert (self-signed).
// Start() generates/loads the cert if needed. TLS 1.2+ only.
class Fox_Httpsd
{
public:
	// Start the TLS listener on 'port' (default 443). Ensures a cert exists,
	// builds the SSL_CTX, and spawns the accept thread. Idempotent. Returns
	// false if the cert can't be produced or bind/listen fails.
	static bool Start(int port = 443);

	// Stop the listener and free the SSL_CTX.
	static void Stop();

	static bool IsRunning();
	static int Port();

	// Default HTTPS port when none is requested.
	static int DefaultPort() { return 443; }
};

#endif // _FOX_HTTPSD_HPP
