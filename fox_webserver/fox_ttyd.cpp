/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_ttyd.hpp"

#include <errno.h>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../twcommon.h"

namespace {

pid_t g_pid = -1;
int g_port = 7681;

bool file_exists(const char* path)
{
	return access(path, X_OK) == 0;
}

const char* shell_path()
{
	if (file_exists("/sbin/sh"))
		return "/sbin/sh";
	if (file_exists("/system/bin/sh"))
		return "/system/bin/sh";
	return "sh";
}

bool process_alive(pid_t pid)
{
	if (pid <= 0)
		return false;
	if (kill(pid, 0) == 0)
		return true;
	return errno == EPERM;
}

void reap_if_dead()
{
	if (g_pid <= 0)
		return;
	int status = 0;
	pid_t rc = waitpid(g_pid, &status, WNOHANG);
	if (rc == g_pid)
		g_pid = -1;
}

} // namespace

bool Fox_Ttyd::Start(int port)
{
	reap_if_dead();
	if (IsRunning())
		return true;

	if (port <= 0)
		port = 7681;
	g_port = port;
	std::string port_s = std::to_string(g_port);
	const char* sh = shell_path();

	pid_t pid = fork();
	if (pid < 0) {
		LOGERR("Fox_Ttyd: fork() failed\n");
		return false;
	}
	if (pid == 0) {
		setsid();
		execlp("ttyd", "ttyd", "-i", "0.0.0.0", "-p", port_s.c_str(), "-W", sh, (char*)nullptr);
		execl("/system/bin/ttyd", "ttyd", "-i", "0.0.0.0", "-p", port_s.c_str(), "-W", sh, (char*)nullptr);
		execl("/vendor/bin/ttyd", "ttyd", "-i", "0.0.0.0", "-p", port_s.c_str(), "-W", sh, (char*)nullptr);
		execl("/sbin/ttyd", "ttyd", "-i", "0.0.0.0", "-p", port_s.c_str(), "-W", sh, (char*)nullptr);
		_exit(127);
	}

	g_pid = pid;
	usleep(100 * 1000);
	reap_if_dead();
	if (!IsRunning()) {
		LOGERR("Fox_Ttyd: ttyd failed to start\n");
		return false;
	}
	LOGINFO("Fox_Ttyd: listening on port %d with shell %s\n", g_port, sh);
	return true;
}

void Fox_Ttyd::Stop()
{
	reap_if_dead();
	if (g_pid <= 0)
		return;
	kill(g_pid, SIGTERM);
	for (int i = 0; i < 20; i++) {
		int status = 0;
		pid_t rc = waitpid(g_pid, &status, WNOHANG);
		if (rc == g_pid) {
			g_pid = -1;
			return;
		}
		usleep(50 * 1000);
	}
	kill(g_pid, SIGKILL);
	waitpid(g_pid, nullptr, 0);
	g_pid = -1;
}

bool Fox_Ttyd::IsRunning()
{
	reap_if_dead();
	return process_alive(g_pid);
}

int Fox_Ttyd::Port()
{
	return g_port;
}

std::string Fox_Ttyd::Shell()
{
	return shell_path();
}
