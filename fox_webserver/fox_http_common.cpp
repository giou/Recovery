/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_http_common.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include <json/json.h>

namespace Fox_Http {

namespace {

bool ParseJsonBody(const std::string& body, Json::Value& root)
{
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;

	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(body.data(), body.data() + body.size(), &root, &errors);
}

} // namespace

bool SendAll(int fd, const char* data, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		off += (size_t)n;
	}
	return true;
}

bool SendAll(int fd, const std::string& data)
{
	return SendAll(fd, data.data(), data.size());
}

bool ReadHeaders(int fd, std::string& headers, std::string& leftover)
{
	std::string buf;
	char tmp[2048];
	while (buf.find("\r\n\r\n") == std::string::npos) {
		ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
		if (n <= 0)
			return false;
		buf.append(tmp, n);
		if (buf.size() > 64 * 1024)
			return false;
	}
	size_t pos = buf.find("\r\n\r\n");
	headers = buf.substr(0, pos);
	leftover = buf.substr(pos + 4);
	return true;
}

std::string UrlDecode(const std::string& in)
{
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
		out += in[i] == '+' ? ' ' : in[i];
	}
	return out;
}

std::string HeaderValue(const std::string& headers, const std::string& name)
{
	std::string lower_headers = headers;
	for (char& c : lower_headers)
		c = tolower(c);
	std::string key = name;
	for (char& c : key)
		c = tolower(c);
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

std::string Param(const std::string& qs, const std::string& key)
{
	size_t i = 0;
	while (i < qs.size()) {
		size_t amp = qs.find('&', i);
		std::string pair = qs.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
		size_t eq = pair.find('=');
		std::string k = eq == std::string::npos ? pair : pair.substr(0, eq);
		if (k == key)
			return eq == std::string::npos ? "" : UrlDecode(pair.substr(eq + 1));
		if (amp == std::string::npos)
			break;
		i = amp + 1;
	}
	return "";
}

bool JsonObjectBody(const std::string& body)
{
	Json::Value root;
	return ParseJsonBody(body, root) && root.isObject();
}

std::string JsonString(const std::string& body, const std::string& key)
{
	Json::Value root;
	if (!ParseJsonBody(body, root) || !root.isObject() || !root.isMember(key))
		return "";

	const Json::Value& value = root[key];
	return value.isString() ? value.asString() : "";
}

bool JsonInt(const std::string& body, const std::string& key, long& out)
{
	Json::Value root;
	if (!ParseJsonBody(body, root) || !root.isObject() || !root.isMember(key))
		return false;

	const Json::Value& value = root[key];
	if (!value.isIntegral())
		return false;

	out = value.asInt64();
	return true;
}

std::vector<std::string> JsonArgv(const std::string& body)
{
	std::vector<std::string> out;
	Json::Value root;
	if (!ParseJsonBody(body, root) || !root.isObject() || !root["argv"].isArray())
		return out;

	const Json::Value& argv = root["argv"];
	for (Json::ArrayIndex i = 0; i < argv.size(); i++) {
		if (argv[i].isString())
			out.push_back(argv[i].asString());
	}
	return out;
}

void SendResponse(int fd, const char* status, const char* ctype, const std::string& body)
{
	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
		"Pragma: no-cache\r\n"
		"Expires: 0\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n",
		status, ctype, body.size());
	if (n > 0)
		SendAll(fd, hdr, (size_t)n);
	SendAll(fd, body);
}

void SendJson(int fd, const std::string& json)
{
	SendResponse(fd, "200 OK", "application/json", json);
}

void Send404(int fd)
{
	SendResponse(fd, "404 Not Found", "text/plain", "not found\n");
}

void Send401(int fd)
{
	SendResponse(fd, "401 Unauthorized", "text/plain", "unauthorized\n");
}

void Send403(int fd)
{
	SendResponse(fd, "403 Forbidden", "text/plain", "remote control disabled\n");
}

void SendCorsOptions(int fd)
{
	const char* resp =
		"HTTP/1.1 204 No Content\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
		"Access-Control-Allow-Headers: X-Auth-Token, Content-Type\r\n"
		"Access-Control-Max-Age: 600\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n";
	SendAll(fd, resp, strlen(resp));
}

} // namespace Fox_Http
