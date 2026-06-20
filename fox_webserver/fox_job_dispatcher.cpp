/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_job_dispatcher.hpp"

#include <cstdio>
#include <vector>

#include "fox_jobs.hpp"
#include "../data.hpp"
#include "../fox_fifo/fox_command_dispatcher.hpp"
#include "../twcommon.h"

void Fox_Job_Dispatcher::Poll()
{
	if (Fox_Command_Dispatcher::IsActive())
		return;
	if (DataManager::GetIntValue("tw_busy") != 0)
		return;
	if (!Fox_Jobs::HasPending())
		return;

	FILE* jobout = nullptr;
	std::vector<std::string> argv;
	if (!Fox_Jobs::BeginNext(&jobout, argv) || !jobout)
		return;
	if (argv.empty()) {
		fclose(jobout);
		Fox_Jobs::NotifyExit(1);
		Fox_Jobs::FinishActive();
		return;
	}

	LOGINFO("fox job '%s' (%zu args) dispatched\n", argv[0].c_str(), argv.size());
	if (!Fox_Command_Dispatcher::DispatchJob(jobout, argv)) {
		fclose(jobout);
		Fox_Jobs::NotifyExit(1);
		Fox_Jobs::FinishActive();
	}
}
