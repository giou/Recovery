/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_http_static.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "fox_http_common.hpp"

namespace {

#ifndef FOX_DASHBOARD_DIR
#define FOX_DASHBOARD_DIR "/system/etc/fox/dashboard"
#endif

const char* guess_mime(const std::string& path)
{
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

bool read_file(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

} // namespace

bool Fox_Http::ServeStatic(Fox_Io::Conn& conn, const std::string& url_path)
{
	struct stat st;
	if (stat(FOX_DASHBOARD_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
		return false;

	std::string rel = (url_path == "/") ? "/index.html" : url_path;
	if (rel.find("..") != std::string::npos)
		return false;

	std::string full = std::string(FOX_DASHBOARD_DIR) + rel;
	std::string body;
	if (!read_file(full, body))
		return false;

	SendResponse(conn, "200 OK", guess_mime(full), body);
	return true;
}
