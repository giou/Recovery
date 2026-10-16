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

#include "fox_httpd.hpp"

#include <openssl/rand.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fox_http_routes.hpp"
#include "fox_io.hpp"
#include "../fox_fifo/fox_remote_input.hpp"
#include "../fox_secrets/fox_secrets.hpp"
#include "../twcommon.h"

namespace {

volatile bool g_running = false;
int g_listen_fd = -1;
int g_port = 80;
std::string g_token;
bool g_token_is_password = false;
pthread_t g_accept_thread;
volatile bool g_control_enabled = true;
volatile bool g_redirect_enabled = false;
int g_https_redirect_port = 443;

// CORS allow-list override. Empty = default policy (device-local origins plus
// the official orangefox.tech / orangefox.download domains). A non-empty value
// is a developer override set via `webserver cors set <origin>` ("*" allows any
// origin). In-memory only — it is deliberately not persisted across reboots.
pthread_mutex_t g_cors_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_cors_override;

// Extract the host from an Origin header value ("scheme://host[:port]").
std::string origin_host(const std::string& origin)
{
	size_t s = origin.find("://");
	std::string rest = (s == std::string::npos) ? origin : origin.substr(s + 3);
	size_t slash = rest.find('/');
	if (slash != std::string::npos)
		rest = rest.substr(0, slash);
	size_t colon = rest.rfind(':');
	if (colon != std::string::npos)
		rest = rest.substr(0, colon);
	return rest;
}

bool host_is_orangefox(const std::string& host, const char* domain)
{
	if (host == domain)
		return true;
	std::string suffix = std::string(".") + domain;
	return host.size() > suffix.size() &&
	       host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool host_allowed_default(const std::string& host)
{
	if (host.empty())
		return false;
	if (host == "localhost" || host == "127.0.0.1")
		return true;
	if (host == Fox_Httpd::LocalAddress())
		return true;
	return host_is_orangefox(host, "orangefox.tech") ||
	       host_is_orangefox(host, "orangefox.download");
}

// Persisted redirect preference: a one-byte flag file. Absent = off.
const char* kRedirectFlag = "/data/media/0/Fox/tls/redirect.enabled";

bool read_redirect_flag()
{
	std::ifstream f(kRedirectFlag);
	if (!f)
		return false;
	int v = 0;
	f >> v;
	return v == 1;
}

void write_redirect_flag(bool on)
{
	mkdir("/data/media/0/Fox", 0700);
	mkdir("/data/media/0/Fox/tls", 0700);
	std::string tmp = std::string(kRedirectFlag) + ".tmp";
	{
		std::ofstream ofs(tmp);
		if (!ofs)
			return;
		ofs << (on ? 1 : 0);
	}
	chmod(tmp.c_str(), 0600);
	rename(tmp.c_str(), kRedirectFlag);
	chmod(kRedirectFlag, 0600);
}

struct ConnArg {
	int fd;
};

void* connection_thread(void* arg)
{
	ConnArg* ca = static_cast<ConnArg*>(arg);
	int fd = ca->fd;
	delete ca;
	// Wrap the plaintext fd in a Fox_Io::Conn; HandleConnection closes it.
	Fox_Io::Conn conn(fd);
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
			// Back off briefly so a persistent error (e.g. EMFILE under fd
			// exhaustion) does not spin the accept loop at 100% CPU.
			usleep(20 * 1000);
			continue;
		}
		int one = 1;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		// Bound blocking reads/writes so a stalled peer can't pin this thread.
		Fox_Io::SetTimeouts(cfd, 30);

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

// Returns a 128-bit random token as lowercase hex, or "" if no cryptographic
// entropy source is available. A predictable token would gate root access, so
// we never fall back to a weak PRNG — the caller refuses to start instead.
std::string make_token()
{
	static const char* hex = "0123456789abcdef";
	unsigned char raw[16];
	bool have = false;

	FILE* f = fopen("/dev/urandom", "rb");
	if (f) {
		have = fread(raw, 1, sizeof(raw), f) == sizeof(raw);
		fclose(f);
	}
	// Fall back to BoringSSL's CSPRNG (libcrypto is already linked) rather than
	// rand(), which is trivially predictable from the boot time.
	if (!have)
		have = RAND_bytes(raw, sizeof(raw)) == 1;
	if (!have)
		return "";

	std::string t;
	for (unsigned char b : raw) {
		t += hex[b >> 4];
		t += hex[b & 0xF];
	}
	return t;
}

}  // namespace

bool Fox_Httpd::Start(int port)
{
	if (g_running)
		return true;

	g_port = port > 0 ? port : 80;

	// Prefer the user-configured remote password (shared with ttyd) as the
	// auth token. Fall back to a random per-boot token when none is set so the
	// endpoint is never left unauthenticated.
	std::string configured;
	bool from_password = false;
	if (Fox_Secrets::GetRemotePassword(configured) && !configured.empty()) {
		g_token = configured;
		from_password = true;
	} else {
		g_token = make_token();
		if (g_token.empty()) {
			LOGERR("Fox_Httpd: no entropy source for auth token; refusing to start\n");
			return false;
		}
	}
	g_token_is_password = from_password;

	// Pick up the persisted redirect preference.
	g_redirect_enabled = read_redirect_flag();

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		LOGERR("Fox_Httpd: socket() failed\n");
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
		LOGERR("Fox_Httpd: bind() on port %d failed\n", g_port);
		close(fd);
		return false;
	}
	if (listen(fd, 8) != 0) {
		LOGERR("Fox_Httpd: listen() failed\n");
		close(fd);
		return false;
	}

	g_listen_fd = fd;
	g_running = true;

	Fox_Remote_Input::Init();

	if (pthread_create(&g_accept_thread, nullptr, accept_thread, nullptr) != 0) {
		LOGERR("Fox_Httpd: failed to start accept thread\n");
		g_running = false;
		close(fd);
		g_listen_fd = -1;
		return false;
	}

	// Never log a user-configured password; only the random fallback token.
	if (from_password)
		LOGINFO("Fox_Httpd: listening on %s:%d (auth: configured remote password)\n",
		        LocalAddress().c_str(), g_port);
	else
		LOGINFO("Fox_Httpd: listening on %s:%d (token %s)\n",
		        LocalAddress().c_str(), g_port, g_token.c_str());
	return true;
}

void Fox_Httpd::Stop()
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
	Fox_Remote_Input::Shutdown();
	g_token.clear();
}

bool Fox_Httpd::IsRunning()
{
	return g_running;
}

int Fox_Httpd::Port()
{
	return g_port;
}

std::string Fox_Httpd::Token()
{
	return g_token;
}

bool Fox_Httpd::AuthIsPassword()
{
	return g_token_is_password;
}

void Fox_Httpd::SetControlEnabled(bool on)
{
	g_control_enabled = on;
}

bool Fox_Httpd::ControlEnabled()
{
	return g_control_enabled;
}

void Fox_Httpd::SetRedirectEnabled(bool on)
{
	g_redirect_enabled = on;
	write_redirect_flag(on);
}

bool Fox_Httpd::RedirectEnabled()
{
	return g_redirect_enabled;
}

int Fox_Httpd::HttpsPortForRedirect()
{
	return g_https_redirect_port;
}

void Fox_Httpd::SetHttpsPortForRedirect(int port)
{
	if (port > 0)
		g_https_redirect_port = port;
}

void Fox_Httpd::SetCorsOverride(const std::string& origin)
{
	pthread_mutex_lock(&g_cors_lock);
	g_cors_override = origin;
	pthread_mutex_unlock(&g_cors_lock);
}

std::string Fox_Httpd::CorsOverride()
{
	pthread_mutex_lock(&g_cors_lock);
	std::string v = g_cors_override;
	pthread_mutex_unlock(&g_cors_lock);
	return v;
}

std::string Fox_Httpd::ResolveCorsOrigin(const std::string& request_origin)
{
	std::string override_origin = CorsOverride();
	// Developer override: "*" echoes a wildcard; any other value is an extra
	// exact-match origin added to the default allow-list.
	if (override_origin == "*")
		return "*";
	if (!override_origin.empty() && request_origin == override_origin)
		return request_origin;

	// Same-origin requests carry no Origin header and need no CORS header.
	if (request_origin.empty())
		return "";

	if (host_allowed_default(origin_host(request_origin)))
		return request_origin;
	return "";
}

std::string Fox_Httpd::LocalAddress()
{
	struct ifaddrs* ifaddr = nullptr;
	std::string result = "0.0.0.0";
	if (getifaddrs(&ifaddr) != 0)
		return result;
	for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		char buf[INET_ADDRSTRLEN] = { 0 };
		struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
		inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
		if (buf[0]) {
			result = buf;
			break;
		}
	}
	freeifaddrs(ifaddr);
	return result;
}
