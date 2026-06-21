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

#ifndef _FOX_TLS_CERT_HPP
#define _FOX_TLS_CERT_HPP

#include <string>

// Fox_Tls_Cert manages the self-signed certificate used by the HTTPS listener.
//
// The certificate is generated once (ECDSA P-256, self-signed, SHA-256) and
// persisted under /data/media/0/Fox/tls/ so it survives reboots; a browser may
// then accept/pin it once. CN/SAN carry the WLAN IP (when available) plus
// "localhost". Call EnsureCert() before starting the TLS listener; RegenCert()
// forces a fresh keypair+cert (used by the `webserver https cert regen` verb).
//
// Threat model: same as Fox_Secrets — the key lives on the FBE-encrypted user
// data partition (mode 0600) and protects against casual extraction, but it is
// NOT hardware-backed. Anyone who boots this recovery can read/regenerate it.
class Fox_Tls_Cert
{
public:
	// Ensure a usable cert+key exist on disk (reuse if present and parseable,
	// generate otherwise). On success, cert_path/key_path are set. On failure,
	// err is set and false is returned. Idempotent.
	static bool EnsureCert(std::string& cert_path, std::string& key_path, std::string& err);

	// Force a brand-new keypair+cert, overwriting any existing files. The TLS
	// listener must be restarted afterwards to pick up the new material.
	static bool RegenCert(std::string& err);

	// SHA-256 fingerprint of the certificate's DER (hex, colon-separated),
	// suitable for browser pinning. Empty on error.
	static std::string Fingerprint();

	// Absolute paths (constants; no allocation). Cert: server.crt, key: server.key.
	static const char* CertPath() { return "/data/media/0/Fox/tls/server.crt"; }
	static const char* KeyPath()  { return "/data/media/0/Fox/tls/server.key"; }
};

#endif // _FOX_TLS_CERT_HPP
