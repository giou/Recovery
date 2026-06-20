/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_COMMAND_DISPATCHER_HPP
#define _FOX_COMMAND_DISPATCHER_HPP

#include <cstdio>
#include <string>
#include <vector>

class Fox_Command_Dispatcher
{
public:
	enum Result {
		FINISHED,
		ACTIVE,
		FAILED
	};

	static bool IsActive();
	static Result DispatchFifo(FILE* out, const std::vector<std::string>& argv);
	static bool DispatchJob(FILE* out, const std::vector<std::string>& argv);
	static bool CapturePendingFrameIfNeeded();
	static bool CancelRequested(bool legacy_command_active);
	static void CommandDone();
	static void ResetPendingIfIdle();
};

#endif // _FOX_COMMAND_DISPATCHER_HPP
