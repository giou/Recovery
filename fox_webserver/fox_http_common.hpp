/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_HTTP_COMMON_HPP
#define _FOX_HTTP_COMMON_HPP

#include <string>
#include <vector>

namespace Fox_Http {

struct Request {
	std::string method;
	std::string path;
	std::string query;
	std::string headers;
	std::string body;
};

bool SendAll(int fd, const char* data, size_t len);
bool SendAll(int fd, const std::string& data);
bool ReadHeaders(int fd, std::string& headers, std::string& leftover);

std::string UrlDecode(const std::string& in);
std::string HeaderValue(const std::string& headers, const std::string& name);
std::string Param(const std::string& qs, const std::string& key);
bool JsonObjectBody(const std::string& body);
std::string JsonString(const std::string& body, const std::string& key);
bool JsonInt(const std::string& body, const std::string& key, long& out);
std::vector<std::string> JsonArgv(const std::string& body);

void SendResponse(int fd, const char* status, const char* ctype, const std::string& body);
void SendJson(int fd, const std::string& json);
void Send404(int fd);
void Send401(int fd);
void Send403(int fd);
void SendCorsOptions(int fd);

} // namespace Fox_Http

#endif // _FOX_HTTP_COMMON_HPP
