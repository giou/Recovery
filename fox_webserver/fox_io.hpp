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

#ifndef _FOX_IO_HPP
#define _FOX_IO_HPP

#include <openssl/ssl.h>

#include <cstddef>

// Fox_Io::Conn unifies plain-socket and TLS I/O behind one face so the HTTP
// request handlers (fox_http_routes / _common / _api / _screen / _static) can
// be written once and driven over either transport. The plaintext constructor
// wraps a socket fd; the TLS constructor wraps an fd plus an established SSL*
// (the accept path in fox_httpsd.cpp performs SSL_accept() before constructing
// a Conn). All handlers operate on Conn by value; the destructor closes the
// underlying fd (and, for TLS, shuts down and frees the SSL object).
namespace Fox_Io {

// Bound a freshly-accepted socket's blocking I/O so a peer that connects and
// then stalls (slow-loris) cannot pin a handler thread forever. Applies both a
// receive and a send deadline. Best-effort; failures are ignored.
void SetTimeouts(int fd, int seconds);

class Conn {
public:
	// Plaintext connection. Takes ownership of fd.
	explicit Conn(int fd);
	// TLS connection. Takes ownership of both fd and ssl (ssl may be null to
	// fall back to plaintext behaviour, e.g. on handshake failure cleanup).
	Conn(int fd, SSL* ssl);
	~Conn();

	Conn(const Conn&) = delete;
	Conn& operator=(const Conn&) = delete;

	// Movable so handlers can pass Conns through by value into sub-handlers.
	Conn(Conn&& other) noexcept;
	Conn& operator=(Conn&& other) noexcept;

	ssize_t recv(void* buf, size_t len);
	ssize_t send(const void* buf, size_t len);
	// Releases the fd/ssl. Safe to call repeatedly.
	void close();

	bool is_tls() const { return ssl_ != nullptr; }
	int fd() const { return fd_; }

private:
	int fd_ = -1;
	SSL* ssl_ = nullptr;
};

} // namespace Fox_Io

#endif // _FOX_IO_HPP
