/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_API_HPP
#define _FOX_HTTP_API_HPP

#include "fox_http_common.hpp"

namespace Fox_Http {

void HandleStatus(Fox_Io::Conn& conn);
void HandleCmd(Fox_Io::Conn& conn, const Request& req);
void HandleJobList(Fox_Io::Conn& conn);
void HandleJobRoute(Fox_Io::Conn& conn, const Request& req, int id, bool output);
void HandleTouch(Fox_Io::Conn& conn, const Request& req);
void HandleKey(Fox_Io::Conn& conn, const Request& req);

} // namespace Fox_Http

#endif // _FOX_HTTP_API_HPP
