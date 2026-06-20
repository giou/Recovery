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

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fox_fifo.hpp"
#include "twrp-functions.hpp"
#include "partitions.hpp"
#include "twcommon.h"
#include "openrecoveryscript.hpp"
#include "variables.h"
#include "data.hpp"
#include "twrpRepacker.hpp"
#include "gui/gui.hpp"
#include "gui/gui.h"
#include "gui/pages.hpp"
#include "twinstall.h"

#ifdef OF_ENABLE_WLAN
#include "wlan.hpp"
#endif

#ifdef OF_ENABLE_WLAN
#include "../fox_webserver/fox_httpd.hpp"
#include "../fox_webserver/fox_ttyd.hpp"
#endif
#include "fox_remote_input.hpp"
#include "minuitwrp/minui.h"

std::vector<std::string> Fox_Fifo::args;

void Fox_Fifo::Set_Args(const std::vector<std::string>& a) {
	args = a;
}

// Returns true if 'flag' (e.g. "--compress") appears anywhere in a, removing it.
static bool take_flag(std::vector<std::string>& a, const std::string& flag) {
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] == flag) {
			a.erase(a.begin() + i);
			return true;
		}
	}
	return false;
}

// Returns the value of an option (e.g. "--name foo") and removes both tokens.
// Returns empty string if not present.
static std::string take_option(std::vector<std::string>& a, const std::string& opt) {
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] == opt) {
			std::string val;
			if (i + 1 < a.size()) {
				val = a[i + 1];
				a.erase(a.begin() + i, a.begin() + i + 2);
			} else {
				a.erase(a.begin() + i);
			}
			return val;
		}
	}
	return "";
}

static std::string normalize_partition_spec(const std::string& raw) {
	if (raw.empty() || raw[0] == '/')
		return raw;
	return "/" + raw;
}

static std::string display_name_without_size(const std::string& display_name) {
	size_t pos = display_name.find(" (");
	if (pos == std::string::npos)
		return display_name;
	return display_name.substr(0, pos);
}

static std::string lower_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return value;
}

static std::string read_text_file(const std::string& path) {
	std::ifstream input(path.c_str());
	if (!input.is_open())
		return "";
	std::ostringstream out;
	out << input.rdbuf();
	return out.str();
}

bool Fox_Fifo::Resolve_Partition_List(const std::vector<std::string>& specs,
                                     std::string& list_out) {
	list_out.clear();
	std::vector<PartitionList> backup_partitions;
	PartitionManager.Get_Partition_List("backup", &backup_partitions);

	for (const auto& raw : specs) {
		std::string path = normalize_partition_spec(raw);
		bool found = false;
		for (const auto& part : backup_partitions) {
			if (part.Mount_Point == path || part.Mount_Point == raw ||
			    display_name_without_size(part.Display_Name) == raw) {
				list_out += part.Mount_Point + ";";
				found = true;
				break;
			}
		}
		if (!found) {
			gui_print("fox: unknown partition '%s'\n", raw.c_str());
			return false;
		}
	}
	return true;
}

bool Fox_Fifo::Resolve_Storage(const std::string& spec, std::string& path_out) {
	path_out.clear();
	if (spec.empty())
		return false;
	if (spec[0] == '/') {
		path_out = spec;
		return true;
	}

	std::vector<PartitionList> storage_list;
	PartitionManager.Get_Partition_List("storage", &storage_list);

	std::string wanted = lower_copy(spec);
	for (const auto& storage : storage_list) {
		std::string path = storage.Mount_Point;
		std::string display = lower_copy(display_name_without_size(storage.Display_Name));
		std::string mount = lower_copy(path);

		bool matched = path == spec || mount == wanted || display == wanted;
		if (!matched && (wanted == "internal" || wanted == "emulated"))
			matched = mount == "/sdcard" || mount == "/data/media" || mount == "/data/media/0";
		if (!matched && (wanted == "microsd" || wanted == "sdcard" || wanted == "sd"))
			matched = mount == "/external_sd" || mount == "/external_sdcard" ||
			          mount == "/sdcard1";
		if (!matched && (wanted == "otg" || wanted == "usb" || wanted == "usb-otg"))
			matched = mount == "/usb_otg" || mount == "/usb-otg";

		if (matched) {
			path_out = path;
			return true;
		}
	}
	return false;
}

void Fox_Fifo::Emit_Partition_List(const std::string& list_type, bool enrich) {
	std::vector<PartitionList> parts;
	PartitionManager.Get_Partition_List(list_type, &parts);

	gui_print("FOX_LIST_BEGIN\n");
	for (const auto& part : parts) {
		int mounted = PartitionManager.Is_Mounted_By_Path(part.Mount_Point) ? 1 : 0;
		std::string extra;
		if (enrich) {
			TWPartition* p = PartitionManager.Find_Partition_By_Path(part.Mount_Point);
			if (p) {
				extra = "fs=" + p->Current_File_System;
				if (p->Can_Repair())
					extra += ",repair";
				if (p->Can_Resize())
					extra += ",resize";
			}
		}
		// Tab-delimited record: mount_point \t display_name \t mounted \t extra
		gui_print("%s\t%s\t%d\t%s\n", part.Mount_Point.c_str(),
		          part.Display_Name.c_str(), mounted, extra.c_str());
	}
	gui_print("FOX_LIST_END\n");
}

int Fox_Fifo::Cmd_Mount(const std::vector<std::string>& a, bool unmount) {
	if (a.empty()) {
		// No partitions: show what is mountable and its current mount state.
		Emit_Partition_List("mount", false);
		return 0;
	}
	int rc = 0;
	for (const auto& path : a) {
		std::string p = path;
		if (!p.empty() && p[0] != '/')
			p = "/" + p;
		bool ok = unmount ? PartitionManager.UnMount_By_Path(p, true)
		                  : PartitionManager.Mount_By_Path(p, true);
		if (!ok) {
			rc = 1;
		} else {
			gui_print("%s %s\n", unmount ? "Unmounted" : "Mounted", p.c_str());
		}
	}
	return rc;
}

int Fox_Fifo::Cmd_Flash(const std::vector<std::string>& argv) {
	std::vector<std::string> a = argv;
	// Signature verification, reflash-after-flash and unmount toggles. Each only
	// overrides the persistent (UI-configured) value when its flag is supplied,
	// so omitting a flag keeps the same default the GUI would use.
	bool no_verify = take_flag(a, "--no-verify");
	bool verify = take_flag(a, "--verify");
	bool reflash = take_flag(a, "--reflash-orangefox") || take_flag(a, "--reflash");
	bool no_reflash = take_flag(a, "--no-reflash");
	bool unmount_system = take_flag(a, "--unmount-system");
	bool unmount_vendor = take_flag(a, "--unmount-vendor");

	if (a.empty()) {
		gui_print("fox: flash requires a zip path\n");
		return 2;
	}

	if (no_verify)
		DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, 0);
	else if (verify)
		DataManager::SetValue(TW_SIGNED_ZIP_VERIFY_VAR, 1);
	if (reflash)
		DataManager::SetValue(TW_AUTO_REFLASHTWRP_VAR, 1);
	else if (no_reflash)
		DataManager::SetValue(TW_AUTO_REFLASHTWRP_VAR, 0);
	if (unmount_system)
		DataManager::SetValue(TW_UNMOUNT_SYSTEM, 1);
	if (unmount_vendor)
		DataManager::SetValue(TW_UNMOUNT_VENDOR, 1);

	int rc = 0;
	for (const auto& zip : a) {
		// Call the tested installer directly so zip paths containing spaces
		// remain space-safe over the new argv protocol.
		int wipe_cache = 0;
		if (TWinstall_zip(zip.c_str(), &wipe_cache) != 0)
			rc = 1;
	}

	// Reflash OrangeFox after a successful flash when enabled (default mirrors
	// the GUI's tw_auto_reflashtwrp). The direct installer path above does not
	// trigger the auto-reflash that the GUI flash flow performs, so do it here.
	if (rc == 0 && DataManager::GetIntValue(TW_AUTO_REFLASHTWRP_VAR) != 0) {
		twrpRepacker repacker;
		if (!repacker.Flash_Current_Twrp())
			gui_print("fox: warning: OrangeFox reflash failed\n");
	}
	return rc;
}

int Fox_Fifo::Cmd_Backup(const std::vector<std::string>& argv) {
	std::vector<std::string> a = argv;
	std::string name = take_option(a, "--name");
	std::string path = take_option(a, "--path");
	std::string storage = take_option(a, "--storage");
	bool compress = take_flag(a, "--compress") || take_flag(a, "--compression");
	bool no_digest = take_flag(a, "--no-digest") ||
	                 take_flag(a, "--disable-digest-generation");

	if (a.empty()) {
		// No partitions: show what can be backed up.
		Emit_Partition_List("backup", false);
		return 0;
	}

	std::string backup_list;
	if (!Resolve_Partition_List(a, backup_list))
		return 1;

	// Make sure storages are mounted before resolving the destination, so a real
	// persistent location is available (mirrors Cmd_Restore). Without this the
	// default tw_storage_path can still point at "/" (tmpfs), where a backup
	// completes instantly and silently vanishes on reboot.
	PartitionManager.Mount_All_Storage();

	// Default the destination to internal storage when the caller did not pick a
	// --storage or --path, so "fox backup data" lands on Internal Storage rather
	// than wherever tw_storage_path happens to point.
	bool storage_defaulted = false;
	if (storage.empty() && path.empty()) {
		storage = "internal";
		storage_defaulted = true;
	}

	if (!storage.empty()) {
		if (!Resolve_Storage(storage, path)) {
			if (!storage_defaulted) {
				gui_print("fox: unknown storage '%s' (try 'fox storages')\n", storage.c_str());
				return 1;
			}
			// Internal storage could not be resolved by alias; fall back to the
			// current storage path so the backup still lands somewhere real.
			path = DataManager::GetCurrentStoragePath();
		}
	}
	if (!path.empty()) {
		DataManager::SetValue("tw_storage_path", path);
		DataManager::SetBackupFolder();
	}

	// Guard: refuse to back up to a destination whose partition is not mounted.
	// Writing into an unmounted mount point lands on the underlying tmpfs (RAM),
	// where the backup finishes instantly and is silently lost on reboot. We
	// only check paths that map to a known partition, so an arbitrary --path
	// outside the partition table is left to the caller.
	std::string dest = path.empty() ? DataManager::GetCurrentStoragePath() : path;
	if (!dest.empty() && PartitionManager.Find_Partition_By_Path(dest) != NULL &&
	    !PartitionManager.Is_Mounted_By_Path(dest)) {
		gui_print("fox: backup destination '%s' is not mounted; aborting.\n", dest.c_str());
		gui_print("fox: mount it first, run 'fox decrypt' if storage is encrypted, "
		          "or pass --storage/--path.\n");
		return 1;
	}

	DataManager::SetValue(TW_USE_COMPRESSION_VAR, compress ? 1 : 0);
	DataManager::SetValue(TW_SKIP_DIGEST_GENERATE_VAR, no_digest ? 1 : 0);
	if (!name.empty())
		DataManager::SetValue(TW_BACKUP_NAME, name);
	else
		// Generate a real date/build-based name now. Setting the literal
		// "(Auto Generate)" only works in the GUI, where Run_Backup matches it
		// against the localized gui_lookup("auto_generate") string; from the CLI
		// that comparison fails and the literal becomes the folder name.
		TWFunc::Auto_Generate_Backup_Name();
	DataManager::SetValue("tw_backup_list", backup_list);

	if (!PartitionManager.Run_Backup(false)) {
		// Distinguish a user cancellation (via the fox cancel channel or the GUI)
		// from a genuine failure so the client can report it cleanly.
		if (PartitionManager.Check_Backup_Cancel() != 0) {
			gui_print("Backup cancelled\n");
			return 130;  // 128 + SIGINT, the shell convention for Ctrl+C
		}
		gui_print("fox: backup failed\n");
		return 1;
	}
	gui_print("Backup complete\n");
	return 0;
}

int Fox_Fifo::Cmd_Restore(const std::vector<std::string>& argv) {
	std::vector<std::string> a = argv;
	std::string name = take_option(a, "--name");
	std::string path = take_option(a, "--path");
	if (name.empty())
		name = path;
	bool skip_digest = take_flag(a, "--no-digest-check") ||
	                   take_flag(a, "--disable-digest-check");

	if (name.empty()) {
		gui_print("fox: restore requires --name <backup name> or --path <absolute path>\n");
		return 2;
	}

	PartitionManager.Mount_All_Storage();
	DataManager::SetValue(TW_SKIP_DIGEST_CHECK_VAR, skip_digest ? 1 : 0);

	// Resolve the backup folder: absolute path used directly, otherwise resolved
	// against the backups folder of each mounted storage (mirrors ORS restore).
	std::string folder = name;
	if (folder[0] != '/') {
		std::vector<PartitionList> storage_list;
		PartitionManager.Get_Partition_List("storage", &storage_list);
		bool found = false;
		for (const auto& s : storage_list) {
			if (PartitionManager.Is_Mounted_By_Path(s.Mount_Point)) {
				DataManager::SetValue("tw_storage_path", s.Mount_Point);
				std::string folder_var;
				DataManager::GetValue(TW_BACKUPS_FOLDER_VAR, folder_var);
				std::string candidate = folder_var + "/" + name;
				if (TWFunc::Path_Exists(candidate)) {
					folder = candidate;
					found = true;
					break;
				}
			}
		}
		if (!found) {
			gui_print("fox: unable to locate backup '%s'\n", name.c_str());
			return 1;
		}
	} else {
		folder += (folder[folder.size() - 1] == '/') ? "." : "/.";
	}

	if (!TWFunc::Path_Exists(folder)) {
		gui_print("fox: unable to locate backup '%s'\n", folder.c_str());
		return 1;
	}

	DataManager::SetValue("tw_restore", folder);
	PartitionManager.Set_Restore_Files(folder);

	int is_encrypted = 0;
	DataManager::GetValue("tw_restore_encrypted", is_encrypted);
	if (is_encrypted) {
		gui_print("fox: cannot restore an encrypted backup from the CLI\n");
		return 1;
	}

	std::string available;
	DataManager::GetValue("tw_restore_list", available);

	std::string selected;
	if (a.empty()) {
		// No explicit partitions: restore everything in the backup.
		selected = available;
	} else {
		for (const auto& raw : a) {
			std::string p = raw;
			if (!p.empty() && p[0] != '/')
				p = "/" + p;
			if (available.find(p + ";") == std::string::npos) {
				gui_print("fox: partition '%s' is not present in this backup\n", p.c_str());
				return 1;
			}
			selected += p + ";";
		}
	}
	DataManager::SetValue("tw_restore_selected", selected);

	if (!PartitionManager.Run_Restore(folder)) {
		gui_print("fox: restore failed\n");
		return 1;
	}
	gui_print("Restore complete\n");
	return 0;
}

int Fox_Fifo::Cmd_Decrypt(const std::vector<std::string>& argv) {
	std::vector<std::string> a = argv;
	std::string user = take_option(a, "--user");
	if (a.empty()) {
		gui_print("fox: decrypt requires a password\n");
		return 2;
	}
	// The new protocol delivers the password intact, so spaces are supported
	// here (unlike the legacy space-split ORS path).
	std::string password = a[0];
	int user_id = user.empty() ? 0 : atoi(user.c_str());

	gui_print("Attempting to decrypt data partition...\n");
	if (PartitionManager.Decrypt_Device(password, user_id) != 0) {
		gui_print("fox: decryption failed\n");
		return 1;
	}
	gui_print("Decryption successful\n");
	return 0;
}

int Fox_Fifo::Cmd_Sideload(const std::vector<std::string>& /*a*/) {
	// Reuse the tested ORS sideload handler.
	return OpenRecoveryScript::Run_ORS_Line("sideload");
}

static std::string fallback_property(const std::string& first, const std::string& second) {
	std::string value = TWFunc::Fox_Property_Get(first);
	if (value.empty())
		value = TWFunc::System_Property_Get(second);
	return value;
}

static void print_status_kv(const std::string& key, const std::string& value) {
	gui_print("%s=%s\n", key.c_str(), value.c_str());
}

static void print_status_kv(const std::string& key, int value) {
	gui_print("%s=%d\n", key.c_str(), value);
}

int Fox_Fifo::Cmd_Status(const std::vector<std::string>& /*a*/) {
	int is_encrypted = 0;
	int is_decrypted = 0;
	int fox_encrypted = 0;
	DataManager::GetValue(TW_IS_ENCRYPTED, is_encrypted);
	DataManager::GetValue(TW_IS_DECRYPTED, is_decrypted);
	DataManager::GetValue(FOX_ENCRYPTED_DEVICE, fox_encrypted);
	int storage_encrypted = (is_encrypted || fox_encrypted || PartitionManager.Storage_Is_Encrypted()) ? 1 : 0;

	std::string device_model = TWFunc::Fox_Property_Get("ro.orangefox.device_model");
	std::string device_code = TWFunc::Fox_Property_Get("ro.product.device");
	if (device_code.empty())
		device_code = TWFunc::Fox_Property_Get("ro.product.system.device");
	if (device_model.empty())
		device_model = device_code;

	std::string fingerprint = TWFunc::Fox_Property_Get("orangefox.system.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.system.build.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.build.fingerprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.build.thumbprint");
	if (fingerprint.empty())
		fingerprint = TWFunc::System_Property_Get("ro.vendor.build.thumbprint");

	std::string rom_name = TWFunc::System_Property_Get("ro.build.display.id");
	if (rom_name.empty())
		rom_name = TWFunc::System_Property_Get("ro.build.id");
	std::string rom_incremental = TWFunc::System_Property_Get("ro.build.version.incremental");
	std::string rom_sdk = fallback_property("orangefox.rom.sdk", "ro.build.version.sdk");
	std::string android_release = TWFunc::System_Property_Get("ro.build.version.release");

	gui_print("FOX_STATUS_BEGIN\n");
	print_status_kv("release", FOX_BUILD);
	print_status_kv("recovery_version", TW_VERSION_STR);
	print_status_kv("variant", FOX_VARIANT);
	print_status_kv("build_type", FOX_BUILD_TYPE);
	print_status_kv("branch", OF_CURRENT_BRANCH);
	print_status_kv("codebase_sdk", TWFunc::Fox_Property_Get("ro.build.version.sdk"));
	print_status_kv("codebase", FOX_CURRENT_DEV_STR);
	print_status_kv("build_date", DataManager::GetStrValue("FOX_BUILD_DATE_REAL"));
	print_status_kv("device_model", device_model);
	print_status_kv("device_code", device_code);
	print_status_kv("storage_encrypted", storage_encrypted);
	print_status_kv("storage_decrypted", is_decrypted);
	print_status_kv("dynamic_partitions", TWFunc::Fox_Property_Get("orangefox.super.partition") == "true" ? 1 : 0);
	print_status_kv("boot_slot", TWFunc::Fox_Property_Get("ro.boot.slot_suffix"));
	print_status_kv("virtual_ab", TWFunc::Fox_Property_Get("ro.virtual_ab.enabled") == "true" ? 1 : 0);
	print_status_kv("kernel", TWFunc::Fox_Property_Get("ro.orangefox.kernel"));
	print_status_kv("rom_name", rom_name);
	print_status_kv("rom_incremental", rom_incremental);
	print_status_kv("rom_sdk", rom_sdk);
	print_status_kv("android_release", android_release);
	print_status_kv("rom_miui", TWFunc::Fox_Property_Get("orangefox.miui.rom") == "1" ? 1 : 0);
	print_status_kv("fingerprint", fingerprint);
	gui_print("FOX_STATUS_END\n");
	return 0;
}

int Fox_Fifo::Cmd_Reboot(const std::vector<std::string>& a) {
	std::string target = a.empty() ? "system" : a[0];
	return OpenRecoveryScript::Run_ORS_Line("reboot " + target);
}

int Fox_Fifo::Cmd_Wlan(const std::vector<std::string>& a) {
#ifndef OF_ENABLE_WLAN
	(void)a;
	gui_print("fox: WLAN support is not enabled in this build\n");
	return 1;
#else
	if (!Wlan::IsEnabled() && (a.empty() || a[0] != "enable")) {
		gui_print("fox: WLAN is not enabled (try 'fox wlan enable')\n");
		return 1;
	}
	if (a.empty()) {
		gui_print("fox: wlan requires a subcommand (enable|disable|list|connect|saved|connect-saved|forget|info|test)\n");
		return 2;
	}
	const std::string& sub = a[0];
	if (sub == "enable")
		return Wlan::Enable() ? 0 : 1;
	if (sub == "disable")
		return Wlan::Disable() ? 0 : 1;
	if (sub == "list") {
		if (!Wlan::Scan())
			return 1;
		std::string list = read_text_file("/tmp/wlan/list.txt");
		gui_print("%s\n", list.c_str());
		return 0;
	}
	if (sub == "connect") {
		if (a.size() < 2) {
			gui_print("fox: wlan connect requires an SSID\n");
			return 2;
		}
		// Populate scan metadata so Connect() can resolve the encryption type.
		Wlan::Scan();
		DataManager::SetValue("wlanselectedid", a[1]);
		std::string pass;
		std::vector<std::string> rest(a.begin() + 1, a.end());
		pass = take_option(rest, "--password");
		DataManager::SetValue("wlan_password", pass);
		return Wlan::Connect() ? 0 : 1;
	}
	if (sub == "saved") {
		if (!Wlan::RefreshSaved())
			return 1;
		std::string list = read_text_file("/tmp/wlan/saved.txt");
		gui_print("%s\n", list.c_str());
		return 0;
	}
	if (sub == "connect-saved") {
		if (a.size() < 2) {
			gui_print("fox: wlan connect-saved requires an SSID\n");
			return 2;
		}
		DataManager::SetValue("wlanselectedid", a[1]);
		return Wlan::ConnectSaved() ? 0 : 1;
	}
	if (sub == "forget") {
		if (a.size() < 2) {
			gui_print("fox: wlan forget requires an SSID\n");
			return 2;
		}
		DataManager::SetValue("wlanselectedid", a[1]);
		return Wlan::ForgetSaved() ? 0 : 1;
	}
	if (sub == "info") {
		if (!Wlan::Info())
			return 1;
		std::string info = read_text_file("/tmp/wlan/info.txt");
		gui_print("%s\n", info.c_str());
		return 0;
	}
	if (sub == "test") {
		if (!Wlan::TestConnection())
			return 1;
		for (int i = 1; i <= 5; i++) {
			std::string line = DataManager::GetStrValue("wlan_test_line" + std::to_string(i));
			if (!line.empty())
				gui_print("%s\n", line.c_str());
		}
		return 0;
	}
	gui_print("fox: unknown wlan subcommand '%s'\n", sub.c_str());
	return 2;
#endif // OF_ENABLE_WLAN
}

int Fox_Fifo::Cmd_Internal(const std::vector<std::string>& a) {
	if (a.empty()) {
		gui_print("fox: internal requires a subcommand (get|set|go)\n");
		return 2;
	}
	const std::string& sub = a[0];
	if (sub == "get") {
		if (a.size() < 2) {
			gui_print("fox: internal get requires a variable name\n");
			return 2;
		}
		std::string value;
		DataManager::GetValue(a[1], value);
		gui_print("%s\n", value.c_str());
		return 0;
	}
	if (sub == "set") {
		if (a.size() < 2) {
			gui_print("fox: internal set requires a variable name\n");
			return 2;
		}
		std::string value = (a.size() > 2) ? a[2] : "";
		DataManager::SetValue(a[1], value);
		return 0;
	}
	if (sub == "go") {
		if (a.size() < 2) {
			gui_print("fox: internal go requires a page name\n");
			return 2;
		}
		gui_changePage(a[1]);
		return 0;
	}
	gui_print("fox: unknown internal subcommand '%s'\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Ors(const std::vector<std::string>& a) {
	if (a.empty()) {
		gui_print("fox: ors requires a script file path\n");
		return 2;
	}
	return OpenRecoveryScript::Run_ORS_File(a[0]);
}

int Fox_Fifo::Cmd_Wipe(const std::vector<std::string>& a) {
	if (a.empty()) {
		// No targets: show what can be wiped.
		Emit_Partition_List("wipe", false);
		return 0;
	}
	int rc = 0;
	for (const auto& raw : a) {
		std::string lower = raw;
		for (auto& c : lower) c = tolower(c);
		if (lower == "dalvik" || lower == "cache" || lower == "dalvik-cache") {
			if (!PartitionManager.Wipe_Dalvik_Cache())
				rc = 1;
			else
				gui_print("Wiped dalvik/cache\n");
			continue;
		}
		std::string p = normalize_partition_spec(raw);
		if (!PartitionManager.Find_Partition_By_Path(p)) {
			gui_print("fox: unknown partition '%s'\n", raw.c_str());
			rc = 1;
			continue;
		}
		if (!PartitionManager.Wipe_By_Path(p))
			rc = 1;
		else
			gui_print("Wiped %s\n", p.c_str());
	}
	return rc;
}

int Fox_Fifo::Cmd_Format_Data(const std::vector<std::string>& argv) {
	std::vector<std::string> a = argv;
	bool confirmed = take_flag(a, "--yes") || take_flag(a, "--confirm");
	if (!confirmed) {
		gui_print("fox: format_data erases ALL data and removes encryption.\n");
		gui_print("fox: re-run with --yes to confirm: fox format_data --yes\n");
		return 2;
	}
	gui_print("Formatting data...\n");
	if (!PartitionManager.Format_Data()) {
		gui_print("fox: format data failed\n");
		return 1;
	}
	gui_print("Data formatted\n");
	return 0;
}

int Fox_Fifo::Cmd_Reflash(const std::vector<std::string>& /*a*/) {
	gui_print("Reflashing OrangeFox...\n");
	twrpRepacker repacker;
	if (!repacker.Flash_Current_Twrp()) {
		gui_print("fox: reflash failed\n");
		return 1;
	}
	gui_print("OrangeFox reflashed\n");
	return 0;
}

int Fox_Fifo::Cmd_Mtp(const std::vector<std::string>& a) {
	std::string sub = a.empty() ? "status" : a[0];
	if (sub == "status") {
		int enabled = DataManager::GetIntValue("tw_mtp_enabled");
		gui_print("mtp_enabled=%d\n", enabled ? 1 : 0);
		return 0;
	}
	if (sub == "enable") {
		if (!PartitionManager.Enable_MTP()) {
			gui_print("fox: failed to enable MTP\n");
			return 1;
		}
		gui_print("MTP enabled\n");
		return 0;
	}
	if (sub == "disable") {
		if (!PartitionManager.Disable_MTP()) {
			gui_print("fox: failed to disable MTP\n");
			return 1;
		}
		gui_print("MTP disabled\n");
		return 0;
	}
	gui_print("fox: unknown mtp subcommand '%s' (status|enable|disable)\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Log(const std::vector<std::string>& /*a*/) {
	// Mirror the OrangeFox log export: copy the recovery log into the Fox logs
	// folder and compress it (pigz produces <name>.zip), as in Run_Before_Reboot.
	TWFunc::Create_Dir_Recursive(Fox_Logs_Dir, 0777);

	std::string log_file = Fox_Logs_Dir + "/recovery_" +
	                       std::to_string((long long)time(NULL)) + ".log";
	if (TWFunc::copy_file("/tmp/recovery.log", log_file, 0777) != 0) {
		gui_print("fox: unable to read /tmp/recovery.log\n");
		return 1;
	}

	std::string pigz = Fox_Bin_Dir + "/pigz";
	if (TWFunc::Path_Exists(pigz)) {
		TWFunc::Exec_Cmd(pigz + " -K --best " + log_file);
		std::string zip = log_file + ".zip";
		TWFunc::set_media_rw_permissions(zip);
		gui_print("Saved log to %s\n", zip.c_str());
	} else {
		TWFunc::set_media_rw_permissions(log_file);
		gui_print("Saved log to %s\n", log_file.c_str());
	}
	return 0;
}

int Fox_Fifo::Cmd_Partition(const std::vector<std::string>& a) {
	if (a.empty()) {
		// No path: list partitions with file system and capability flags.
		Emit_Partition_List("part_option", true);
		return 0;
	}
	if (a.size() < 2) {
		gui_print("fox: partition requires <path> <repair|resize|change-fs <fs>|wipe>\n");
		return 2;
	}

	std::string path = normalize_partition_spec(a[0]);
	const std::string& op = a[1];
	if (!PartitionManager.Find_Partition_By_Path(path)) {
		gui_print("fox: unknown partition '%s'\n", a[0].c_str());
		return 1;
	}

	if (op == "repair") {
		if (!PartitionManager.Repair_By_Path(path, true))
			return 1;
		gui_print("Repaired %s\n", path.c_str());
		return 0;
	}
	if (op == "resize") {
		if (!PartitionManager.Resize_By_Path(path, true))
			return 1;
		gui_print("Resized %s\n", path.c_str());
		return 0;
	}
	if (op == "change-fs") {
		if (a.size() < 3) {
			gui_print("fox: partition change-fs requires a file system (e.g. ext4)\n");
			return 2;
		}
		if (!PartitionManager.Wipe_By_Path(path, a[2]))
			return 1;
		gui_print("Changed %s to %s\n", path.c_str(), a[2].c_str());
		return 0;
	}
	if (op == "wipe") {
		if (!PartitionManager.Wipe_By_Path(path))
			return 1;
		gui_print("Wiped %s\n", path.c_str());
		return 0;
	}
	gui_print("fox: unknown partition op '%s' (repair|resize|change-fs|wipe)\n", op.c_str());
	return 2;
}

// Collect *.zip files in 'dir' into 'out' (full paths).
static void collect_zip_files(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		std::string name = ent->d_name;
		if (name == "." || name == "..")
			continue;
		std::string path = dir + "/" + name;
		if (name.size() > 4 && name.substr(name.size() - 4) == ".zip")
			out.push_back(path);
		else if (depth < 4)
			collect_zip_files(path, out, depth + 1);
	}
	closedir(d);
}

int Fox_Fifo::Cmd_Addons(const std::vector<std::string>& a) {
	std::vector<std::string> dirs = { Fox_Home_Files, FFiles_dir };

	if (a.empty() || a[0] == "list") {
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		gui_print("FOX_LIST_BEGIN\n");
		for (const auto& z : zips) {
			std::string name = z.substr(z.find_last_of('/') + 1);
			// mount_point column carries the path; display the file name.
			gui_print("%s\t%s\t0\t\n", z.c_str(), name.c_str());
		}
		gui_print("FOX_LIST_END\n");
		return 0;
	}

	if (a[0] == "install") {
		if (a.size() < 2) {
			gui_print("fox: addons install requires a zip name or path\n");
			return 2;
		}
		int rc = 0;
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		for (size_t i = 1; i < a.size(); i++) {
			std::string target = a[i];
			if (target.find('/') == std::string::npos) {
				// Resolve a bare name against the known addon folders.
				bool found = false;
				std::string wanted = target;
				if (wanted.size() <= 4 || wanted.substr(wanted.size() - 4) != ".zip")
					wanted += ".zip";
				for (const auto& zip : zips) {
					std::string basename = zip.substr(zip.find_last_of('/') + 1);
					if (basename == wanted || basename == target) {
						target = zip;
						found = true;
						break;
					}
				}
				if (!found) {
					gui_print("fox: addon '%s' not found\n", a[i].c_str());
					rc = 1;
					continue;
				}
			}
			int wipe_cache = 0;
			if (TWinstall_zip(target.c_str(), &wipe_cache) != 0)
				rc = 1;
		}
		return rc;
	}

	gui_print("fox: unknown addons subcommand '%s' (list|install)\n", a[0].c_str());
	return 2;
}

int Fox_Fifo::Cmd_Screencap(const std::vector<std::string>& a) {
	// Capture the live framebuffer to a PNG file (tmpfs by default) so a client
	// can read it back over an ADB sync/pull -- no network stack involved.
	std::string path = a.empty() ? "/tmp/.fox_screen.png" : a[0];
	if (gr_save_screenshot(path.c_str()) != 0) {
		gui_print("fox: screencap failed\n");
		return 1;
	}
	gui_print("%s\n", path.c_str());
	return 0;
}

int Fox_Fifo::Cmd_Input(const std::vector<std::string>& a) {
	std::string output;
	int code = Fox_Remote_Input::RunCommand(a, &output);
	if (!output.empty())
		gui_print("%s", output.c_str());
	return code;
}

// fox webserver {start [--port N]|stop|status|terminal ...}: control the HTTP
// server (dashboard + REST/job control plane) on its own. The remote-control
// plane (screen + input) is governed separately by `fox control`.
int Fox_Fifo::Cmd_Webserver(const std::vector<std::string>& a) {
#ifndef OF_ENABLE_WLAN
	(void)a;
	gui_print("fox: webserver is not available in this build (OF_ENABLE_WLAN=0)\n");
	gui_print("fox: direct WebUSB remains available via screencap/input/screenstream FIFO commands\n");
	return 1;
#else
	std::string sub = a.empty() ? "status" : a[0];

	if (sub == "start") {
		std::vector<std::string> rest(a.begin() + (a.empty() ? 0 : 1), a.end());
		std::string port_s = take_option(rest, "--port");
		int port = port_s.empty() ? 80 : atoi(port_s.c_str());
		if (Fox_Httpd::IsRunning()) {
			gui_print("Web server already running on %s:%d\n",
			          Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
			gui_print("token=%s\n", Fox_Httpd::Token().c_str());
			return 0;
		}
		if (!Fox_Httpd::Start(port)) {
			gui_print("fox: failed to start web server\n");
			return 1;
		}
		gui_print("Web server started\n");
		gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
		gui_print("port=%d\n", Fox_Httpd::Port());
		gui_print("token=%s\n", Fox_Httpd::Token().c_str());
		gui_print("control=%d\n", Fox_Httpd::ControlEnabled() ? 1 : 0);
		return 0;
	}
	if (sub == "stop") {
		Fox_Ttyd::Stop();
		Fox_Httpd::Stop();
		gui_print("Web server stopped\n");
		return 0;
	}
	if (sub == "status") {
		gui_print("FOX_STATUS_BEGIN\n");
		gui_print("running=%d\n", Fox_Httpd::IsRunning() ? 1 : 0);
		gui_print("control=%d\n", Fox_Httpd::ControlEnabled() ? 1 : 0);
		if (Fox_Httpd::IsRunning()) {
			gui_print("address=%s\n", Fox_Httpd::LocalAddress().c_str());
			gui_print("port=%d\n", Fox_Httpd::Port());
			gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
			gui_print("token=%s\n", Fox_Httpd::Token().c_str());
		}
		gui_print("FOX_STATUS_END\n");
		return 0;
	}
	if (sub == "terminal" || sub == "ttyd") {
		std::string action = a.size() > 1 ? a[1] : "status";
		if (action == "start") {
			std::vector<std::string> rest(a.begin() + 2, a.end());
			std::string port_s = take_option(rest, "--port");
			int port = port_s.empty() ? 7681 : atoi(port_s.c_str());
			if (!Fox_Ttyd::Start(port)) {
				gui_print("fox: failed to start ttyd\n");
				return 1;
			}
			gui_print("ttyd started\n");
			gui_print("port=%d\n", Fox_Ttyd::Port());
			gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Ttyd::Port());
			gui_print("shell=%s\n", Fox_Ttyd::Shell().c_str());
			return 0;
		}
		if (action == "stop") {
			Fox_Ttyd::Stop();
			gui_print("ttyd stopped\n");
			return 0;
		}
		if (action == "status") {
			gui_print("FOX_TTYD_BEGIN\n");
			gui_print("running=%d\n", Fox_Ttyd::IsRunning() ? 1 : 0);
			gui_print("port=%d\n", Fox_Ttyd::Port());
			gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Ttyd::Port());
			gui_print("shell=%s\n", Fox_Ttyd::Shell().c_str());
			gui_print("FOX_TTYD_END\n");
			return 0;
		}
		gui_print("fox: unknown webserver terminal subcommand '%s' (start|stop|status)\n", action.c_str());
		return 2;
	}
	gui_print("fox: unknown webserver subcommand '%s' (start|stop|status|terminal)\n", sub.c_str());
	return 2;
#endif
}

// fox control {start|stop|status}: toggle the remote-control plane (screen
// mirroring + input injection) exposed over the HTTP server, independently of
// whether the server is running. Does not gate the `fox input`/`fox screencap`
// CLI commands (those need adb shell access already).
int Fox_Fifo::Cmd_Control(const std::vector<std::string>& a) {
#ifndef OF_ENABLE_WLAN
	(void)a;
	gui_print("fox: HTTP control plane is not available in this build (OF_ENABLE_WLAN=0)\n");
	gui_print("fox: direct WebUSB remains available via input/screencap/screenstream FIFO commands\n");
	return 1;
#else
	std::string sub = a.empty() ? "status" : a[0];

	if (sub == "start" || sub == "enable" || sub == "on") {
		Fox_Httpd::SetControlEnabled(true);
		gui_print("Remote control enabled\n");
		return 0;
	}
	if (sub == "stop" || sub == "disable" || sub == "off") {
		Fox_Httpd::SetControlEnabled(false);
		gui_print("Remote control disabled\n");
		return 0;
	}
	if (sub == "status") {
		gui_print("FOX_STATUS_BEGIN\n");
		gui_print("control=%d\n", Fox_Httpd::ControlEnabled() ? 1 : 0);
		gui_print("input_ready=%d\n", Fox_Remote_Input::IsReady() ? 1 : 0);
		gui_print("FOX_STATUS_END\n");
		return 0;
	}
	gui_print("fox: unknown control subcommand '%s' (start|stop|status)\n", sub.c_str());
	return 2;
#endif
}

int Fox_Fifo::Cmd_Storages(const std::vector<std::string>& /*a*/) {
	PartitionManager.Mount_All_Storage();
	std::vector<PartitionList> storage_list;
	PartitionManager.Get_Partition_List("storage", &storage_list);
	std::string current = DataManager::GetCurrentStoragePath();

	gui_print("FOX_LIST_BEGIN\n");
	for (const auto& storage : storage_list) {
		int selected = storage.Mount_Point == current ? 1 : 0;
		std::string extra = storage.isFiles ? "files" : "block";
		gui_print("%s\t%s\t%d\t%s\n", storage.Mount_Point.c_str(),
		          storage.Display_Name.c_str(), selected, extra.c_str());
	}
	gui_print("FOX_LIST_END\n");
	return 0;
}

int Fox_Fifo::Run_Command() {
	if (args.empty()) {
		gui_print("fox: no command given\n");
		return 2;
	}

	std::string verb = args[0];
	std::vector<std::string> rest(args.begin() + 1, args.end());

	if (verb == "mount")
		return Cmd_Mount(rest, false);
	if (verb == "unmount" || verb == "umount")
		return Cmd_Mount(rest, true);
	if (verb == "flash" || verb == "install")
		return Cmd_Flash(rest);
	if (verb == "backup")
		return Cmd_Backup(rest);
	if (verb == "restore")
		return Cmd_Restore(rest);
	if (verb == "decrypt")
		return Cmd_Decrypt(rest);
	if (verb == "sideload")
		return Cmd_Sideload(rest);
	if (verb == "status")
		return Cmd_Status(rest);
	if (verb == "reboot")
		return Cmd_Reboot(rest);
	if (verb == "wlan")
		return Cmd_Wlan(rest);
	if (verb == "internal")
		return Cmd_Internal(rest);
	if (verb == "wipe")
		return Cmd_Wipe(rest);
	if (verb == "format_data")
		return Cmd_Format_Data(rest);
	if (verb == "reflash")
		return Cmd_Reflash(rest);
	if (verb == "mtp")
		return Cmd_Mtp(rest);
	if (verb == "log")
		return Cmd_Log(rest);
	if (verb == "partition")
		return Cmd_Partition(rest);
	if (verb == "addons")
		return Cmd_Addons(rest);
	if (verb == "storages")
		return Cmd_Storages(rest);
	if (verb == "webserver")
		return Cmd_Webserver(rest);
	if (verb == "control")
		return Cmd_Control(rest);
	if (verb == "input")
		return Cmd_Input(rest);
	if (verb == "screencap")
		return Cmd_Screencap(rest);
	if (verb == "ors")
		return Cmd_Ors(rest);
	if (verb == "ors-cmd") {
		// Run a raw ORS command line (joined back with spaces).
		std::string joined;
		for (size_t i = 0; i < rest.size(); i++) {
			if (i) joined += " ";
			joined += rest[i];
		}
		return OpenRecoveryScript::Run_ORS_Line(joined);
	}

	gui_print("fox: unknown command '%s'\n", verb.c_str());
	return 2;
}
