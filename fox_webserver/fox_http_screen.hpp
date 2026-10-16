/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_SCREEN_HPP
#define _FOX_HTTP_SCREEN_HPP

#include "fox_io.hpp"

namespace Fox_Http {

void HandleScreenPng(Fox_Io::Conn& conn);
void HandleStream(Fox_Io::Conn& conn);

} // namespace Fox_Http

#endif // _FOX_HTTP_SCREEN_HPP
