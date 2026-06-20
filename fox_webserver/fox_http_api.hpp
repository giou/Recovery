/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_API_HPP
#define _FOX_HTTP_API_HPP

#include "fox_http_common.hpp"

namespace Fox_Http {

void HandleStatus(int fd);
void HandleCmd(int fd, const Request& req);
void HandleJobList(int fd);
void HandleJobRoute(int fd, const Request& req, int id, bool output);
void HandleTouch(int fd, const Request& req);
void HandleKey(int fd, const Request& req);

} // namespace Fox_Http

#endif // _FOX_HTTP_API_HPP
