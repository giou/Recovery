/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_PROTOCOL_HPP
#define _FOX_PROTOCOL_HPP

#include <cstdio>
#include <map>
#include <string>
#include <vector>

class Fox_Protocol
{
public:
	static std::string Base64Encode(const std::string& in, bool newline = true);
	static std::vector<std::string> ParseNulArgv(const char* buffer, int len);

	static void WriteExitSentinel(FILE* out, int code);
	static void WriteKvBlock(FILE* out, const char* begin, const std::map<std::string, std::string>& kv, const char* end);

	static int ClampFps(int fps);
	static std::string JsonEscape(const std::string& in);
};

#endif // _FOX_PROTOCOL_HPP
