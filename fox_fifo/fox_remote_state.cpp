/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_remote_state.hpp"

#include "fox_channel.hpp"
#include "fox_screen_service.hpp"
#include "fox_screen_stream.hpp"
#ifdef OF_ENABLE_WLAN
#include "../fox_webserver/fox_httpd.hpp"
#include "../fox_webserver/fox_jobs.hpp"
#endif

bool Fox_Remote_State::CommandActive(bool legacy_command_active)
{
	return legacy_command_active || Fox_Channel::IsActive();
}

bool Fox_Remote_State::CanAcceptCommand(bool legacy_command_active)
{
	return !CommandActive(legacy_command_active);
}

bool Fox_Remote_State::ShouldForceRender()
{
	return Fox_Screen_Stream::ShouldForceRender() || Fox_Screen_Service::ShouldForceRender();
}

bool Fox_Remote_State::ScreenStreamActive()
{
	return Fox_Screen_Stream::IsActive();
}

bool Fox_Remote_State::ServerRunning()
{
#ifdef OF_ENABLE_WLAN
	return Fox_Httpd::IsRunning();
#else
	return false;
#endif
}

bool Fox_Remote_State::ControlEnabled()
{
#ifdef OF_ENABLE_WLAN
	return Fox_Httpd::ControlEnabled();
#else
	return true;
#endif
}

int Fox_Remote_State::ActiveJob()
{
#ifdef OF_ENABLE_WLAN
	return Fox_Jobs::Active();
#else
	return 0;
#endif
}
