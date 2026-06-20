/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_protocol.hpp"

#include <cstdio>

#include "../orscmd/orscmd.h"

std::string Fox_Protocol::Base64Encode(const std::string& in, bool newline)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4 + (newline ? 1 : 0));

	for (size_t i = 0; i < in.size(); i += 3) {
		unsigned int b0 = (unsigned char)in[i];
		unsigned int b1 = i + 1 < in.size() ? (unsigned char)in[i + 1] : 0;
		unsigned int b2 = i + 2 < in.size() ? (unsigned char)in[i + 2] : 0;
		out += table[b0 >> 2];
		out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
		out += i + 1 < in.size() ? table[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
		out += i + 2 < in.size() ? table[b2 & 0x3f] : '=';
	}
	if (newline)
		out += '\n';
	return out;
}

std::vector<std::string> Fox_Protocol::ParseNulArgv(const char* buffer, int len)
{
	std::vector<std::string> argv;
	int start = 0;
	for (int i = 0; i < len; i++) {
		if (buffer[i] == '\0') {
			if (i == start)
				break;
			argv.push_back(std::string(buffer + start, i - start));
			start = i + 1;
		}
	}
	return argv;
}

void Fox_Protocol::WriteExitSentinel(FILE* out, int code)
{
	if (out)
		fprintf(out, "%s%d\n", FOX_EXIT_SENTINEL, code);
}

void Fox_Protocol::WriteKvBlock(FILE* out, const char* begin, const std::map<std::string, std::string>& kv, const char* end)
{
	if (!out)
		return;
	fprintf(out, "%s\n", begin);
	for (std::map<std::string, std::string>::const_iterator it = kv.begin(); it != kv.end(); ++it)
		fprintf(out, "%s=%s\n", it->first.c_str(), it->second.c_str());
	fprintf(out, "%s\n", end);
}

int Fox_Protocol::ClampFps(int fps)
{
	if (fps < 1)
		return 1;
	if (fps > 30)
		return 30;
	return fps;
}

std::string Fox_Protocol::JsonEscape(const std::string& in)
{
	std::string out;
	for (char c : in) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if ((unsigned char)c < 0x20) {
					char b[8];
					snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
					out += b;
				} else {
					out += c;
				}
		}
	}
	return out;
}
