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
#include "fox_updater.hpp"
#include "../fox_adbd.hpp"
#endif

#ifdef OF_ENABLE_WLAN
#include "../fox_webserver/fox_httpd.hpp"
#include "../fox_webserver/fox_httpsd.hpp"
#include "../fox_webserver/fox_tls_cert.hpp"
#include "../fox_webserver/fox_ttyd.hpp"
#include "../fox_secrets/fox_secrets.hpp"
#endif
#include "fox_remote_input.hpp"
#include "fox_command_dispatcher.hpp"
#include "minuitwrp/minui.h"

std::string Fox_Fifo::op;
Json::Value Fox_Fifo::cmd_args;

void Fox_Fifo::Set_Request(const std::string& request_op, const Json::Value& args) {
	op = request_op;
	cmd_args = args;
}

// ---- Typed argument accessors --------------------------------------------
// Handlers read their parameters straight off the request's JSON args object.
// These helpers coerce loosely (a number or bool requested as a string is
// stringified, etc.) so a slightly off-typed client request still works.

// String value for 'key' ("" if absent). Numbers/bools are stringified.
static std::string arg_str(const Json::Value& a, const char* key) {
	if (!a.isObject() || !a.isMember(key))
		return "";
	const Json::Value& v = a[key];
	if (v.isString())
		return v.asString();
	if (v.isIntegral())
		return std::to_string(v.asInt64());
	if (v.isBool())
		return v.asBool() ? "1" : "0";
	return "";
}

// Convenience for the conventional "action" field (the subcommand verb).
static std::string arg_action(const Json::Value& a) {
	return arg_str(a, "action");
}

static bool arg_has(const Json::Value& a, const char* key) {
	return a.isObject() && a.isMember(key);
}

// Bool value for 'key', defaulting to 'dflt' when absent or not coercible.
static bool arg_bool(const Json::Value& a, const char* key, bool dflt = false) {
	if (!arg_has(a, key))
		return dflt;
	const Json::Value& v = a[key];
	if (v.isBool())
		return v.asBool();
	if (v.isIntegral())
		return v.asInt64() != 0;
	if (v.isString()) {
		std::string s = v.asString();
		return s == "1" || s == "true" || s == "yes" || s == "on";
	}
	return dflt;
}

// Integer value for 'key', defaulting to 'dflt' when absent.
static long arg_int(const Json::Value& a, const char* key, long dflt) {
	if (!arg_has(a, key))
		return dflt;
	const Json::Value& v = a[key];
	if (v.isIntegral())
		return (long)v.asInt64();
	if (v.isString())
		return strtol(v.asString().c_str(), nullptr, 10);
	return dflt;
}

// String array for 'key'. A lone string is accepted as a one-element list;
// numeric elements are stringified. Empty when absent.
static std::vector<std::string> arg_array(const Json::Value& a, const char* key) {
	std::vector<std::string> out;
	if (!arg_has(a, key))
		return out;
	const Json::Value& v = a[key];
	if (v.isArray()) {
		for (const Json::Value& e : v) {
			if (e.isString())
				out.push_back(e.asString());
			else if (e.isIntegral())
				out.push_back(std::to_string(e.asInt64()));
		}
	} else if (v.isString()) {
		out.push_back(v.asString());
	}
	return out;
}

// Generate a strong random alphanumeric password. Used by `web password
// --generate`. Sourced from /dev/urandom; the slight modulo bias across a
// 62-char alphabet is irrelevant for a 20-char secret.
static std::string generate_password(size_t len = 20) {
	static const char* cs =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	const size_t n = 62;
	std::string out;
	out.reserve(len);
	FILE* f = fopen("/dev/urandom", "rb");
	for (size_t i = 0; i < len; i++) {
		unsigned char b = 0;
		if (!f || fread(&b, 1, 1, f) != 1)
			b = static_cast<unsigned char>(time(nullptr) + i * 61);
		out.push_back(cs[b % n]);
	}
	if (f)
		fclose(f);
	return out;
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

	Json::Value items(Json::arrayValue);
	for (const auto& part : parts) {
		bool mounted = PartitionManager.Is_Mounted_By_Path(part.Mount_Point);
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
		Json::Value item(Json::objectValue);
		item["mount_point"] = part.Mount_Point;
		item["display_name"] = part.Display_Name;
		item["mounted"] = mounted;
		item["extra"] = extra;
		items.append(item);
	}
	Json::Value payload(Json::objectValue);
	payload["items"] = items;
	Fox_Command_Dispatcher::EmitData("list", payload);
}

int Fox_Fifo::Cmd_Mount(const Json::Value& args, bool unmount) {
	std::vector<std::string> a = arg_array(args, "paths");
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

int Fox_Fifo::Cmd_Flash(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "zips");
	// Signature verification, reflash-after-flash and unmount toggles. The
	// verify/reflash fields only override the persistent (UI-configured) value
	// when present, so omitting one keeps the same default the GUI would use.
	bool verify = arg_has(args, "verify") && arg_bool(args, "verify");
	bool no_verify = arg_has(args, "verify") && !arg_bool(args, "verify");
	bool reflash = arg_has(args, "reflash") && arg_bool(args, "reflash");
	bool no_reflash = arg_has(args, "reflash") && !arg_bool(args, "reflash");
	bool unmount_system = arg_bool(args, "unmount_system");
	bool unmount_vendor = arg_bool(args, "unmount_vendor");

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

int Fox_Fifo::Cmd_Backup(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
	std::string name = arg_str(args, "name");
	std::string path = arg_str(args, "path");
	std::string storage = arg_str(args, "storage");
	bool compress = arg_bool(args, "compress");
	// digest generation is on by default; an explicit digest:false disables it.
	bool no_digest = arg_has(args, "digest") && !arg_bool(args, "digest", true);

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
	// --storage or --path, so a "backup data" request lands on Internal Storage rather
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
		// against the localized gui_lookup("auto_generate") string.
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

int Fox_Fifo::Cmd_Restore(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
	std::string name = arg_str(args, "name");
	std::string path = arg_str(args, "path");
	if (name.empty())
		name = path;
	// digest check is on by default; an explicit digest_check:false skips it.
	bool skip_digest = arg_has(args, "digest_check") && !arg_bool(args, "digest_check", true);

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

int Fox_Fifo::Cmd_Decrypt(const Json::Value& args) {
	std::string password = arg_str(args, "password");
	std::string user = arg_str(args, "user");
	if (password.empty()) {
		gui_print("fox: decrypt requires a password\n");
		return 2;
	}
	int user_id = user.empty() ? 0 : atoi(user.c_str());

	gui_print("Attempting to decrypt data partition...\n");
	if (PartitionManager.Decrypt_Device(password, user_id) != 0) {
		gui_print("fox: decryption failed\n");
		return 1;
	}
	gui_print("Decryption successful\n");
	return 0;
}

int Fox_Fifo::Cmd_Sideload(const Json::Value& /*args*/) {
	// Reuse the tested ORS sideload handler.
	return OpenRecoveryScript::Run_ORS_Line("sideload");
}

static std::string fallback_property(const std::string& first, const std::string& second) {
	std::string value = TWFunc::Fox_Property_Get(first);
	if (value.empty())
		value = TWFunc::System_Property_Get(second);
	return value;
}

int Fox_Fifo::Cmd_Status(const Json::Value& /*args*/) {
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

	Json::Value s(Json::objectValue);
	s["release"] = FOX_BUILD;
	s["recovery_version"] = TW_VERSION_STR;
	s["variant"] = FOX_VARIANT;
	s["build_type"] = FOX_BUILD_TYPE;
	s["branch"] = OF_CURRENT_BRANCH;
	s["codebase_sdk"] = TWFunc::Fox_Property_Get("ro.build.version.sdk");
	s["codebase"] = FOX_CURRENT_DEV_STR;
	s["build_date"] = DataManager::GetStrValue("FOX_BUILD_DATE_REAL");
	s["device_model"] = device_model;
	s["device_code"] = device_code;
	s["storage_encrypted"] = storage_encrypted != 0;
	s["storage_decrypted"] = is_decrypted != 0;
	s["dynamic_partitions"] = TWFunc::Fox_Property_Get("orangefox.super.partition") == "true";
	s["boot_slot"] = TWFunc::Fox_Property_Get("ro.boot.slot_suffix");
	s["virtual_ab"] = TWFunc::Fox_Property_Get("ro.virtual_ab.enabled") == "true";
	s["kernel"] = TWFunc::Fox_Property_Get("ro.orangefox.kernel");
	s["rom_name"] = rom_name;
	s["rom_incremental"] = rom_incremental;
	s["rom_sdk"] = rom_sdk;
	s["android_release"] = android_release;
	s["rom_miui"] = TWFunc::Fox_Property_Get("orangefox.miui.rom") == "1";
	s["fingerprint"] = fingerprint;
	Fox_Command_Dispatcher::EmitData("status", s);
	return 0;
}

int Fox_Fifo::Cmd_Reboot(const Json::Value& args) {
	std::string target = arg_str(args, "target");
	if (target.empty())
		target = "system";
	return OpenRecoveryScript::Run_ORS_Line("reboot " + target);
}

int Fox_Fifo::Cmd_Wlan(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: WLAN support is not enabled in this build\n");
	return 1;
#else
	std::string sub = arg_action(args);
	std::string ssid = arg_str(args, "ssid");
	// `ap` manages its own radio bring-up (and `ap ssid|password` only touch the
	// secret store), so it is exempt from the "enable WLAN first" guard.
	if (!Wlan::IsEnabled() && sub != "enable" && sub != "ap") {
		gui_print("fox: WLAN is not enabled (enable it first)\n");
		return 1;
	}
	if (sub.empty()) {
		gui_print("fox: wlan requires a subcommand (enable|disable|list|connect|saved|connect-saved|forget|info|test|ap)\n");
		return 2;
	}
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
		if (ssid.empty()) {
			gui_print("fox: wlan connect requires an SSID\n");
			return 2;
		}
		// Populate scan metadata so Connect() can resolve the encryption type.
		Wlan::Scan();
		DataManager::SetValue("wlanselectedid", ssid);
		DataManager::SetValue("wlan_password", arg_str(args, "password"));
		return Wlan::Connect() ? 0 : 1;
	}
	if (sub == "saved") {
		if (!Wlan::RefreshSaved())
			return 1;
		std::string list = read_text_file("/tmp/wlan/saved.txt");
		gui_print("%s\n", list.c_str());
		return 0;
	}
	if (sub == "password_list") {
		// Saved networks with their decrypted passwords.
		std::vector<std::string> ssids;
		Fox_Secrets::ListWlanNetworks(ssids);
		Json::Value items(Json::arrayValue);
		for (const std::string& ssid : ssids) {
			std::string password, encryption;
			if (!Fox_Secrets::GetWlanNetwork(ssid, password, encryption))
				continue;
			Json::Value item(Json::objectValue);
			item["ssid"] = ssid;
			item["encryption"] = encryption;
			item["password"] = password;
			items.append(item);
		}
		Json::Value payload(Json::objectValue);
		payload["items"] = items;
		Fox_Command_Dispatcher::EmitData("wlan_passwords", payload);
		return 0;
	}
	if (sub == "connect-saved") {
		if (ssid.empty()) {
			gui_print("fox: wlan connect-saved requires an SSID\n");
			return 2;
		}
		DataManager::SetValue("wlanselectedid", ssid);
		return Wlan::ConnectSaved() ? 0 : 1;
	}
	if (sub == "forget") {
		if (ssid.empty()) {
			gui_print("fox: wlan forget requires an SSID\n");
			return 2;
		}
		DataManager::SetValue("wlanselectedid", ssid);
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
	if (sub == "ap") {
#ifndef OF_WLAN_AP
		gui_print("fox: WLAN hotspot (AP) support is not enabled in this build (set OF_WLAN_AP=1)\n");
		return 1;
#else
		std::string verb = arg_str(args, "subaction");
		const Json::Value& values = args["values"];
		auto value0 = [&]() -> std::string {
			if (values.isArray() && values.size() > 0 && values[0].isString())
				return values[0].asString();
			return "";
		};

		if (verb.empty() || verb == "status") {
			std::string ap_ssid, ap_pass;
			Wlan::ApGetConfig(ap_ssid, ap_pass);

			std::vector<Wlan::ApClient> clients;
			Wlan::ApListClients(clients);

			bool enabled = Wlan::ApIsEnabled();
			std::string ip = DataManager::GetStrValue("tw_wlan_ap_ip");

			// Human-readable lines (captured + pretty-printed by the CLI).
			gui_print("Hotspot: %s\n", enabled ? "enabled" : "disabled");
			gui_print("SSID: %s\n", ap_ssid.c_str());
			gui_print("Security: %s\n", ap_pass.empty() ? "open" : "WPA2-PSK");
			if (enabled) {
				gui_print("IP: %s\n", ip.c_str());
				gui_print("Clients: %d\n", (int)clients.size());
			}

			// Structured payload for WebUSB/JSON callers.
			Json::Value payload(Json::objectValue);
			payload["enabled"] = enabled;
			payload["ssid"] = ap_ssid;
			payload["secured"] = !ap_pass.empty();
			payload["ip"] = ip;
			payload["clients"] = (Json::UInt)clients.size();
			Fox_Command_Dispatcher::EmitData("wlan_ap_status", payload);
			return 0;
		}
		if (verb == "enable")
			return Wlan::ApEnable() ? 0 : 1;
		if (verb == "disable")
			return Wlan::ApDisable() ? 0 : 1;
		if (verb == "ssid") {
			if (value0().empty()) {
				gui_print("fox: wlan ap ssid requires a value\n");
				return 2;
			}
			return Wlan::ApSetSsid(value0()) ? 0 : 1;
		}
		if (verb == "password") {
			// "clear" (or no value) configures an open hotspot.
			std::string pw = value0();
			if (pw == "clear")
				pw.clear();
			return Wlan::ApSetPassword(pw) ? 0 : 1;
		}
		if (verb == "list") {
			std::vector<Wlan::ApClient> clients;
			Wlan::ApListClients(clients);

			if (clients.empty()) {
				gui_print("No devices connected to the hotspot\n");
			} else {
				for (const Wlan::ApClient& c : clients)
					gui_print("%s\t%s\t%s\n", c.ip.c_str(), c.mac.c_str(),
					          c.hostname.empty() ? "-" : c.hostname.c_str());
			}

			Json::Value items(Json::arrayValue);
			for (const Wlan::ApClient& c : clients) {
				Json::Value item(Json::objectValue);
				item["ip"] = c.ip;
				item["mac"] = c.mac;
				item["hostname"] = c.hostname;
				items.append(item);
			}
			Json::Value payload(Json::objectValue);
			payload["items"] = items;
			Fox_Command_Dispatcher::EmitData("wlan_ap_clients", payload);
			return 0;
		}

		gui_print("fox: unknown wlan ap verb '%s' (enable|disable|status|ssid|password|list)\n", verb.c_str());
		return 2;
#endif // OF_WLAN_AP
	}
	gui_print("fox: unknown wlan subcommand '%s'\n", sub.c_str());
	return 2;
#endif // OF_ENABLE_WLAN
}

int Fox_Fifo::Cmd_Adbd(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: network ADB is not enabled in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	bool no_auth = arg_bool(args, "no_auth");
	std::string action = arg_action(args);
	if (action.empty())
		action = "status";
	int port = (int)arg_int(args, "port", 5555);
	int timeout = (int)arg_int(args, "timeout", 60);

	if (action == "status") {
		Fox_Adbd::PrintStatus();
		return 0;
	}
	if (action == "start" || action == "enable") {
		if (no_auth)
			return Fox_Adbd::StartNoAuth(port) ? 0 : 1;
		return Fox_Adbd::StartSecure(port) ? 0 : 1;
	}
	if (action == "pair")
		return Fox_Adbd::StartPairing(timeout) ? 0 : 1;
	if (action == "stop-pair")
		return Fox_Adbd::StopPairing() ? 0 : 1;
	if (action == "stop" || action == "disable")
		return Fox_Adbd::StopAll() ? 0 : 1;
	if (action == "list") {
		Fox_Adbd::PrintDevices();
		return 0;
	}
	if (action == "forget") {
		// The device id rides in `subaction` (the generic action mapping); also
		// accept an explicit `device` field.
		std::string id = arg_str(args, "device");
		if (id.empty())
			id = arg_str(args, "subaction");
		return Fox_Adbd::ForgetDevice(id) ? 0 : 1;
	}

	gui_print("fox: unknown adbd subcommand '%s' "
	          "(start|pair|stop-pair|stop|status|list|forget)\n", action.c_str());
	return 2;
#endif
}

int Fox_Fifo::Cmd_Internal(const Json::Value& args) {
	std::string sub = arg_action(args);
	std::string name = arg_str(args, "name");
	if (sub.empty()) {
		gui_print("fox: internal requires a subcommand (get|set|go)\n");
		return 2;
	}
	if (sub == "get") {
		if (name.empty()) {
			gui_print("fox: internal get requires a variable name\n");
			return 2;
		}
		std::string value;
		DataManager::GetValue(name, value);
		gui_print("%s\n", value.c_str());
		return 0;
	}
	if (sub == "set") {
		if (name.empty()) {
			gui_print("fox: internal set requires a variable name\n");
			return 2;
		}
		DataManager::SetValue(name, arg_str(args, "value"));
		return 0;
	}
	if (sub == "go") {
		std::string page = arg_str(args, "page");
		if (page.empty()) {
			gui_print("fox: internal go requires a page name\n");
			return 2;
		}
		gui_changePage(page);
		return 0;
	}
	gui_print("fox: unknown internal subcommand '%s'\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Ors(const Json::Value& args) {
	std::string path = arg_str(args, "path");
	if (path.empty()) {
		gui_print("fox: ors requires a script file path\n");
		return 2;
	}
	return OpenRecoveryScript::Run_ORS_File(path);
}

int Fox_Fifo::Cmd_OrsCmd(const Json::Value& args) {
	std::vector<std::string> raw = arg_array(args, "raw");
	std::string joined;
	for (size_t i = 0; i < raw.size(); i++) {
		if (i)
			joined += " ";
		joined += raw[i];
	}
	if (joined.empty()) {
		gui_print("fox: ors-cmd requires a command line\n");
		return 2;
	}
	return OpenRecoveryScript::Run_ORS_Line(joined);
}

int Fox_Fifo::Cmd_Wipe(const Json::Value& args) {
	std::vector<std::string> a = arg_array(args, "parts");
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

int Fox_Fifo::Cmd_Format_Data(const Json::Value& args) {
	bool confirmed = arg_bool(args, "confirm");
	if (!confirmed) {
		gui_print("fox: format_data erases ALL data and removes encryption.\n");
		gui_print("fox: refusing without an explicit confirm flag\n");
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

int Fox_Fifo::Cmd_Reflash(const Json::Value& /*args*/) {
	gui_print("Reflashing OrangeFox...\n");
	twrpRepacker repacker;
	if (!repacker.Flash_Current_Twrp()) {
		gui_print("fox: reflash failed\n");
		return 1;
	}
	gui_print("OrangeFox reflashed\n");
	return 0;
}

int Fox_Fifo::Cmd_Mtp(const Json::Value& args) {
	std::string sub = arg_action(args);
	if (sub.empty())
		sub = "status";
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["enabled"] = DataManager::GetIntValue("tw_mtp_enabled") != 0;
		Fox_Command_Dispatcher::EmitData("mtp", s);
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

int Fox_Fifo::Cmd_Log(const Json::Value& /*args*/) {
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

int Fox_Fifo::Cmd_Partition(const Json::Value& args) {
	std::string path_in = arg_str(args, "path");
	std::string op = arg_action(args);
	if (path_in.empty()) {
		// No path: list partitions with file system and capability flags.
		Emit_Partition_List("part_option", true);
		return 0;
	}
	if (op.empty()) {
		gui_print("fox: partition requires <path> <repair|resize|change-fs <fs>|wipe>\n");
		return 2;
	}

	std::string path = normalize_partition_spec(path_in);
	if (!PartitionManager.Find_Partition_By_Path(path)) {
		gui_print("fox: unknown partition '%s'\n", path_in.c_str());
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
		std::string fs = arg_str(args, "fs");
		if (fs.empty()) {
			gui_print("fox: partition change-fs requires a file system (e.g. ext4)\n");
			return 2;
		}
		if (!PartitionManager.Wipe_By_Path(path, fs))
			return 1;
		gui_print("Changed %s to %s\n", path.c_str(), fs.c_str());
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

int Fox_Fifo::Cmd_Addons(const Json::Value& args) {
	std::vector<std::string> dirs = { Fox_Home_Files, FFiles_dir };
	std::string sub = arg_action(args);
	std::vector<std::string> names = arg_array(args, "names");

	if (sub.empty() || sub == "list") {
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		Json::Value items(Json::arrayValue);
		for (const auto& z : zips) {
			std::string name = z.substr(z.find_last_of('/') + 1);
			Json::Value item(Json::objectValue);
			// mount_point column carries the path; display the file name.
			item["mount_point"] = z;
			item["display_name"] = name;
			item["mounted"] = false;
			item["extra"] = "";
			items.append(item);
		}
		Json::Value payload(Json::objectValue);
		payload["items"] = items;
		Fox_Command_Dispatcher::EmitData("list", payload);
		return 0;
	}

	if (sub == "install") {
		if (names.empty()) {
			gui_print("fox: addons install requires a zip name or path\n");
			return 2;
		}
		int rc = 0;
		std::vector<std::string> zips;
		for (const auto& dir : dirs)
			collect_zip_files(dir, zips);
		std::sort(zips.begin(), zips.end());
		zips.erase(std::unique(zips.begin(), zips.end()), zips.end());
		for (const std::string& requested : names) {
			std::string target = requested;
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
					gui_print("fox: addon '%s' not found\n", requested.c_str());
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

	gui_print("fox: unknown addons subcommand '%s' (list|install)\n", sub.c_str());
	return 2;
}

int Fox_Fifo::Cmd_Screencap(const Json::Value& args) {
	// Capture the live framebuffer to a PNG file (tmpfs by default) so a client
	// can read it back over an ADB sync/pull -- no network stack involved.
	std::string path = arg_str(args, "path");
	if (path.empty())
		path = "/tmp/.fox_screen.png";
	if (gr_save_screenshot(path.c_str()) != 0) {
		gui_print("fox: screencap failed\n");
		return 1;
	}
	gui_print("%s\n", path.c_str());
	return 0;
}

int Fox_Fifo::Cmd_Input(const Json::Value& args) {
	// Normally intercepted by the dispatcher's control fast path; handled here
	// too for completeness. Fox_Remote_Input reads the typed args directly.
	std::string output;
	int code = Fox_Remote_Input::RunCommand(args, &output);
	if (!output.empty())
		gui_print("%s", output.c_str());
	return code;
}

#ifdef OF_ENABLE_WLAN
namespace {
// Print the auth line without ever echoing a user-configured password.
void print_auth_line() {
	if (Fox_Httpd::AuthIsPassword())
		gui_print("auth=remote-password\n");
	else
		gui_print("token=%s\n", Fox_Httpd::Token().c_str());
}

// Add the auth descriptor to a status payload without leaking a configured
// password: either auth="remote-password" or the random per-boot token.
void auth_into(Json::Value& obj) {
	if (Fox_Httpd::AuthIsPassword())
		obj["auth"] = "remote-password";
	else
		obj["token"] = Fox_Httpd::Token();
}

// "http://<addr>:<port>/" — the dashboard-facing URL for a status payload.
std::string http_url(int port) {
	return "http://" + Fox_Httpd::LocalAddress() + ":" + std::to_string(port) + "/";
}
} // namespace
#endif

// webserver: the HTTP server umbrella.
//   webserver {start [--port N]|stop|status|terminal ...}  — plaintext server
//   webserver http {enable|disable|status} [--port N]      — plaintext listener
//   webserver https {enable|disable|status|cert ...} [--port N]  — TLS listener
//   webserver redirect {enable|disable|status}             — plaintext->HTTPS
//   webserver cert {regen|status}                          — TLS cert (alias)
//   webserver cors {set <origin>|reset|status}             — CORS allow-list
// The remote-control plane (screen + input) is governed separately by `control`.
int Fox_Fifo::Cmd_Webserver(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: webserver is not available in this build (OF_ENABLE_WLAN=0)\n");
	gui_print("fox: direct WebUSB remains available via screencap/input/screenstream requests\n");
	return 1;
#else
	std::string sub = arg_action(args);
	if (sub.empty())
		sub = "status";
	std::string subaction = arg_str(args, "subaction");
	std::vector<std::string> values = arg_array(args, "values");
	std::string port_s = arg_str(args, "port");

	// Nested transport/policy groups: the group is `action`, its verb is
	// `subaction`, and any further tokens ride in `values`.
	if (sub == "http")
		return Cmd_Http(subaction, port_s);
	if (sub == "https")
		return Cmd_Https(subaction, values, port_s);
	if (sub == "redirect" || sub == "http_redirect")
		return Cmd_HttpRedirect(subaction);
	if (sub == "cors")
		return Cmd_Cors(subaction, values);
	// `webserver cert {regen|status}` — convenience alias for the TLS cert
	// actions (forwarded to the shared cert handler), so the certificate can be
	// (re)generated without first going through `webserver https`.
	if (sub == "cert")
		return Cmd_Https("cert", { subaction }, port_s);

	if (sub == "start" || sub == "enable") {
		int port = port_s.empty() ? 80 : atoi(port_s.c_str());
		if (Fox_Httpd::IsRunning()) {
			gui_print("Web server already running on %s:%d\n",
			          Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
			print_auth_line();
			return 0;
		}
		if (!Fox_Httpd::Start(port)) {
			gui_print("fox: failed to start web server\n");
			return 1;
		}
		gui_print("Web server started\n");
		gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
		gui_print("port=%d\n", Fox_Httpd::Port());
		print_auth_line();
		gui_print("control=%d\n", Fox_Httpd::ControlEnabled() ? 1 : 0);
		return 0;
	}
	if (sub == "stop" || sub == "disable") {
		Fox_Ttyd::Stop();
		Fox_Httpd::Stop();
		gui_print("Web server stopped\n");
		return 0;
	}
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["running"] = Fox_Httpd::IsRunning();
		s["control"] = Fox_Httpd::ControlEnabled();
		if (Fox_Httpd::IsRunning()) {
			s["address"] = Fox_Httpd::LocalAddress();
			s["port"] = Fox_Httpd::Port();
			s["url"] = http_url(Fox_Httpd::Port());
			auth_into(s);
		}
		Fox_Command_Dispatcher::EmitData("webserver", s);
		return 0;
	}
	if (sub == "terminal" || sub == "ttyd") {
		std::string action = subaction.empty() ? "status" : subaction;
		if (action == "start") {
			int port = port_s.empty() ? 7681 : atoi(port_s.c_str());
			if (!Fox_Ttyd::Start(port)) {
				gui_print("fox: failed to start ttyd\n");
				if (!Fox_Ttyd::LastError().empty())
					gui_print("%s\n", Fox_Ttyd::LastError().c_str());
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
			Json::Value s(Json::objectValue);
			s["running"] = Fox_Ttyd::IsRunning();
			s["port"] = Fox_Ttyd::Port();
			s["url"] = http_url(Fox_Ttyd::Port());
			s["shell"] = Fox_Ttyd::Shell();
			Fox_Command_Dispatcher::EmitData("ttyd", s);
			return 0;
		}
		gui_print("fox: unknown webserver terminal subcommand '%s' (start|stop|status)\n", action.c_str());
		return 2;
	}
	gui_print("fox: unknown webserver subcommand '%s' "
	          "(start|stop|status|terminal|http|https|redirect|cert|cors)\n", sub.c_str());
	return 2;
#endif
}

// control {start|stop|status}: toggle the remote-control plane (screen
// mirroring + input injection) exposed over the HTTP server, independently of
// whether the server is running. Direct `input`/`screencap` RPC commands do
// not depend on this toggle.
int Fox_Fifo::Cmd_Control(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: HTTP control plane is not available in this build (OF_ENABLE_WLAN=0)\n");
	gui_print("fox: direct WebUSB remains available via input/screencap/screenstream requests\n");
	return 1;
#else
	std::string sub = arg_action(args);
	if (sub.empty())
		sub = "status";

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
		Json::Value s(Json::objectValue);
		s["control"] = Fox_Httpd::ControlEnabled();
		s["input_ready"] = Fox_Remote_Input::IsReady();
		Fox_Command_Dispatcher::EmitData("control", s);
		return 0;
	}
	gui_print("fox: unknown control subcommand '%s' (start|stop|status)\n", sub.c_str());
	return 2;
#endif
}

// web {enable|disable|status|password ...}: umbrella control for the
// networked access surface. The same password authenticates the ttyd terminal
// (HTTP basic-auth) and the REST/remote HTTP API (token).
int Fox_Fifo::Cmd_Web(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: web access is not available in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	std::string sub = arg_action(args);
	if (sub.empty())
		sub = "status";

	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["password_set"] = Fox_Secrets::HasRemotePassword();
		s["webserver"] = Fox_Httpd::IsRunning();
		s["ttyd"] = Fox_Ttyd::IsRunning();
		if (Fox_Httpd::IsRunning()) {
			s["address"] = Fox_Httpd::LocalAddress();
			s["port"] = Fox_Httpd::Port();
		}
		Fox_Command_Dispatcher::EmitData("web", s);
		return 0;
	}

	if (sub == "enable" || sub == "start" || sub == "on") {
		// Secure by default: never expose the network surface without a password.
		if (!Fox_Secrets::HasRemotePassword()) {
			gui_print("fox: refusing to enable web access without a password\n");
			gui_print("fox: set one first: web password <password>\n");
			return 1;
		}

		std::string port_s = arg_str(args, "port");
		int port = port_s.empty() ? 80 : atoi(port_s.c_str());

		int rc = 0;
		if (!Fox_Httpd::IsRunning() && !Fox_Httpd::Start(port)) {
			gui_print("fox: failed to start web server\n");
			rc = 1;
		} else {
			gui_print("Web server: http://%s:%d/\n",
			          Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
		}
		if (!Fox_Ttyd::IsRunning() && !Fox_Ttyd::Start()) {
			gui_print("fox: failed to start ttyd\n");
			if (!Fox_Ttyd::LastError().empty())
				gui_print("%s\n", Fox_Ttyd::LastError().c_str());
			rc = 1;
		} else {
			gui_print("Terminal: http://%s:%d/\n",
			          Fox_Httpd::LocalAddress().c_str(), Fox_Ttyd::Port());
		}
		if (!Fox_Adbd::StartSecure(5555)) {
			gui_print("fox: failed to start network adbd\n");
			rc = 1;
		} else {
			gui_print("Network adbd started on %s:5555\n", Fox_Adbd::WlanIp().c_str());
		}
		return rc;
	}

	if (sub == "disable" || sub == "stop" || sub == "off") {
		Fox_Ttyd::Stop();
		Fox_Httpd::Stop();
		Fox_Adbd::StopAll();
		gui_print("Web access disabled\n");
		return 0;
	}

	if (sub == "password") {
		bool generate = arg_bool(args, "generate");
		std::string value = arg_str(args, "subaction");
		std::vector<std::string> extra = arg_array(args, "values");

		// `web password` with no value and no --generate: show the currently
		// configured password (the caller explicitly asked to see it).
		if (!generate && value.empty() && extra.empty()) {
			std::string current;
			Json::Value s(Json::objectValue);
			if (Fox_Secrets::GetRemotePassword(current) && !current.empty()) {
				s["password_set"] = true;
				s["password"] = current;
			} else {
				s["password_set"] = false;
			}
			Fox_Command_Dispatcher::EmitData("password", s);
			return 0;
		}

		bool ok;
		if (value == "clear") {
			ok = Fox_Secrets::ClearRemotePassword();
			if (!ok) {
				gui_print("fox: failed to clear web password\n");
				return 1;
			}
			gui_print("Web password cleared\n");
		} else {
			std::string password;
			if (generate) {
				password = generate_password();
			} else {
				// Join the password value and any trailing tokens (a password
				// may contain spaces and arrive split across value + values[]).
				password = value;
				for (const auto& tok : extra) {
					if (!password.empty())
						password += " ";
					password += tok;
				}
			}
			ok = Fox_Secrets::SetRemotePassword(password);
			if (!ok) {
				gui_print("fox: failed to save web password\n");
				return 1;
			}
			if (generate) {
				// We minted the secret on the caller's behalf, so return it once.
				gui_print("Web password generated\n");
				gui_print("password=%s\n", password.c_str());
			} else {
				// Never echo a user-supplied password back.
				gui_print("Web password set\n");
			}
		}

		// Re-apply to any live services so the new credential is used at once.
		if (Fox_Httpd::IsRunning()) {
			int port = Fox_Httpd::Port();
			Fox_Httpd::Stop();
			Fox_Httpd::Start(port);
		}
		if (Fox_Ttyd::IsRunning()) {
			int port = Fox_Ttyd::Port();
			Fox_Ttyd::Stop();
			// After a clear, ttyd will refuse to restart (no password) by design.
			Fox_Ttyd::Start(port);
		}
		return 0;
	}

	gui_print("fox: unknown web subcommand '%s' (enable|disable|status|password)\n", sub.c_str());
	return 2;
#endif
}

// webserver http {enable|disable|status} [--port N]: the plaintext HTTP
// listener on its own, independent of the HTTPS listener and the `web` umbrella.
// When redirect mode is enabled, this listener serves only 301s to HTTPS.
int Fox_Fifo::Cmd_Http(const std::string& verb, const std::string& port_s) {
#ifndef OF_ENABLE_WLAN
	(void)verb;
	(void)port_s;
	gui_print("fox: http server is not available in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	std::string sub = verb.empty() ? "status" : verb;

	if (sub == "enable" || sub == "start" || sub == "on") {
		int port = port_s.empty() ? 80 : atoi(port_s.c_str());
		if (Fox_Httpd::IsRunning()) {
			gui_print("HTTP server already running on %s:%d\n",
			          Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
			gui_print("redirect=%d\n", Fox_Httpd::RedirectEnabled() ? 1 : 0);
			return 0;
		}
		if (!Fox_Httpd::Start(port)) {
			gui_print("fox: failed to start HTTP server\n");
			return 1;
		}
		gui_print("HTTP server started\n");
		gui_print("url=http://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Httpd::Port());
		gui_print("port=%d\n", Fox_Httpd::Port());
		print_auth_line();
		gui_print("redirect=%d\n", Fox_Httpd::RedirectEnabled() ? 1 : 0);
		return 0;
	}
	if (sub == "disable" || sub == "stop" || sub == "off") {
		Fox_Httpd::Stop();
		gui_print("HTTP server stopped\n");
		return 0;
	}
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["running"] = Fox_Httpd::IsRunning();
		s["redirect"] = Fox_Httpd::RedirectEnabled();
		if (Fox_Httpd::IsRunning()) {
			s["address"] = Fox_Httpd::LocalAddress();
			s["port"] = Fox_Httpd::Port();
			s["url"] = http_url(Fox_Httpd::Port());
			auth_into(s);
		}
		Fox_Command_Dispatcher::EmitData("http", s);
		return 0;
	}
	gui_print("fox: unknown http subcommand '%s' (enable|disable|status)\n", sub.c_str());
	return 2;
#endif
}

// webserver https {enable|disable|status|cert} [--port N]: the TLS listener
// with a persistent self-signed cert. `cert regen` forces a fresh keypair+cert.
int Fox_Fifo::Cmd_Https(const std::string& verb, const std::vector<std::string>& values,
                        const std::string& port_s) {
#ifndef OF_ENABLE_WLAN
	(void)verb;
	(void)values;
	(void)port_s;
	gui_print("fox: https server is not available in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	std::string sub = verb.empty() ? "status" : verb;

	if (sub == "enable" || sub == "start" || sub == "on") {
		int port = port_s.empty() ? Fox_Httpsd::DefaultPort() : atoi(port_s.c_str());

		// Ensure the cert exists first; a failure here means TLS can't come up.
		std::string cp, kp, err;
		if (!Fox_Tls_Cert::EnsureCert(cp, kp, err)) {
			gui_print("fox: failed to prepare TLS cert: %s\n", err.c_str());
			return 1;
		}

		if (Fox_Httpsd::IsRunning())
			Fox_Httpsd::Stop();
		if (!Fox_Httpsd::Start(port)) {
			gui_print("fox: failed to start HTTPS server\n");
			return 1;
		}
		// Record the live HTTPS port so plaintext redirects target it.
		Fox_Httpd::SetHttpsPortForRedirect(Fox_Httpsd::Port());

		gui_print("HTTPS server started\n");
		gui_print("url=https://%s:%d/\n", Fox_Httpd::LocalAddress().c_str(), Fox_Httpsd::Port());
		gui_print("port=%d\n", Fox_Httpsd::Port());
		std::string fp = Fox_Tls_Cert::Fingerprint();
		if (!fp.empty())
			gui_print("fingerprint=%s\n", fp.c_str());
		gui_print("cert=%s\n", Fox_Tls_Cert::CertPath());
		print_auth_line();
		gui_print("note=self-signed certificate; browsers will warn until it is trusted/pinned\n");
		return 0;
	}
	if (sub == "disable" || sub == "stop" || sub == "off") {
		Fox_Httpsd::Stop();
		gui_print("HTTPS server stopped\n");
		return 0;
	}
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["running"] = Fox_Httpsd::IsRunning();
		if (Fox_Httpsd::IsRunning()) {
			s["address"] = Fox_Httpd::LocalAddress();
			s["port"] = Fox_Httpsd::Port();
			s["url"] = "https://" + Fox_Httpd::LocalAddress() + ":" +
			           std::to_string(Fox_Httpsd::Port()) + "/";
			auth_into(s);
		}
		std::string fp = Fox_Tls_Cert::Fingerprint();
		if (!fp.empty()) {
			s["fingerprint"] = fp;
			s["cert"] = Fox_Tls_Cert::CertPath();
		}
		Fox_Command_Dispatcher::EmitData("https", s);
		return 0;
	}
	if (sub == "cert") {
		std::string action = values.empty() ? "status" : values[0];
		if (action == "regen") {
			std::string err;
			if (!Fox_Tls_Cert::RegenCert(err)) {
				gui_print("fox: cert regen failed: %s\n", err.c_str());
				return 1;
			}
			// Reload into a running listener, if any.
			if (Fox_Httpsd::IsRunning()) {
				int p = Fox_Httpsd::Port();
				Fox_Httpsd::Stop();
				Fox_Httpsd::Start(p);
				gui_print("HTTPS server reloaded with new cert\n");
			}
			gui_print("fingerprint=%s\n", Fox_Tls_Cert::Fingerprint().c_str());
			return 0;
		}
		if (action == "status") {
			std::string fp = Fox_Tls_Cert::Fingerprint();
			if (fp.empty()) {
				gui_print("cert=none\n");
			} else {
				gui_print("cert=%s\n", Fox_Tls_Cert::CertPath());
				gui_print("fingerprint=%s\n", fp.c_str());
			}
			return 0;
		}
		gui_print("Usage: webserver https cert {regen|status}\n");
		return 2;
	}
	gui_print("fox: unknown https subcommand '%s' (enable|disable|status|cert ...)\n", sub.c_str());
	return 2;
#endif
}

// webserver redirect {enable|disable|status}: toggle the plaintext listener's
// redirect-to-https mode. Persists across reboots. A redirect-mode listener
// answers plaintext requests with a 301 to HTTPS and serves nothing else.
int Fox_Fifo::Cmd_HttpRedirect(const std::string& verb) {
#ifndef OF_ENABLE_WLAN
	(void)verb;
	gui_print("fox: http server is not available in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	std::string sub = verb.empty() ? "status" : verb;

	if (sub == "enable" || sub == "on") {
		Fox_Httpd::SetRedirectEnabled(true);
		gui_print("HTTP redirect mode enabled (plaintext -> https)\n");
		if (!Fox_Httpsd::IsRunning())
			gui_print("note: HTTPS listener is not running; start it with `webserver https enable`\n");
		if (!Fox_Httpd::IsRunning())
			gui_print("note: HTTP listener is not running; start it with `webserver http enable`\n");
		return 0;
	}
	if (sub == "disable" || sub == "off") {
		Fox_Httpd::SetRedirectEnabled(false);
		gui_print("HTTP redirect mode disabled\n");
		return 0;
	}
	if (sub == "status") {
		Json::Value s(Json::objectValue);
		s["redirect"] = Fox_Httpd::RedirectEnabled();
		s["https_port"] = Fox_Httpd::HttpsPortForRedirect();
		Fox_Command_Dispatcher::EmitData("redirect", s);
		return 0;
	}
	gui_print("fox: unknown redirect subcommand '%s' (enable|disable|status)\n", sub.c_str());
	return 2;
#endif
}

// webserver cors {set <origin>|reset|status}: inspect or override the HTTP
// cross-origin allow-list. By default only the device-local origins and the
// official orangefox.tech / orangefox.download domains are accepted; `set "*"`
// (or a specific origin) is a developer escape hatch. The override is in-memory
// only and resets on reboot.
int Fox_Fifo::Cmd_Cors(const std::string& verb, const std::vector<std::string>& values) {
#ifndef OF_ENABLE_WLAN
	(void)verb;
	(void)values;
	gui_print("fox: http server is not available in this build (OF_ENABLE_WLAN=0)\n");
	return 1;
#else
	std::string sub = verb.empty() ? "status" : verb;

	if (sub == "set") {
		if (values.empty()) {
			gui_print("fox: cors set requires an origin (or \"*\")\n");
			return 2;
		}
		Fox_Httpd::SetCorsOverride(values[0]);
		gui_print("CORS override set\n");
		gui_print("cors=%s\n", values[0].c_str());
		return 0;
	}
	if (sub == "reset" || sub == "clear" || sub == "default") {
		Fox_Httpd::SetCorsOverride("");
		gui_print("CORS override cleared (default allow-list restored)\n");
		return 0;
	}
	if (sub == "status") {
		std::string override_origin = Fox_Httpd::CorsOverride();
		Json::Value s(Json::objectValue);
		s["mode"] = override_origin.empty() ? "default" : "override";
		if (override_origin.empty())
			s["allow"] = "localhost," + Fox_Httpd::LocalAddress() +
			             ",*.orangefox.tech,*.orangefox.download";
		else
			s["override"] = override_origin;
		Fox_Command_Dispatcher::EmitData("cors", s);
		return 0;
	}
	gui_print("fox: unknown cors subcommand '%s' (set|reset|status)\n", sub.c_str());
	return 2;
#endif
}

int Fox_Fifo::Cmd_Storages(const Json::Value& /*args*/) {
	PartitionManager.Mount_All_Storage();
	std::vector<PartitionList> storage_list;
	PartitionManager.Get_Partition_List("storage", &storage_list);
	std::string current = DataManager::GetCurrentStoragePath();

	Json::Value items(Json::arrayValue);
	for (const auto& storage : storage_list) {
		Json::Value item(Json::objectValue);
		item["mount_point"] = storage.Mount_Point;
		item["display_name"] = storage.Display_Name;
		item["mounted"] = storage.Mount_Point == current;  // "selected" storage
		item["extra"] = storage.isFiles ? "files" : "block";
		items.append(item);
	}
	Json::Value payload(Json::objectValue);
	payload["items"] = items;
	Fox_Command_Dispatcher::EmitData("list", payload);
	return 0;
}

int Fox_Fifo::Cmd_Ota(const Json::Value& args) {
#ifndef OF_ENABLE_WLAN
	(void)args;
	gui_print("fox: OTA support is not enabled in this build\n");
	return 1;
#else
	std::string fox_id = arg_str(args, "fox_id");
	std::string type_filter = arg_str(args, "type");
	std::string action = arg_action(args);
	if (action.empty())
		action = "status";

	int opt_ret = FoxUpdater::SetTestOptions(fox_id, type_filter);
	if (opt_ret != 0)
		return opt_ret;

	if (action == "status") {
		FoxUpdater::Refresh();
		Json::Value s(Json::objectValue);
		s["status"] = DataManager::GetStrValue("fox_update_status");
		s["available"] = DataManager::GetStrValue("fox_update_available");
		s["downloaded"] = DataManager::GetStrValue("fox_update_downloaded");
		s["need_variant"] = DataManager::GetStrValue("fox_update_need_variant");
		s["current_build_id"] = DataManager::GetStrValue("fox_update_current_build_id");
		s["fox_id_override"] = DataManager::GetStrValue("fox_update_current_build_id_override");
		s["current_type"] = DataManager::GetStrValue("fox_update_current_type");
		s["current_version"] = DataManager::GetStrValue("fox_update_current_version");
		s["device_id"] = DataManager::GetStrValue("fox_update_device_id");
		s["device_name"] = DataManager::GetStrValue("fox_update_device_name");
		s["device_codename"] = DataManager::GetStrValue("fox_update_device_codename");
		s["latest_build_id"] = DataManager::GetStrValue("fox_update_latest_build_id");
		s["latest_type"] = DataManager::GetStrValue("fox_update_latest_type");
		s["latest_version"] = DataManager::GetStrValue("fox_update_latest_version");
		s["latest_filename"] = DataManager::GetStrValue("fox_update_latest_filename");
		s["latest_md5"] = DataManager::GetStrValue("fox_update_latest_md5");
		s["latest_size"] = DataManager::GetStrValue("fox_update_latest_size");
		s["latest_variant"] = DataManager::GetStrValue("fox_update_latest_variant");
		s["zip_path"] = DataManager::GetStrValue("fox_update_zip_path");
		s["type_filter"] = DataManager::GetStrValue("fox_update_type_filter");
		Fox_Command_Dispatcher::EmitData("ota", s);
		return 0;
	}
	if (action == "refresh")
		return FoxUpdater::Refresh();
	if (action == "init")
		return FoxUpdater::Init();
	if (action == "check")
		return FoxUpdater::Check();
	if (action == "download")
		return FoxUpdater::Download();
	if (action == "install")
		return FoxUpdater::Install();

	gui_print("fox: unknown ota subcommand '%s' (status|init|check|download|install|refresh)\n", action.c_str());
	return 2;
#endif
}

int Fox_Fifo::Run_Command() {
	if (op.empty()) {
		gui_print("fox: no command given\n");
		return 2;
	}

	// Dispatch straight off the typed request: the op selects the handler, which
	// reads its parameters from the args object (no argv round-trip).
	const Json::Value& a = cmd_args;

	if (op == "mount")
		return Cmd_Mount(a, false);
	if (op == "unmount" || op == "umount")
		return Cmd_Mount(a, true);
	if (op == "flash" || op == "install")
		return Cmd_Flash(a);
	if (op == "backup")
		return Cmd_Backup(a);
	if (op == "restore")
		return Cmd_Restore(a);
	if (op == "decrypt")
		return Cmd_Decrypt(a);
	if (op == "sideload")
		return Cmd_Sideload(a);
	if (op == "status")
		return Cmd_Status(a);
	if (op == "reboot")
		return Cmd_Reboot(a);
	if (op == "wlan" || op == "wifi")  // `wifi` is an alias for `wlan`
		return Cmd_Wlan(a);
	if (op == "adbd")
		return Cmd_Adbd(a);
	if (op == "internal")
		return Cmd_Internal(a);
	if (op == "wipe")
		return Cmd_Wipe(a);
	if (op == "format_data")
		return Cmd_Format_Data(a);
	if (op == "reflash")
		return Cmd_Reflash(a);
	if (op == "mtp")
		return Cmd_Mtp(a);
	if (op == "log")
		return Cmd_Log(a);
	if (op == "partition")
		return Cmd_Partition(a);
	if (op == "addons")
		return Cmd_Addons(a);
	if (op == "storages")
		return Cmd_Storages(a);
	if (op == "ota")
		return Cmd_Ota(a);
	if (op == "webserver")
		return Cmd_Webserver(a);
	if (op == "control")
		return Cmd_Control(a);
	if (op == "web")
		return Cmd_Web(a);
	if (op == "input")
		return Cmd_Input(a);
	if (op == "screencap")
		return Cmd_Screencap(a);
	if (op == "ors")
		return Cmd_Ors(a);
	if (op == "ors-cmd")
		return Cmd_OrsCmd(a);

	gui_print("fox: unknown command '%s'\n", op.c_str());
	return 2;
}
