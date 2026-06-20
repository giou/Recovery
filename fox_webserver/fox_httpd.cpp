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

#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "fox_httpd.hpp"
#include "fox_jobs.hpp"
#include "../fox_fifo/fox_screen.hpp"
#include "../fox_fifo/fox_input.hpp"
#include "../twcommon.h"

namespace {

// Root of the optional static dashboard bundle on the ramdisk. The build packs
// a web bundle here only when FOX_REMOTE_DASHBOARD=1 (see orangefox.mk); if the
// directory is absent the server runs API-only.
#ifndef FOX_DASHBOARD_DIR
#define FOX_DASHBOARD_DIR "/system/etc/fox/dashboard"
#endif

volatile bool g_running = false;
int g_listen_fd = -1;
int g_port = 8080;
std::string g_token;
pthread_t g_accept_thread;
// Remote-control plane (screen + input) gate; independent of g_running.
volatile bool g_control_enabled = true;

// ---- low-level IO ---------------------------------------------------------

bool send_all(int fd, const char* data, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		off += (size_t)n;
	}
	return true;
}

bool send_all(int fd, const std::string& s) {
	return send_all(fd, s.data(), s.size());
}

// Read until "\r\n\r\n"; returns the header block (without the blank line) and
// leaves any already-read body bytes in 'leftover'. Returns false on error.
bool read_headers(int fd, std::string& headers, std::string& leftover) {
	std::string buf;
	char tmp[2048];
	while (buf.find("\r\n\r\n") == std::string::npos) {
		ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
		if (n <= 0)
			return false;
		buf.append(tmp, n);
		if (buf.size() > 64 * 1024)
			return false;  // header flood guard
	}
	size_t pos = buf.find("\r\n\r\n");
	headers = buf.substr(0, pos);
	leftover = buf.substr(pos + 4);
	return true;
}

// ---- parsing helpers ------------------------------------------------------

std::string url_decode(const std::string& in) {
	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '%' && i + 2 < in.size()) {
			int hi = isxdigit(in[i + 1]) ? (isdigit(in[i + 1]) ? in[i + 1] - '0' : tolower(in[i + 1]) - 'a' + 10) : -1;
			int lo = isxdigit(in[i + 2]) ? (isdigit(in[i + 2]) ? in[i + 2] - '0' : tolower(in[i + 2]) - 'a' + 10) : -1;
			if (hi >= 0 && lo >= 0) {
				out += (char)(hi * 16 + lo);
				i += 2;
				continue;
			}
		}
		if (in[i] == '+')
			out += ' ';
		else
			out += in[i];
	}
	return out;
}

std::string header_value(const std::string& headers, const std::string& name) {
	// Case-insensitive line search for "name:".
	std::string lower_headers = headers;
	for (char& c : lower_headers) c = tolower(c);
	std::string key = name;
	for (char& c : key) c = tolower(c);
	key += ":";
	size_t pos = lower_headers.find("\n" + key);
	size_t start;
	if (pos == std::string::npos) {
		if (lower_headers.compare(0, key.size(), key) == 0)
			start = key.size();
		else
			return "";
	} else {
		start = pos + 1 + key.size();
	}
	size_t end = headers.find("\r\n", start);
	if (end == std::string::npos)
		end = headers.size();
	std::string val = headers.substr(start, end - start);
	size_t b = val.find_first_not_of(" \t");
	if (b == std::string::npos)
		return "";
	size_t e = val.find_last_not_of(" \t\r");
	return val.substr(b, e - b + 1);
}

// Extract a query/form parameter from a "a=1&b=2" string.
std::string param(const std::string& qs, const std::string& key) {
	size_t i = 0;
	while (i < qs.size()) {
		size_t amp = qs.find('&', i);
		std::string pair = qs.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
		size_t eq = pair.find('=');
		std::string k = eq == std::string::npos ? pair : pair.substr(0, eq);
		if (k == key)
			return eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
		if (amp == std::string::npos)
			break;
		i = amp + 1;
	}
	return "";
}

// Minimal flat-JSON helpers for our own controlled client.
std::string json_string(const std::string& body, const std::string& key) {
	std::string needle = "\"" + key + "\"";
	size_t k = body.find(needle);
	if (k == std::string::npos)
		return "";
	size_t colon = body.find(':', k + needle.size());
	if (colon == std::string::npos)
		return "";
	size_t q1 = body.find('"', colon);
	if (q1 == std::string::npos)
		return "";
	std::string out;
	for (size_t i = q1 + 1; i < body.size(); i++) {
		if (body[i] == '\\' && i + 1 < body.size()) {
			char c = body[++i];
			switch (c) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				default:  out += c;    break;
			}
			continue;
		}
		if (body[i] == '"')
			break;
		out += body[i];
	}
	return out;
}

bool json_int(const std::string& body, const std::string& key, long& out) {
	std::string needle = "\"" + key + "\"";
	size_t k = body.find(needle);
	if (k == std::string::npos)
		return false;
	size_t colon = body.find(':', k + needle.size());
	if (colon == std::string::npos)
		return false;
	size_t i = colon + 1;
	while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
		i++;
	bool neg = false;
	if (i < body.size() && (body[i] == '-' || body[i] == '+')) {
		neg = body[i] == '-';
		i++;
	}
	if (i >= body.size() || !isdigit(body[i]))
		return false;
	long v = 0;
	while (i < body.size() && isdigit(body[i]))
		v = v * 10 + (body[i++] - '0');
	out = neg ? -v : v;
	return true;
}

// Parse {"argv":["a","b"]} into a vector of strings.
std::vector<std::string> json_argv(const std::string& body) {
	std::vector<std::string> out;
	size_t k = body.find("\"argv\"");
	if (k == std::string::npos)
		return out;
	size_t lb = body.find('[', k);
	size_t rb = body.find(']', lb == std::string::npos ? k : lb);
	if (lb == std::string::npos || rb == std::string::npos)
		return out;
	size_t i = lb + 1;
	while (i < rb) {
		size_t q1 = body.find('"', i);
		if (q1 == std::string::npos || q1 >= rb)
			break;
		std::string s;
		size_t j = q1 + 1;
		for (; j < body.size(); j++) {
			if (body[j] == '\\' && j + 1 < body.size()) {
				s += body[++j];
				continue;
			}
			if (body[j] == '"')
				break;
			s += body[j];
		}
		out.push_back(s);
		i = j + 1;
	}
	return out;
}

// ---- HTTP responses -------------------------------------------------------

void send_response(int fd, const char* status, const char* ctype, const std::string& body) {
	char hdr[512];
	// Permissive CORS so the dashboard can be hosted off-device and target a
	// device by IP. Endpoints still require the session token, so '*' only
	// widens reachability, not authority.
	int n = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n",
		status, ctype, body.size());
	if (n > 0)
		send_all(fd, hdr, (size_t)n);
	send_all(fd, body);
}

void send_json(int fd, const std::string& json) {
	send_response(fd, "200 OK", "application/json", json);
}

void send_404(int fd) {
	send_response(fd, "404 Not Found", "text/plain", "not found\n");
}

void send_401(int fd) {
	send_response(fd, "401 Unauthorized", "text/plain", "unauthorized\n");
}

void send_403(int fd) {
	send_response(fd, "403 Forbidden", "text/plain", "remote control disabled\n");
}

// ---- static dashboard (optional, served from the ramdisk) -----------------

// Guess a Content-Type from the file extension. Defaults to octet-stream.
const char* guess_mime(const std::string& path) {
	auto ends = [&](const char* ext) {
		size_t n = strlen(ext);
		return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
	};
	if (ends(".html") || ends(".htm")) return "text/html; charset=utf-8";
	if (ends(".js") || ends(".mjs"))   return "text/javascript; charset=utf-8";
	if (ends(".css"))                  return "text/css; charset=utf-8";
	if (ends(".json"))                 return "application/json";
	if (ends(".svg"))                  return "image/svg+xml";
	if (ends(".png"))                  return "image/png";
	if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
	if (ends(".webp"))                 return "image/webp";
	if (ends(".ico"))                  return "image/x-icon";
	if (ends(".woff2"))                return "font/woff2";
	if (ends(".woff"))                 return "font/woff";
	if (ends(".map") || ends(".txt"))  return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

bool read_file(const std::string& path, std::string& out) {
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

// Serve a static asset from the dashboard bundle. 'url_path' is the request
// path ("/" maps to index.html). Returns false if there is no bundle or the
// asset does not exist, so the caller can fall through to a 404. Rejects any
// path containing ".." to prevent directory traversal outside the bundle.
bool serve_static(int fd, const std::string& url_path) {
	struct stat st;
	if (stat(FOX_DASHBOARD_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
		return false;  // no bundle packed: API-only mode

	std::string rel = (url_path == "/") ? "/index.html" : url_path;
	if (rel.find("..") != std::string::npos)
		return false;

	std::string full = std::string(FOX_DASHBOARD_DIR) + rel;
	std::string body;
	if (!read_file(full, body))
		return false;

	send_response(fd, "200 OK", guess_mime(full), body);
	return true;
}

// ---- key name mapping -----------------------------------------------------

int key_code_from_name(const std::string& name) {
	if (name == "back")    return KEY_BACK;
	if (name == "home")    return KEY_HOMEPAGE;
	if (name == "menu")    return KEY_MENU;
	if (name == "power")   return KEY_POWER;
	if (name == "volup")   return KEY_VOLUMEUP;
	if (name == "voldown") return KEY_VOLUMEDOWN;
	if (name == "enter")   return KEY_ENTER;
	if (name == "up")      return KEY_UP;
	if (name == "down")    return KEY_DOWN;
	if (name == "left")    return KEY_LEFT;
	if (name == "right")   return KEY_RIGHT;
	return -1;
}

// ---- request dispatch -----------------------------------------------------

struct Request {
	std::string method;
	std::string path;
	std::string query;
	std::string headers;
	std::string body;
};

bool authorized(const Request& req) {
	if (g_token.empty())
		return false;
	if (param(req.query, "token") == g_token)
		return true;
	if (header_value(req.headers, "X-Auth-Token") == g_token)
		return true;
	std::string bearer = header_value(req.headers, "Authorization");
	if (bearer == "Bearer " + g_token)
		return true;
	return false;
}

void handle_status(int fd) {
	std::string json = "{\"width\":";
	json += std::to_string(Fox_Screen::Width());
	json += ",\"height\":";
	json += std::to_string(Fox_Screen::Height());
	json += ",\"input_ready\":";
	json += Fox_Input::IsReady() ? "true" : "false";
	json += ",\"control\":";
	json += g_control_enabled ? "true" : "false";
	json += "}";
	send_json(fd, json);
}

void handle_cmd(int fd, const Request& req) {
	std::vector<std::string> argv = json_argv(req.body);
	if (argv.empty()) {
		// Fallback: "cmd=verb arg arg" (space-split, not space-safe).
		std::string cmd = json_string(req.body, "cmd");
		if (cmd.empty())
			cmd = param(req.body, "cmd");
		std::string tok;
		for (char c : cmd) {
			if (c == ' ') {
				if (!tok.empty()) { argv.push_back(tok); tok.clear(); }
			} else {
				tok += c;
			}
		}
		if (!tok.empty())
			argv.push_back(tok);
	}
	if (argv.empty()) {
		send_response(fd, "400 Bad Request", "application/json", "{\"error\":\"empty command\"}");
		return;
	}
	int id = Fox_Jobs::Submit(argv);
	send_json(fd, "{\"id\":" + std::to_string(id) + "}");
}

void handle_job_output(int fd, const Request& req, int id) {
	std::string offs = param(req.query, "offset");
	size_t offset = offs.empty() ? 0 : (size_t)strtoul(offs.c_str(), nullptr, 10);

	std::string data;
	Fox_Jobs::Snapshot snap;
	if (!Fox_Jobs::ReadOutputAndSnapshot(id, offset, data, snap)) {
		send_404(fd);
		return;
	}

	// JSON-escape the data chunk.
	std::string esc;
	for (char c : data) {
		switch (c) {
			case '"':  esc += "\\\""; break;
			case '\\': esc += "\\\\"; break;
			case '\n': esc += "\\n";  break;
			case '\r': esc += "\\r";  break;
			case '\t': esc += "\\t";  break;
			default:
				if ((unsigned char)c < 0x20) {
					char b[8];
					snprintf(b, sizeof(b), "\\u%04x", c);
					esc += b;
				} else {
					esc += c;
				}
		}
	}
	const char* state = snap.state == Fox_Jobs::PENDING ? "pending" :
	                    snap.state == Fox_Jobs::RUNNING ? "running" : "done";
	std::string json = "{\"offset\":" + std::to_string(offset) +
	                   ",\"state\":\"" + state + "\"" +
	                   ",\"exit_code\":" + std::to_string(snap.exit_code) +
	                   ",\"data\":\"" + esc + "\"}";
	send_json(fd, json);
}

void handle_touch(int fd, const Request& req) {
	long x = 0, y = 0;
	json_int(req.body, "x", x);
	json_int(req.body, "y", y);
	std::string action = json_string(req.body, "action");
	if (action.empty())
		action = "tap";
	if (action == "down")      Fox_Input::TouchDown((int)x, (int)y);
	else if (action == "move") Fox_Input::TouchMove((int)x, (int)y);
	else if (action == "up")   Fox_Input::TouchUp();
	else                       Fox_Input::Tap((int)x, (int)y);
	send_json(fd, "{\"ok\":true}");
}

void handle_key(int fd, const Request& req) {
	int code = -1;
	std::string name = json_string(req.body, "key");
	if (!name.empty())
		code = key_code_from_name(name);
	long raw = 0;
	if (code < 0 && json_int(req.body, "code", raw))
		code = (int)raw;
	if (code < 0) {
		send_response(fd, "400 Bad Request", "application/json", "{\"error\":\"unknown key\"}");
		return;
	}
	std::string action = json_string(req.body, "action");
	if (action == "down")      Fox_Input::Key(code, true);
	else if (action == "up")   Fox_Input::Key(code, false);
	else                       Fox_Input::KeyTap(code);
	send_json(fd, "{\"ok\":true}");
}

void handle_screen_png(int fd) {
	// Ask the GUI thread for a fresh frame. WebUSB polls this endpoint over
	// short-lived sockets, so returning the cached snapshot immediately can make
	// the browser cycle new blob URLs while displaying stale pixels.
	std::string png;
	uint64_t start_gen = 0;
	bool had_frame = Fox_Screen::GetLatest(png, &start_gen);
	for (int i = 0; i < 40; i++) {
		Fox_Screen::RequestFrames();
		std::string next;
		uint64_t gen = 0;
		if (Fox_Screen::GetLatest(next, &gen)) {
			if (!had_frame || gen != start_gen) {
				png.swap(next);
				break;
			}
			if (png.empty())
				png.swap(next);
		}
		struct timespec ts = { 0, 25 * 1000 * 1000 };
		nanosleep(&ts, nullptr);
	}
	// Catch a frame produced during the final sleep before falling back to the
	// previous snapshot.
	std::string latest;
	uint64_t latest_gen = 0;
	if (Fox_Screen::GetLatest(latest, &latest_gen) && (!had_frame || latest_gen != start_gen))
		png.swap(latest);
	if (png.empty()) {
		send_response(fd, "503 Service Unavailable", "text/plain", "capture failed\n");
		return;
	}
	send_response(fd, "200 OK", "image/png", png);
}

// Multipart MJPEG-style stream of PNG frames. Long-lived; runs until the client
// disconnects. Identical consecutive frames are skipped to save bandwidth
// (recovery screens are mostly static).
void handle_stream(int fd) {
	// NOTE on framing: browsers commit a multipart/x-mixed-replace part only
	// when they see the boundary that FOLLOWS it (they ignore Content-Length for
	// render timing). So we must terminate each frame with its own trailing
	// boundary immediately -- otherwise the displayed frame always lags by one
	// (you'd see the previous page after a switch). We send a leading boundary
	// once, then "<headers><data>\r\n--boundary\r\n" per frame.
	std::string head =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=foxframe\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"\r\n"
		"--foxframe\r\n";
	if (!send_all(fd, head))
		return;

	std::string last;
	while (g_running) {
		// Keep the GUI-thread capture alive; it produces a new snapshot only
		// when the screen actually changes, so identical frames are skipped.
		Fox_Screen::RequestFrames();
		std::string png;
		if (Fox_Screen::GetLatest(png) && png != last) {
			last = png;
			char part[128];
			int n = snprintf(part, sizeof(part),
				"Content-Type: image/png\r\nContent-Length: %zu\r\n\r\n",
				png.size());
			if (!send_all(fd, part, (size_t)n))
				break;
			if (!send_all(fd, png))
				break;
			// Trailing boundary flushes THIS frame to the browser right away.
			if (!send_all(fd, "\r\n--foxframe\r\n", 14))
				break;
		}
		// Poll the snapshot a bit faster than the capture ceiling so we forward
		// new frames promptly without busy-spinning.
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, nullptr);
	}
}

void handle_connection(int fd) {
	Request req;
	std::string leftover;
	if (!read_headers(fd, req.headers, leftover)) {
		close(fd);
		return;
	}

	// Request line.
	size_t eol = req.headers.find("\r\n");
	std::string line = req.headers.substr(0, eol);
	size_t sp1 = line.find(' ');
	size_t sp2 = line.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) {
		close(fd);
		return;
	}
	req.method = line.substr(0, sp1);
	std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t q = target.find('?');
	if (q == std::string::npos) {
		req.path = target;
	} else {
		req.path = target.substr(0, q);
		req.query = target.substr(q + 1);
	}
	// Trim the request line off the header block so header_value() searches the
	// real headers only.
	req.headers = req.headers.substr(eol + 2);

	// Read body if Content-Length present.
	std::string cl = header_value(req.headers, "Content-Length");
	if (!cl.empty()) {
		size_t need = (size_t)strtoul(cl.c_str(), nullptr, 10);
		req.body = leftover;
		char tmp[4096];
		while (req.body.size() < need) {
			ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
			if (n <= 0)
				break;
			req.body.append(tmp, n);
			if (req.body.size() > 1 * 1024 * 1024)
				break;  // 1 MiB body guard
		}
	}

	// CORS preflight: a cross-origin POST carrying the JSON body and the
	// X-Auth-Token header is preceded by an OPTIONS request. Answer it (no auth)
	// with the allowed methods/headers so the real request can follow.
	if (req.method == "OPTIONS") {
		const char* resp =
			"HTTP/1.1 204 No Content\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
			"Access-Control-Allow-Headers: X-Auth-Token, Content-Type\r\n"
			"Access-Control-Max-Age: 600\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		send_all(fd, resp, strlen(resp));
		close(fd);
		return;
	}

	// Static dashboard assets (when a bundle is packed on the ramdisk) are
	// public and leak nothing; the APIs and the screen endpoints below require
	// the session token. A non-API GET that is not one of the dynamic routes is
	// treated as a static asset request.
	if (req.method == "GET" && req.path.compare(0, 5, "/api/") != 0 &&
	    req.path != "/screen.png" && req.path != "/stream.mjpeg") {
		if (!serve_static(fd, req.path))
			send_404(fd);
		close(fd);
		return;
	}

	// Everything else requires the session token.
	if (!authorized(req)) {
		send_401(fd);
		close(fd);
		return;
	}

	if (req.method == "GET" && req.path == "/api/status") {
		handle_status(fd);
	} else if (req.method == "POST" && req.path == "/api/cmd") {
		handle_cmd(fd, req);
	} else if (req.method == "GET" && req.path == "/api/jobs") {
		send_json(fd, Fox_Jobs::ListJson());
	} else if (req.method == "GET" && req.path.compare(0, 10, "/api/jobs/") == 0) {
		std::string rest = req.path.substr(10);
		size_t slash = rest.find('/');
		int id = atoi(rest.c_str());
		if (slash != std::string::npos && rest.substr(slash) == "/output") {
			handle_job_output(fd, req, id);
		} else {
			Fox_Jobs::Snapshot snap;
			if (!Fox_Jobs::Get(id, snap)) {
				send_404(fd);
			} else {
				const char* state = snap.state == Fox_Jobs::PENDING ? "pending" :
				                    snap.state == Fox_Jobs::RUNNING ? "running" : "done";
				std::string json = "{\"id\":" + std::to_string(snap.id) +
				                   ",\"state\":\"" + state + "\"" +
				                   ",\"exit_code\":" + std::to_string(snap.exit_code) +
				                   ",\"output_size\":" + std::to_string(snap.output_size) + "}";
				send_json(fd, json);
			}
		}
	} else if (req.method == "GET" && req.path == "/screen.png") {
		if (!g_control_enabled) send_403(fd);
		else handle_screen_png(fd);
	} else if (req.method == "GET" && req.path == "/stream.mjpeg") {
		if (!g_control_enabled) send_403(fd);
		else handle_stream(fd);  // blocks until disconnect
	} else if (req.method == "POST" && req.path == "/api/input/touch") {
		if (!g_control_enabled) send_403(fd);
		else handle_touch(fd, req);
	} else if (req.method == "POST" && req.path == "/api/input/key") {
		if (!g_control_enabled) send_403(fd);
		else handle_key(fd, req);
	} else {
		send_404(fd);
	}
	close(fd);
}

struct ConnArg { int fd; };

void* connection_thread(void* arg) {
	ConnArg* ca = static_cast<ConnArg*>(arg);
	int fd = ca->fd;
	delete ca;
	handle_connection(fd);
	return nullptr;
}

void* accept_thread(void*) {
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

std::string make_token() {
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

bool Fox_Httpd::Start(int port) {
	if (g_running)
		return true;

	g_port = port > 0 ? port : 8080;
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

	// uinput is best-effort: the dashboard still works for control/screen even
	// if input injection is unavailable.
	Fox_Input::Init();

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

void Fox_Httpd::Stop() {
	if (!g_running)
		return;
	g_running = false;
	if (g_listen_fd >= 0) {
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	pthread_join(g_accept_thread, nullptr);
	Fox_Input::Shutdown();
	g_token.clear();
}

bool Fox_Httpd::IsRunning() {
	return g_running;
}

int Fox_Httpd::Port() {
	return g_port;
}

std::string Fox_Httpd::Token() {
	return g_token;
}

void Fox_Httpd::SetControlEnabled(bool on) {
	g_control_enabled = on;
}

bool Fox_Httpd::ControlEnabled() {
	return g_control_enabled;
}

std::string Fox_Httpd::LocalAddress() {
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
