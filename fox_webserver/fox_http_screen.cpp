/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_http_screen.hpp"

#include <cstdio>
#include <string>
#include <time.h>

#include "fox_http_common.hpp"
#include "fox_httpd.hpp"
#include "../fox_fifo/fox_screen_service.hpp"

void Fox_Http::HandleScreenPng(int fd)
{
	std::string png;
	if (!Fox_Screen_Service::GetFreshPng(png)) {
		SendResponse(fd, "503 Service Unavailable", "text/plain", "capture failed\n");
		return;
	}
	SendResponse(fd, "200 OK", "image/png", png);
}

void Fox_Http::HandleStream(int fd)
{
	std::string head =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=foxframe\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
		"Pragma: no-cache\r\n"
		"Expires: 0\r\n"
		"Connection: close\r\n"
		"\r\n"
		"--foxframe\r\n";
	if (!SendAll(fd, head))
		return;

	std::string start_png;
	uint64_t start_gen = 0;
	bool have_start = Fox_Screen_Service::GetLatest(start_png, &start_gen);
	uint64_t last_sent_gen = 0;
	bool sent_any = false;
	while (Fox_Httpd::IsRunning()) {
		Fox_Screen_Service::RequestFrames();
		std::string png;
		uint64_t gen = 0;
		if (Fox_Screen_Service::GetLatest(png, &gen) &&
		    gen != last_sent_gen && (!have_start || sent_any || gen != start_gen)) {
			last_sent_gen = gen;
			sent_any = true;
			char part[128];
			int n = snprintf(part, sizeof(part),
				"Content-Type: image/png\r\nContent-Length: %zu\r\n\r\n",
				png.size());
			if (!SendAll(fd, part, (size_t)n))
				break;
			if (!SendAll(fd, png))
				break;
			if (!SendAll(fd, "\r\n--foxframe\r\n", 14))
				break;
		}
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, nullptr);
	}
}
