/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_TTYD_HPP
#define _FOX_TTYD_HPP

#include <string>

class Fox_Ttyd
{
public:
	static bool Start(int port = 7681);
	static void Stop();
	static bool IsRunning();
	static int Port();
	static std::string Shell();
	static std::string LastError();
};

#endif // _FOX_TTYD_HPP
