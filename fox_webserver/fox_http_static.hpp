/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_STATIC_HPP
#define _FOX_HTTP_STATIC_HPP

#include <string>

#include "fox_io.hpp"

namespace Fox_Http {

bool ServeStatic(Fox_Io::Conn& conn, const std::string& url_path);

} // namespace Fox_Http

#endif // _FOX_HTTP_STATIC_HPP
