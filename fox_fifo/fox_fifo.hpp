/*
	Copyright (C) 2024-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _FOX_FIFO_HPP
#define _FOX_FIFO_HPP

#include <string>
#include <vector>

// Fox_Fifo implements the modern "fox" command-line front-end. Unlike the
// legacy ORS-based "twrp" tool, fox passes a NUL-delimited argv vector over a
// dedicated FIFO pair (FOX_INPUT_FILE/FOX_OUTPUT_FILE), which keeps spaces in
// passwords/paths intact and lets the engine report a real exit code.
//
// Fox_Channel parses FIFO argv, stores it with Set_Args(), then dispatches the
// "foxcmd" GUI action which calls Run_Command() on the action thread (so
// gui_print output is mirrored back to the caller).
//
// A few high-frequency remote-control commands are intentionally handled in
// Fox_Channel/Fox_Screen_Stream before the foxcmd action path:
//   fox screencap --base64
//   fox input ...
//   fox screenstream {start [--fps N]|stop|status}
// This keeps WebUSB screen/input traffic off singleaction_page and lets the
// continuous screen stream use FOX_SCREEN_STREAM_FILE while foxin/foxout remain
// available for stop/touch/key commands.
class Fox_Fifo
{
public:
	// Stash the parsed argv before the foxcmd action runs.
	static void Set_Args(const std::vector<std::string>& args);

	// Execute the stashed command. Returns the process exit code that the
	// fox client should exit with (0 == success).
	static int Run_Command();

private:
	static std::vector<std::string> args;

	// Subcommand handlers. Each returns a shell-style exit code.
	static int Cmd_Mount(const std::vector<std::string>& a, bool unmount);
	static int Cmd_Flash(const std::vector<std::string>& a);
	static int Cmd_Backup(const std::vector<std::string>& a);
	static int Cmd_Restore(const std::vector<std::string>& a);
	static int Cmd_Decrypt(const std::vector<std::string>& a);
	static int Cmd_Sideload(const std::vector<std::string>& a);
	static int Cmd_Status(const std::vector<std::string>& a);
	static int Cmd_Reboot(const std::vector<std::string>& a);
	static int Cmd_Wlan(const std::vector<std::string>& a);
	static int Cmd_Internal(const std::vector<std::string>& a);
	static int Cmd_Ors(const std::vector<std::string>& a);
	static int Cmd_Wipe(const std::vector<std::string>& a);
	static int Cmd_Format_Data(const std::vector<std::string>& a);
	static int Cmd_Reflash(const std::vector<std::string>& a);
	static int Cmd_Mtp(const std::vector<std::string>& a);
	static int Cmd_Log(const std::vector<std::string>& a);
	static int Cmd_Partition(const std::vector<std::string>& a);
	static int Cmd_Addons(const std::vector<std::string>& a);
	static int Cmd_Storages(const std::vector<std::string>& a);
	// fox webserver {start|stop|status|terminal ...}: the HTTP server
	// (dashboard + REST/job control plane) and optional ttyd terminal.
	static int Cmd_Webserver(const std::vector<std::string>& a);
	// fox control {start|stop|status}: the remote-control plane (screen + input
	// over HTTP), toggled independently of the server.
	static int Cmd_Control(const std::vector<std::string>& a);
	// fox input {tap|swipe|key|down|move|up}: inject touch/keys via uinput.
	// Usable over a plain `adb shell` (no network stack).
	static int Cmd_Input(const std::vector<std::string>& a);
	// fox screencap [path]: write the framebuffer to a PNG for ADB sync/pull.
	static int Cmd_Screencap(const std::vector<std::string>& a);

	// Build a ;-delimited backup/restore list from explicit partition specs
	// (mount points like "/system" or partition display names). Validates
	// each spec against PartitionManager and prints an error for unknowns.
	// Returns true on success; sets list_out.
	static bool Resolve_Partition_List(const std::vector<std::string>& specs,
	                                   std::string& list_out);
	static bool Resolve_Storage(const std::string& spec, std::string& path_out);

	// Emit a FOX_LIST_BEGIN/END block describing the partitions in the given
	// Get_Partition_List ListType ("mount", "backup", "wipe", "part_option").
	// Each record is "mount_point\tdisplay_name\tmounted(0|1)\textra". When
	// 'enrich' is true, 'extra' carries the file system and repair/resize/wipe
	// capability flags (used for partition management listings).
	static void Emit_Partition_List(const std::string& list_type, bool enrich);
};

#endif // _FOX_FIFO_HPP
