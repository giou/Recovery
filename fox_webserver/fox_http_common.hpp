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

#include "fox_io.hpp"

namespace Fox_Http {

struct Request {
	std::string method;
	std::string path;
	std::string query;
	std::string headers;
	std::string body;
};

// All I/O goes through Fox_Io::Conn so the same handlers serve both plaintext
// (Fox_Httpd) and TLS (Fox_Httpsd) connections.
bool SendAll(Fox_Io::Conn& conn, const char* data, size_t len);
bool SendAll(Fox_Io::Conn& conn, const std::string& data);
bool ReadHeaders(Fox_Io::Conn& conn, std::string& headers, std::string& leftover);

std::string UrlDecode(const std::string& in);
std::string HeaderValue(const std::string& headers, const std::string& name);
std::string Param(const std::string& qs, const std::string& key);
bool JsonObjectBody(const std::string& body);
std::string JsonString(const std::string& body, const std::string& key);
bool JsonInt(const std::string& body, const std::string& key, long& out);

// Resolve the per-request CORS policy: pass the value to echo in
// Access-Control-Allow-Origin (empty = emit no CORS header). Must be called once
// per connection, before any response, so every reply on this thread carries the
// right header. CorsHeaderBlock returns the rendered header line(s).
void SetCorsForRequest(const std::string& allowed_origin);
const std::string& CorsHeaderBlock();

void SendResponse(Fox_Io::Conn& conn, const char* status, const char* ctype, const std::string& body);
void SendJson(Fox_Io::Conn& conn, const std::string& json);
void Send404(Fox_Io::Conn& conn);
void Send401(Fox_Io::Conn& conn);
void Send403(Fox_Io::Conn& conn);
void SendCorsOptions(Fox_Io::Conn& conn);

} // namespace Fox_Http

#endif // _FOX_HTTP_COMMON_HPP
