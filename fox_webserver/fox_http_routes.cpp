/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_http_routes.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "fox_http_api.hpp"
#include "fox_http_common.hpp"
#include "fox_http_screen.hpp"
#include "fox_http_static.hpp"
#include "fox_httpd.hpp"

namespace {

// Length-independent constant-time string comparison to avoid leaking the
// token/password through response timing.
bool ct_equal(const std::string& a, const std::string& b)
{
	unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
	size_t n = a.size() > b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i) {
		unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
		unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
		diff |= static_cast<unsigned char>(ca ^ cb);
	}
	return diff == 0;
}

bool authorized(const Fox_Http::Request& req)
{
	std::string token = Fox_Httpd::Token();
	if (token.empty())
		return false;
	if (ct_equal(Fox_Http::Param(req.query, "token"), token))
		return true;
	if (ct_equal(Fox_Http::HeaderValue(req.headers, "X-Auth-Token"), token))
		return true;
	std::string bearer = Fox_Http::HeaderValue(req.headers, "Authorization");
	return ct_equal(bearer, "Bearer " + token);
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

void read_body(Fox_Io::Conn& conn, Fox_Http::Request& req, const std::string& leftover)
{
	static const size_t kMaxBody = 1 * 1024 * 1024;  // 1 MiB hard cap
	std::string cl = Fox_Http::HeaderValue(req.headers, "Content-Length");
	if (cl.empty())
		return;
	size_t need = (size_t)strtoul(cl.c_str(), nullptr, 10);
	if (need > kMaxBody)
		need = kMaxBody;  // never read (or trust) more than the cap, whatever is advertised
	req.body = leftover;
	char tmp[4096];
	while (req.body.size() < need) {
		ssize_t n = conn.recv(tmp, sizeof(tmp));
		if (n <= 0)
			break;
		req.body.append(tmp, n);
		if (req.body.size() >= kMaxBody)
			break;
	}
}

// When Fox_Httpd redirect mode is on, plaintext requests are answered with a
// 301 to the HTTPS endpoint. Only meaningful for non-TLS connections.
void send_redirect_to_https(Fox_Io::Conn& conn, const Fox_Http::Request& req)
{
	char loc[512];
	std::string host = Fox_Http::HeaderValue(req.headers, "Host");
	std::string addr = Fox_Httpd::LocalAddress();
	int https_port = Fox_Httpd::HttpsPortForRedirect();
	// Strip any :port from the Host header so we don't end up with host:80:443.
	if (!host.empty()) {
		size_t c = host.find(':');
		if (c != std::string::npos)
			host = host.substr(0, c);
	}
	// The Host header is attacker-controlled. Only echo it back into the redirect
	// when it is a plain hostname/IP (letters, digits, '.', '-'); otherwise fall
	// back to the server's own address so a crafted Host can't steer the redirect.
	if (!host.empty() &&
	    host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
		host.clear();
	if (host.empty())
		host = addr;
	std::string port_part = (https_port == 443) ? "" : (":" + std::to_string(https_port));
	snprintf(loc, sizeof(loc), "https://%s%s%s",
	         host.c_str(), port_part.c_str(), req.path.c_str());

	std::string hdr;
	hdr += "HTTP/1.1 301 Moved Permanently\r\nLocation: ";
	hdr += loc;
	hdr += "\r\nContent-Length: 0\r\nConnection: close\r\n";
	hdr += Fox_Http::CorsHeaderBlock();
	hdr += "\r\n";
	Fox_Http::SendAll(conn, hdr);
}

} // namespace

void Fox_Http::HandleConnection(Fox_Io::Conn conn)
{
	Request req;
	std::string leftover;
	if (!ReadHeaders(conn, req.headers, leftover))
		return;
	if (!parse_request_line(req))
		return;
	read_body(conn, req, leftover);

	// Resolve the cross-origin policy once for this connection so every reply
	// (preflight, redirect, static, API) carries a consistent CORS header.
	SetCorsForRequest(Fox_Httpd::ResolveCorsOrigin(HeaderValue(req.headers, "Origin")));

	if (req.method == "OPTIONS") {
		SendCorsOptions(conn);
		return;
	}

	// Redirect mode: plaintext (non-TLS) connections get a 301 to HTTPS and
	// nothing else. No auth required — the redirect is public metadata.
	if (!conn.is_tls() && Fox_Httpd::RedirectEnabled()) {
		send_redirect_to_https(conn, req);
		return;
	}

	if (req.method == "GET" && req.path.compare(0, 5, "/api/") != 0 &&
	    req.path != "/screen.png" && req.path != "/stream.mjpeg") {
		if (!ServeStatic(conn, req.path))
			Send404(conn);
		return;
	}

	if (!authorized(req)) {
		Send401(conn);
		return;
	}

	if (req.method == "GET" && req.path == "/api/status") {
		HandleStatus(conn);
	} else if (req.method == "POST" && req.path == "/api/cmd") {
		HandleCmd(conn, req);
	} else if (req.method == "GET" && req.path == "/api/jobs") {
		HandleJobList(conn);
	} else if (req.method == "GET" && req.path.compare(0, 10, "/api/jobs/") == 0) {
		std::string rest = req.path.substr(10);
		size_t slash = rest.find('/');
		int id = atoi(rest.c_str());
		HandleJobRoute(conn, req, id, slash != std::string::npos && rest.substr(slash) == "/output");
	} else if (req.method == "GET" && req.path == "/screen.png") {
		if (!Fox_Httpd::ControlEnabled()) Send403(conn);
		else HandleScreenPng(conn);
	} else if (req.method == "GET" && req.path == "/stream.mjpeg") {
		if (!Fox_Httpd::ControlEnabled()) Send403(conn);
		else HandleStream(conn);
	} else if (req.method == "POST" && req.path == "/api/input/touch") {
		if (!Fox_Httpd::ControlEnabled()) Send403(conn);
		else HandleTouch(conn, req);
	} else if (req.method == "POST" && req.path == "/api/input/key") {
		if (!Fox_Httpd::ControlEnabled()) Send403(conn);
		else HandleKey(conn, req);
	} else {
		Send404(conn);
	}
}
