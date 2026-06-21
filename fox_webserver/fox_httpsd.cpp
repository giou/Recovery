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

#include "fox_httpsd.hpp"

#ifdef OF_ENABLE_WLAN

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "fox_http_routes.hpp"
#include "fox_httpd.hpp"
#include "fox_io.hpp"
#include "fox_tls_cert.hpp"
#include "../fox_fifo/fox_remote_input.hpp"
#include "../twcommon.h"

namespace {

volatile bool g_running = false;
int g_listen_fd = -1;
int g_port = 443;
SSL_CTX* g_ctx = nullptr;
pthread_t g_accept_thread;

struct ConnArg {
	int fd;
};

void* connection_thread(void* arg)
{
	ConnArg* ca = static_cast<ConnArg*>(arg);
	int fd = ca->fd;
	delete ca;

	SSL* ssl = SSL_new(g_ctx);
	if (!ssl) {
		close(fd);
		return nullptr;
	}
	SSL_set_fd(ssl, fd);

	// Short handshake deadline so a misbehaving peer can't pin a thread. Bound
	// both directions: a peer that completes the read side but stops reading can
	// otherwise stall our handshake writes once the TCP send buffer fills.
	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	int rc = SSL_accept(ssl);
	if (rc <= 0) {
		SSL_free(ssl);
		close(fd);
		return nullptr;
	}

	// Bound the handler's blocking reads/writes so a stalled peer cannot pin
	// this thread (do not reset to an infinite timeout after the handshake).
	Fox_Io::SetTimeouts(fd, 30);

	// HandleConnection closes the connection (and thus frees ssl/fd via the
	// Conn destructor). Pass by value (move) so ownership transfers cleanly.
	Fox_Io::Conn conn(fd, ssl);
	Fox_Http::HandleConnection(std::move(conn));
	return nullptr;
}

void* accept_thread(void*)
{
	while (g_running) {
		struct sockaddr_in cli;
		socklen_t len = sizeof(cli);
		int cfd = accept(g_listen_fd, (struct sockaddr*)&cli, &len);
		if (cfd < 0) {
			if (!g_running)
				break;
			// Back off so a persistent accept error (e.g. EMFILE) does not
			// spin this loop at 100% CPU.
			usleep(20 * 1000);
			continue;
		}
		int one = 1;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		ConnArg* ca = new ConnArg{ cfd };
		pthread_t t;
		if (pthread_create(&t, nullptr, connection_thread, ca) != 0) {
			close(cfd);
			delete ca;
		} else {
			pthread_detach(t);
		}
	}
	return nullptr;
}

}  // namespace

bool Fox_Httpsd::Start(int port)
{
	if (g_running)
		return true;

	g_port = port > 0 ? port : DefaultPort();

	std::string cert_path, key_path, err;
	if (!Fox_Tls_Cert::EnsureCert(cert_path, key_path, err)) {
		LOGERR("Fox_Httpsd: no cert: %s\n", err.c_str());
		return false;
	}

	g_ctx = SSL_CTX_new(TLS_server_method());
	if (!g_ctx) {
		LOGERR("Fox_Httpsd: SSL_CTX_new failed\n");
		return false;
	}
	// TLS 1.2+ only; disable legacy renegotiation / old protocols.
	SSL_CTX_set_options(g_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
	                           SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 |
	                           SSL_OP_NO_COMPRESSION);
	// The fox auth token already authenticates; TLS provides confidentiality.
	// No client-cert verification.
	SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, nullptr);

	if (SSL_CTX_use_certificate_file(g_ctx, cert_path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
	    SSL_CTX_use_PrivateKey_file(g_ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
		LOGERR("Fox_Httpsd: cannot load cert/key\n");
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
		return false;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		LOGERR("Fox_Httpsd: socket() failed\n");
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
		return false;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)g_port);

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		LOGERR("Fox_Httpsd: bind() on port %d failed\n", g_port);
		close(fd);
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
		return false;
	}
	if (listen(fd, 8) != 0) {
		LOGERR("Fox_Httpsd: listen() failed\n");
		close(fd);
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
		return false;
	}

	g_listen_fd = fd;
	g_running = true;

	if (pthread_create(&g_accept_thread, nullptr, accept_thread, nullptr) != 0) {
		LOGERR("Fox_Httpsd: failed to start accept thread\n");
		g_running = false;
		close(fd);
		g_listen_fd = -1;
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
		return false;
	}

	LOGINFO("Fox_Httpsd: listening on %s:%d (TLS 1.2+)\n",
	        Fox_Httpd::LocalAddress().c_str(), g_port);
	return true;
}

void Fox_Httpsd::Stop()
{
	if (!g_running)
		return;
	g_running = false;
	if (g_listen_fd >= 0) {
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	pthread_join(g_accept_thread, nullptr);
	if (g_ctx) {
		SSL_CTX_free(g_ctx);
		g_ctx = nullptr;
	}
}

bool Fox_Httpsd::IsRunning()
{
	return g_running;
}

int Fox_Httpsd::Port()
{
	return g_port;
}

#else  // !OF_ENABLE_WLAN

// Stubs so the symbols resolve in non-WLAN builds (Fox_Httpsd is only ever
// called from Cmd_Https, which is itself #ifdef-gated, but keep these for
// link safety).
bool Fox_Httpsd::Start(int) { return false; }
void Fox_Httpsd::Stop() {}
bool Fox_Httpsd::IsRunning() { return false; }
int Fox_Httpsd::Port() { return 443; }

#endif // OF_ENABLE_WLAN
