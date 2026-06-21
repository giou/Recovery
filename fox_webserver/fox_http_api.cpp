/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_http_api.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "fox_httpd.hpp"
#include "fox_json.hpp"
#include "fox_jobs.hpp"
#include "../fox_fifo/fox_remote_input.hpp"
#include "../fox_fifo/fox_screen_service.hpp"

namespace {

const char* state_name(Fox_Jobs::State state)
{
	return state == Fox_Jobs::PENDING ? "pending" :
	       state == Fox_Jobs::RUNNING ? "running" : "done";
}

} // namespace

void Fox_Http::HandleStatus(Fox_Io::Conn& conn)
{
	Fox_Json_Object json;
	json.AddInt("width", Fox_Screen_Service::Width())
	    .AddInt("height", Fox_Screen_Service::Height())
	    .AddBool("input_ready", Fox_Remote_Input::IsReady())
	    .AddBool("control", Fox_Httpd::ControlEnabled());
	SendJson(conn, json.String());
}

void Fox_Http::HandleCmd(Fox_Io::Conn& conn, const Request& req)
{
	Fox_Rpc_Request request = Fox_Protocol::ParseRequestJson(req.body);
	if (!request.ok()) {
		Fox_Json_Object json;
		json.AddString("error", request.error);
		SendResponse(conn, "400 Bad Request", "application/json", json.String());
		return;
	}
	int id = Fox_Jobs::Submit(request);
	if (id == 0) {
		Fox_Json_Object json;
		json.AddString("error", "unable to queue request");
		SendResponse(conn, "400 Bad Request", "application/json", json.String());
		return;
	}
	Fox_Json_Object json;
	json.AddInt("id", id);
	SendJson(conn, json.String());
}

void Fox_Http::HandleJobList(Fox_Io::Conn& conn)
{
	SendJson(conn, Fox_Jobs::ListJson());
}

void Fox_Http::HandleJobRoute(Fox_Io::Conn& conn, const Request& req, int id, bool output)
{
	if (output) {
		std::string offs = Param(req.query, "offset");
		size_t offset = offs.empty() ? 0 : (size_t)strtoul(offs.c_str(), nullptr, 10);

		std::string data;
		Fox_Jobs::Snapshot snap;
		if (!Fox_Jobs::ReadOutputAndSnapshot(id, offset, data, snap)) {
			Send404(conn);
			return;
		}

		Fox_Json_Object json;
		json.AddSize("offset", offset)
		    .AddString("state", state_name(snap.state))
		    .AddInt("exit_code", snap.exit_code)
		    .AddString("data", data);
		SendJson(conn, json.String());
		return;
	}

	Fox_Jobs::Snapshot snap;
	if (!Fox_Jobs::Get(id, snap)) {
		Send404(conn);
		return;
	}
	Fox_Json_Object json;
	json.AddInt("id", snap.id)
	    .AddString("state", state_name(snap.state))
	    .AddInt("exit_code", snap.exit_code)
	    .AddSize("output_size", snap.output_size);
	SendJson(conn, json.String());
}

void Fox_Http::HandleTouch(Fox_Io::Conn& conn, const Request& req)
{
	if (!JsonObjectBody(req.body)) {
		Fox_Json_Object json;
		json.AddString("error", "invalid json");
		SendResponse(conn, "400 Bad Request", "application/json", json.String());
		return;
	}

	long x = 0, y = 0;
	JsonInt(req.body, "x", x);
	JsonInt(req.body, "y", y);
	std::string action = JsonString(req.body, "action");
	if (action.empty())
		action = "tap";
	if (!Fox_Remote_Input::Init()) {
		Fox_Json_Object json;
		json.AddString("error", "input unavailable");
		SendResponse(conn, "503 Service Unavailable", "application/json", json.String());
		return;
	}
	if (action == "down")      Fox_Remote_Input::TouchDown((int)x, (int)y);
	else if (action == "move") Fox_Remote_Input::TouchMove((int)x, (int)y);
	else if (action == "up")   Fox_Remote_Input::TouchUp();
	else                       Fox_Remote_Input::Tap((int)x, (int)y);
	Fox_Json_Object json;
	json.AddBool("ok", true);
	SendJson(conn, json.String());
}

void Fox_Http::HandleKey(Fox_Io::Conn& conn, const Request& req)
{
	if (!JsonObjectBody(req.body)) {
		Fox_Json_Object json;
		json.AddString("error", "invalid json");
		SendResponse(conn, "400 Bad Request", "application/json", json.String());
		return;
	}

	int code = -1;
	std::string name = JsonString(req.body, "key");
	if (!name.empty())
		code = Fox_Remote_Input::KeyCodeFromName(name);
	long raw = 0;
	if (code < 0 && JsonInt(req.body, "code", raw))
		code = (int)raw;
	if (code < 0) {
		Fox_Json_Object json;
		json.AddString("error", "unknown key");
		SendResponse(conn, "400 Bad Request", "application/json", json.String());
		return;
	}
	if (!Fox_Remote_Input::Init()) {
		Fox_Json_Object json;
		json.AddString("error", "input unavailable");
		SendResponse(conn, "503 Service Unavailable", "application/json", json.String());
		return;
	}
	std::string action = JsonString(req.body, "action");
	if (action == "down")      Fox_Remote_Input::Key(code, true);
	else if (action == "up")   Fox_Remote_Input::Key(code, false);
	else                       Fox_Remote_Input::KeyTap(code);
	Fox_Json_Object json;
	json.AddBool("ok", true);
	SendJson(conn, json.String());
}
