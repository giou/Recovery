#include "wpa_ctrl_client.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// SOCK_DGRAM reply buffer. One recv() == one datagram, and any bytes past the
// buffer are silently dropped, so this must be large enough for the biggest
// reply we care about (SCAN_RESULTS with many APs). 8 KiB comfortably exceeds
// wpa_cli's traditional 4 KiB.
const size_t kReplyBufSize = 8192;

// Unique suffix per client socket so concurrent channels never collide.
std::atomic<unsigned> g_seq{0};

long NowMs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Wait until `fd` is readable or the deadline (absolute ms) passes.
bool WaitReadable(int fd, long deadline_ms) {
	long now = NowMs();
	if (now >= deadline_ms)
		return false;

	long remaining = deadline_ms - now;
	struct timeval tv;
	tv.tv_sec = remaining / 1000;
	tv.tv_usec = (remaining % 1000) * 1000;

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
	return r > 0 && FD_ISSET(fd, &rfds);
}

// Unsolicited event messages are prefixed with a priority ("<3>") or an
// interface tag ("IFNAME=wlan0 <3>..."). A command reply never is.
bool LooksLikeEvent(const char* buf, ssize_t len) {
	if (len <= 0)
		return false;
	if (buf[0] == '<')
		return true;
	if (len >= 7 && std::strncmp(buf, "IFNAME=", 7) == 0)
		return true;
	return false;
}

}  // namespace

WpaCtrl::WpaCtrl() : mFd(-1) {}

WpaCtrl::~WpaCtrl() {
	Close();
}

bool WpaCtrl::Open(const std::string& server_path) {
	Close();

	int s = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (s < 0)
		return false;

	struct sockaddr_un local;
	std::memset(&local, 0, sizeof(local));
	local.sun_family = AF_UNIX;
	std::snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/fox_wpa_ctrl_%d_%u",
	              (int)getpid(), g_seq.fetch_add(1));
	unlink(local.sun_path);

	if (bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
		close(s);
		return false;
	}

	struct sockaddr_un dest;
	std::memset(&dest, 0, sizeof(dest));
	dest.sun_family = AF_UNIX;
	if (server_path.size() >= sizeof(dest.sun_path)) {
		close(s);
		unlink(local.sun_path);
		return false;
	}
	std::strncpy(dest.sun_path, server_path.c_str(), sizeof(dest.sun_path) - 1);

	if (connect(s, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
		close(s);
		unlink(local.sun_path);
		return false;
	}

	mFd = s;
	mLocalPath = local.sun_path;
	return true;
}

void WpaCtrl::Close() {
	if (mFd >= 0) {
		close(mFd);
		mFd = -1;
	}
	if (!mLocalPath.empty()) {
		unlink(mLocalPath.c_str());
		mLocalPath.clear();
	}
}

bool WpaCtrl::Request(const std::string& cmd, std::string& reply, int timeout_ms) {
	if (mFd < 0)
		return false;

	if (send(mFd, cmd.c_str(), cmd.size(), 0) < 0)
		return false;

	long deadline = NowMs() + timeout_ms;
	for (;;) {
		if (!WaitReadable(mFd, deadline))
			return false;

		char buf[kReplyBufSize];
		ssize_t len = recv(mFd, buf, sizeof(buf) - 1, 0);
		if (len < 0)
			return false;

		// If this channel is also attached as a monitor, an unsolicited event
		// may arrive between the request and its reply; skip those.
		if (LooksLikeEvent(buf, len))
			continue;

		reply.assign(buf, len);
		return true;
	}
}

bool WpaCtrl::Attach() {
	std::string reply;
	if (!Request("ATTACH", reply, 2000))
		return false;
	return reply.compare(0, 2, "OK") == 0;
}

bool WpaCtrl::Detach() {
	std::string reply;
	return Request("DETACH", reply, 1000);
}

bool WpaCtrl::WaitEvent(const std::vector<std::string>& any_of, int timeout_ms,
                        std::string& matched) {
	if (mFd < 0)
		return false;

	long deadline = NowMs() + timeout_ms;
	for (;;) {
		if (!WaitReadable(mFd, deadline))
			return false;

		char buf[kReplyBufSize];
		ssize_t len = recv(mFd, buf, sizeof(buf) - 1, 0);
		if (len < 0)
			return false;
		buf[len] = '\0';

		std::string ev(buf, len);
		for (const std::string& needle : any_of) {
			if (ev.find(needle) != std::string::npos) {
				matched = ev;
				return true;
			}
		}
		// keep waiting until the deadline for a matching event
	}
}
