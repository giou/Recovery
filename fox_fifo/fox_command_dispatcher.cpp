/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_command_dispatcher.hpp"

#include "fox_channel.hpp"
#include "fox_fifo.hpp"
#include "fox_protocol.hpp"
#include "fox_remote_input.hpp"
#include "fox_screen_service.hpp"
#include "fox_screen_stream.hpp"
#include "../data.hpp"
#ifdef OF_ENABLE_WLAN
#include "../fox_webserver/fox_jobs.hpp"
#endif
#include "../gui/gui.h"
#include "../gui/gui.hpp"
#include "../gui/objects.hpp"
#include "../gui/pages.hpp"
#include "../twcommon.h"

namespace {

class CommandOutput
{
public:
	CommandOutput() : file_(nullptr) {}
	~CommandOutput()
	{
		Close();
	}

	void Reset(FILE* file)
	{
		Close();
		file_ = file;
	}

	FILE* file() const
	{
		return file_;
	}

	bool active() const
	{
		return file_ != nullptr;
	}

	void FinishFifo(int code)
	{
		if (!file_)
			return;
		Fox_Protocol::WriteExitSentinel(file_, code);
		Close();
		Fox_Channel::RearmInput();
	}

	void Close()
	{
		if (!file_)
			return;
		gui_set_FILE(nullptr);
		fclose(file_);
		file_ = nullptr;
	}

private:
	FILE* file_;
};

CommandOutput g_output;
bool g_pending_screencap = false;

bool is_silent_screencap(const std::vector<std::string>& argv)
{
	if (argv.empty() || argv[0] != "screencap")
		return false;
	for (size_t i = 1; i < argv.size(); i++)
		if (argv[i] == "--base64")
			return true;
	return false;
}

bool is_silent_input(const std::vector<std::string>& argv)
{
	return !argv.empty() && argv[0] == "input";
}

void start_action(FILE* out, const std::vector<std::string>& argv, bool progress)
{
	if (g_output.file() != out)
		g_output.Reset(out);
	Fox_Fifo::Set_Args(argv);
	gui_set_FILE(g_output.file());
	if (progress)
		gui_fox_progress_begin();
	DataManager::SetValue("tw_action", "foxcmd");
	DataManager::SetValue("tw_action_param", "");
	std::string currentPage = PageManager::GetCurrentPage();
	DataManager::SetValue("tw_has_action2", "1");
	DataManager::SetValue("tw_action2", "page");
	DataManager::SetValue("tw_action2_param", currentPage);
	DataManager::SetValue("tw_action_text1", gui_lookup("running_recovery_commands", "Running Recovery Commands"));
	DataManager::SetValue("tw_action_text2", "");
	gui_changePage("singleaction_page");
}

} // namespace

bool Fox_Command_Dispatcher::IsActive()
{
	return g_output.active();
}

Fox_Command_Dispatcher::Result Fox_Command_Dispatcher::DispatchFifo(FILE* out, const std::vector<std::string>& argv)
{
	if (!out || IsActive())
		return FAILED;

	g_output.Reset(out);

	int code = 0;
	if (Fox_Screen_Stream::HandleCommand(argv, g_output.file(), &code)) {
		g_output.FinishFifo(code);
		return FINISHED;
	}

	if (is_silent_screencap(argv)) {
		g_pending_screencap = true;
		gui_forceRender();
		return ACTIVE;
	}

	if (is_silent_input(argv)) {
		std::vector<std::string> args(argv.begin() + 1, argv.end());
		std::string output;
		code = Fox_Remote_Input::RunCommand(args, &output);
		if (!output.empty())
			fwrite(output.data(), 1, output.size(), g_output.file());
		g_output.FinishFifo(code);
		return FINISHED;
	}

	start_action(g_output.file(), argv, true);
	return ACTIVE;
}

bool Fox_Command_Dispatcher::DispatchJob(FILE* out, const std::vector<std::string>& argv)
{
	if (!out || IsActive())
		return false;
	start_action(out, argv, false);
	return true;
}

bool Fox_Command_Dispatcher::CapturePendingFrameIfNeeded()
{
	if (!g_pending_screencap)
		return false;

	std::string png;
	if (Fox_Screen_Service::CaptureRenderedFrameNow(png)) {
		std::string b64 = Fox_Protocol::Base64Encode(png);
		fwrite(b64.data(), 1, b64.size(), g_output.file());
		g_pending_screencap = false;
		g_output.FinishFifo(0);
	} else {
		fputs("fox: screencap failed\n", g_output.file());
		g_pending_screencap = false;
		g_output.FinishFifo(1);
	}
	return true;
}

bool Fox_Command_Dispatcher::CancelRequested(bool legacy_command_active)
{
	if (Fox_Screen_Stream::IsActive()) {
		LOGINFO("fox: screen stream cancel request received\n");
		Fox_Screen_Stream::Stop();
	}
	if (IsActive() || legacy_command_active) {
		LOGINFO("fox: cancel request received\n");
		return true;
	}
	return false;
}

void Fox_Command_Dispatcher::CommandDone()
{
	gui_fox_progress_end();
	g_output.Close();
#ifdef OF_ENABLE_WLAN
	Fox_Jobs::FinishActive();
#endif

	if (DataManager::GetIntValue("tw_page_done") == 0)
		Fox_Channel::RearmInput();
}

void Fox_Command_Dispatcher::ResetPendingIfIdle()
{
	if (!IsActive())
		g_pending_screencap = false;
}

void fox_command_done()
{
	Fox_Command_Dispatcher::CommandDone();
}
