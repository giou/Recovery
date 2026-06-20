/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_STATIC_HPP
#define _FOX_HTTP_STATIC_HPP

#include <string>

namespace Fox_Http {

bool ServeStatic(int fd, const std::string& url_path);

} // namespace Fox_Http

#endif // _FOX_HTTP_STATIC_HPP
