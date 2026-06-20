/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_remote_input.hpp"

#include <cctype>
#include <cstdlib>
#include <linux/input.h>
#include <string>
#include <time.h>

#include "fox_input.hpp"

bool Fox_Remote_Input::Init()
{
	return Fox_Input::Init();
}

void Fox_Remote_Input::Shutdown()
{
	Fox_Input::Shutdown();
}

bool Fox_Remote_Input::IsReady()
{
	return Fox_Input::IsReady();
}

int Fox_Remote_Input::KeyCodeFromName(const std::string& name)
{
	if (name == "back")    return KEY_BACK;
	if (name == "home")    return KEY_HOMEPAGE;
	if (name == "menu")    return KEY_MENU;
	if (name == "power")   return KEY_POWER;
	if (name == "volup")   return KEY_VOLUMEUP;
	if (name == "voldown") return KEY_VOLUMEDOWN;
	if (name == "enter")   return KEY_ENTER;
	if (name == "up")      return KEY_UP;
	if (name == "down")    return KEY_DOWN;
	if (name == "left")    return KEY_LEFT;
	if (name == "right")   return KEY_RIGHT;
	return -1;
}

void Fox_Remote_Input::Tap(int x, int y)
{
	Fox_Input::Tap(x, y);
}

void Fox_Remote_Input::TouchDown(int x, int y)
{
	Fox_Input::TouchDown(x, y);
}

void Fox_Remote_Input::TouchMove(int x, int y)
{
	Fox_Input::TouchMove(x, y);
}

void Fox_Remote_Input::TouchUp()
{
	Fox_Input::TouchUp();
}

void Fox_Remote_Input::Swipe(int x1, int y1, int x2, int y2, int ms)
{
	if (ms < 0)
		ms = 0;
	const int steps = 16;
	TouchDown(x1, y1);
	for (int i = 1; i <= steps; i++) {
		int x = x1 + (x2 - x1) * i / steps;
		int y = y1 + (y2 - y1) * i / steps;
		TouchMove(x, y);
		struct timespec ts = { 0, (long)ms * 1000000L / steps };
		nanosleep(&ts, nullptr);
	}
	TouchUp();
}

void Fox_Remote_Input::Key(int code, bool down)
{
	Fox_Input::Key(code, down);
}

void Fox_Remote_Input::KeyTap(int code)
{
	Fox_Input::KeyTap(code);
}

namespace {

void append_line(std::string* out, const std::string& line)
{
	if (out)
		*out += line + "\n";
}

} // namespace

int Fox_Remote_Input::RunCommand(const std::vector<std::string>& args, std::string* output)
{
	if (args.empty()) {
		append_line(output, "fox: input requires a subcommand (tap|swipe|key|down|move|up)");
		return 2;
	}
	if (!Init()) {
		append_line(output, "fox: unable to create virtual input device");
		return 1;
	}

	const std::string& sub = args[0];
	if (sub == "tap") {
		if (args.size() < 3) { append_line(output, "fox: input tap requires X Y"); return 2; }
		Tap(atoi(args[1].c_str()), atoi(args[2].c_str()));
		return 0;
	}
	if (sub == "down") {
		if (args.size() < 3) { append_line(output, "fox: input down requires X Y"); return 2; }
		TouchDown(atoi(args[1].c_str()), atoi(args[2].c_str()));
		return 0;
	}
	if (sub == "move") {
		if (args.size() < 3) { append_line(output, "fox: input move requires X Y"); return 2; }
		TouchMove(atoi(args[1].c_str()), atoi(args[2].c_str()));
		return 0;
	}
	if (sub == "up") {
		TouchUp();
		return 0;
	}
	if (sub == "swipe") {
		if (args.size() < 5) { append_line(output, "fox: input swipe requires X1 Y1 X2 Y2 [ms]"); return 2; }
		int ms = args.size() > 5 ? atoi(args[5].c_str()) : 200;
		Swipe(atoi(args[1].c_str()), atoi(args[2].c_str()),
		      atoi(args[3].c_str()), atoi(args[4].c_str()), ms);
		return 0;
	}
	if (sub == "key") {
		if (args.size() < 2) { append_line(output, "fox: input key requires a name or numeric code"); return 2; }
		int code = isdigit((unsigned char)args[1][0]) ? atoi(args[1].c_str()) : KeyCodeFromName(args[1]);
		if (code < 0) {
			append_line(output, "fox: unknown key '" + args[1] + "'");
			return 2;
		}
		KeyTap(code);
		return 0;
	}

	append_line(output, "fox: unknown input subcommand '" + sub + "' (tap|swipe|key|down|move|up)");
	return 2;
}
