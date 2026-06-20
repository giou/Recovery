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

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "fox_http_routes.hpp"
#include "../fox_fifo/fox_remote_input.hpp"
#include "../twcommon.h"

namespace {

volatile bool g_running = false;
int g_listen_fd = -1;
int g_port = 80;
std::string g_token;
pthread_t g_accept_thread;
volatile bool g_control_enabled = true;

struct ConnArg {
	int fd;
};

void* connection_thread(void* arg)
{
	ConnArg* ca = static_cast<ConnArg*>(arg);
	int fd = ca->fd;
	delete ca;
	Fox_Http::HandleConnection(fd);
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

std::string make_token()
{
	static const char* hex = "0123456789abcdef";
	std::string t;
	FILE* f = fopen("/dev/urandom", "rb");
	unsigned char raw[16];
	if (f && fread(raw, 1, sizeof(raw), f) == sizeof(raw)) {
		for (unsigned char b : raw) {
			t += hex[b >> 4];
			t += hex[b & 0xF];
		}
	}
	if (f)
		fclose(f);
	if (t.empty()) {
		srand((unsigned)time(nullptr));
		for (int i = 0; i < 32; i++)
			t += hex[rand() & 0xF];
	}
	return t;
}

}  // namespace

bool Fox_Httpd::Start(int port)
{
	if (g_running)
		return true;

	g_port = port > 0 ? port : 80;
	g_token = make_token();

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

void Fox_Httpd::SetControlEnabled(bool on)
{
	g_control_enabled = on;
}

bool Fox_Httpd::ControlEnabled()
{
	return g_control_enabled;
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
