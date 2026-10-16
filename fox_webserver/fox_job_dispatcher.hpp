/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_JOB_DISPATCHER_HPP
#define _FOX_JOB_DISPATCHER_HPP

class Fox_Job_Dispatcher
{
public:
	// GUI-thread side: drain one queued HTTP job into the serialized foxcmd
	// action path when the recovery engine is idle.
	static void Poll();
};

#endif // _FOX_JOB_DISPATCHER_HPP
