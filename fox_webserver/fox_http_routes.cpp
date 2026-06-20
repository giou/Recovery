/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_http_routes.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "fox_http_api.hpp"
#include "fox_http_common.hpp"
#include "fox_http_screen.hpp"
#include "fox_http_static.hpp"
#include "fox_httpd.hpp"

namespace {

bool authorized(const Fox_Http::Request& req)
{
	std::string token = Fox_Httpd::Token();
	if (token.empty())
		return false;
	if (Fox_Http::Param(req.query, "token") == token)
		return true;
	if (Fox_Http::HeaderValue(req.headers, "X-Auth-Token") == token)
		return true;
	std::string bearer = Fox_Http::HeaderValue(req.headers, "Authorization");
	return bearer == "Bearer " + token;
}

bool parse_request_line(Fox_Http::Request& req)
{
	size_t eol = req.headers.find("\r\n");
	if (eol == std::string::npos)
		return false;
	std::string line = req.headers.substr(0, eol);
	size_t sp1 = line.find(' ');
	size_t sp2 = line.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos)
		return false;
	req.method = line.substr(0, sp1);
	std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t q = target.find('?');
	if (q == std::string::npos) {
		req.path = target;
	} else {
		req.path = target.substr(0, q);
		req.query = target.substr(q + 1);
	}
	req.headers = req.headers.substr(eol + 2);
	return true;
}

void read_body(int fd, Fox_Http::Request& req, const std::string& leftover)
{
	std::string cl = Fox_Http::HeaderValue(req.headers, "Content-Length");
	if (cl.empty())
		return;
	size_t need = (size_t)strtoul(cl.c_str(), nullptr, 10);
	req.body = leftover;
	char tmp[4096];
	while (req.body.size() < need) {
		ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
		if (n <= 0)
			break;
		req.body.append(tmp, n);
		if (req.body.size() > 1 * 1024 * 1024)
			break;
	}
}

} // namespace

void Fox_Http::HandleConnection(int fd)
{
	Request req;
	std::string leftover;
	if (!ReadHeaders(fd, req.headers, leftover)) {
		close(fd);
		return;
	}
	if (!parse_request_line(req)) {
		close(fd);
		return;
	}
	read_body(fd, req, leftover);

	if (req.method == "OPTIONS") {
		SendCorsOptions(fd);
		close(fd);
		return;
	}

	if (req.method == "GET" && req.path.compare(0, 5, "/api/") != 0 &&
	    req.path != "/screen.png" && req.path != "/stream.mjpeg") {
		if (!ServeStatic(fd, req.path))
			Send404(fd);
		close(fd);
		return;
	}

	if (!authorized(req)) {
		Send401(fd);
		close(fd);
		return;
	}

	if (req.method == "GET" && req.path == "/api/status") {
		HandleStatus(fd);
	} else if (req.method == "POST" && req.path == "/api/cmd") {
		HandleCmd(fd, req);
	} else if (req.method == "GET" && req.path == "/api/jobs") {
		HandleJobList(fd);
	} else if (req.method == "GET" && req.path.compare(0, 10, "/api/jobs/") == 0) {
		std::string rest = req.path.substr(10);
		size_t slash = rest.find('/');
		int id = atoi(rest.c_str());
		HandleJobRoute(fd, req, id, slash != std::string::npos && rest.substr(slash) == "/output");
	} else if (req.method == "GET" && req.path == "/screen.png") {
		if (!Fox_Httpd::ControlEnabled()) Send403(fd);
		else HandleScreenPng(fd);
	} else if (req.method == "GET" && req.path == "/stream.mjpeg") {
		if (!Fox_Httpd::ControlEnabled()) Send403(fd);
		else HandleStream(fd);
	} else if (req.method == "POST" && req.path == "/api/input/touch") {
		if (!Fox_Httpd::ControlEnabled()) Send403(fd);
		else HandleTouch(fd, req);
	} else if (req.method == "POST" && req.path == "/api/input/key") {
		if (!Fox_Httpd::ControlEnabled()) Send403(fd);
		else HandleKey(fd, req);
	} else {
		Send404(fd);
	}
	close(fd);
}
