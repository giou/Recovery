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

#include "fox_io.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace Fox_Io {

void SetTimeouts(int fd, int seconds)
{
	if (fd < 0)
		return;
	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Conn::Conn(int fd) : fd_(fd), ssl_(nullptr) {}

Conn::Conn(int fd, SSL* ssl) : fd_(fd), ssl_(ssl) {}

Conn::~Conn()
{
	close();
}

Conn::Conn(Conn&& other) noexcept : fd_(other.fd_), ssl_(other.ssl_)
{
	other.fd_ = -1;
	other.ssl_ = nullptr;
}

Conn& Conn::operator=(Conn&& other) noexcept
{
	if (this != &other) {
		close();
		fd_ = other.fd_;
		ssl_ = other.ssl_;
		other.fd_ = -1;
		other.ssl_ = nullptr;
	}
	return *this;
}

ssize_t Conn::recv(void* buf, size_t len)
{
	if (fd_ < 0)
		return -1;
	if (ssl_)
		return SSL_read(ssl_, buf, (int)len);
	return ::recv(fd_, buf, len, 0);
}

ssize_t Conn::send(const void* buf, size_t len)
{
	if (fd_ < 0)
		return -1;
	if (ssl_)
		return SSL_write(ssl_, buf, (int)len);
	return ::send(fd_, buf, len, MSG_NOSIGNAL);
}

void Conn::close()
{
	if (ssl_) {
		// Best-effort bidirectional shutdown; ignore errors (peer may be gone).
		SSL_shutdown(ssl_);
		SSL_free(ssl_);
		ssl_ = nullptr;
	}
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

} // namespace Fox_Io
