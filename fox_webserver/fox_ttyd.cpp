/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_ttyd.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../fox_secrets/fox_secrets.hpp"
#include "../twcommon.h"

namespace {

pid_t g_pid = -1;
int g_port = 7681;
std::string g_last_error;
const char* kLogPath = "/tmp/fox_ttyd.log";

bool file_exists(const char* path)
{
	return access(path, X_OK) == 0;
}

const char* ttyd_path()
{
	if (file_exists("/system/bin/ttyd"))
		return "/system/bin/ttyd";
	if (file_exists("/sbin/ttyd"))
		return "/sbin/ttyd";
	if (file_exists("/vendor/bin/ttyd"))
		return "/vendor/bin/ttyd";
	if (file_exists("/tmp/ttyd"))
		return "/tmp/ttyd";
	return nullptr;
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

std::string read_log_tail()
{
	std::ifstream in(kLogPath);
	if (!in.is_open())
		return "";

	std::string line;
	std::string out;
	while (std::getline(in, line)) {
		out += line;
		out += '\n';
		if (out.size() > 2048)
			out.erase(0, out.size() - 2048);
	}
	return out;
}

std::string status_text(int status)
{
	std::ostringstream out;
	if (WIFEXITED(status))
		out << "exit " << WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		out << "signal " << WTERMSIG(status);
	else
		out << "status " << status;
	return out.str();
}

void set_error(const std::string& error)
{
	g_last_error = error;
	LOGERR("Fox_Ttyd: %s\n", error.c_str());
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
	const char* ttyd = ttyd_path();
	if (!ttyd) {
		set_error("ttyd binary not found in /system/bin, /sbin, /vendor/bin, or /tmp");
		return false;
	}

	// Secure by default: refuse to expose a network shell without a password.
	// The shared remote password is set via `foxapi remote password <pw>`.
	std::string password;
	if (!Fox_Secrets::GetRemotePassword(password) || password.empty()) {
		set_error("ttyd not started: no remote password set "
		          "(run: foxapi remote password <password>)");
		return false;
	}
	// HTTP basic-auth credential. Passed to the child through the environment
	// (TTYD_CREDENTIAL) rather than the -c argv, so the password is not exposed
	// via /proc/<pid>/cmdline to other processes.
	std::string cred = "fox:" + password;

	g_last_error.clear();
	unlink(kLogPath);

	pid_t pid = fork();
	if (pid < 0) {
		set_error(std::string("fork() failed: ") + strerror(errno));
		return false;
	}
	if (pid == 0) {
		setsid();
		int log_fd = open(kLogPath, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
		if (log_fd >= 0) {
			dup2(log_fd, STDOUT_FILENO);
			dup2(log_fd, STDERR_FILENO);
			close(log_fd);
		}
		// Hand the credential to ttyd out-of-band (kept off the argv/cmdline).
		setenv("TTYD_CREDENTIAL", cred.c_str(), 1);
		execl(ttyd, "ttyd", "-i", "0.0.0.0", "-p", port_s.c_str(),
		      "-W", sh, (char*)nullptr);
		_exit(127);
	}

	g_pid = pid;
	for (int i = 0; i < 10; i++) {
		int status = 0;
		pid_t rc = waitpid(g_pid, &status, WNOHANG);
		if (rc == g_pid) {
			g_pid = -1;
			std::string log = read_log_tail();
			std::string error = "ttyd exited during startup (" + status_text(status) + ")";
			if (!log.empty())
				error += ": " + log;
			set_error(error);
			return false;
		}
		if (rc < 0 && errno != EINTR) {
			set_error(std::string("waitpid() failed: ") + strerror(errno));
			g_pid = -1;
			return false;
		}
		usleep(50 * 1000);
	}
	LOGINFO("Fox_Ttyd: %s listening on port %d with shell %s\n", ttyd, g_port, sh);
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

std::string Fox_Ttyd::LastError()
{
	return g_last_error;
}
