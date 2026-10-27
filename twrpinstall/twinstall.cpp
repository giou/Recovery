/*
	Copyright 2012 to 2017 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2025 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <vector>

#include <string.h>
#include <stdio.h>
#include <cutils/properties.h>

#include <android-base/unique_fd.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#include "otautil/sysutil.h"
#include <ziparchive/zip_archive.h>
#include "twinstall/install.h"
#include "twinstall/verifier.h"
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "orangefox.hpp"
#include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "gui/blanktimer.hpp"
#include "twinstall.h"
#include "installcommand.h"
#include "../twrpRepacker.hpp"
extern "C" {
	#include "gui/gui.h"
}

#define AB_OTA "payload_properties.txt"

enum zip_type {
	UNKNOWN_ZIP_TYPE = 0,
	UPDATE_BINARY_ZIP_TYPE,
	AB_OTA_ZIP_TYPE,
	TWRP_THEME_ZIP_TYPE
};

static int Install_Theme(const char* path, ZipArchiveHandle Zip) {
#ifdef TW_OEM_BUILD // We don't do custom themes in OEM builds
	return INSTALL_CORRUPT;
#else
	std::string binary_name("ui.xml");
	ZipEntry64 binary_entry;
	if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
		return INSTALL_CORRUPT;
	}
	if (!PartitionManager.Mount_Settings_Storage(true))
		return INSTALL_ERROR;
	std::string theme_path = DataManager::GetCurrentStoragePath() + "/theme";
	if (!TWFunc::Path_Exists(theme_path)) {
		if (!TWFunc::Recursive_Mkdir(theme_path)) {
			return INSTALL_ERROR;
		}
	}
	theme_path += "/ui.zip";
	if (TWFunc::copy_file(path, theme_path, 0644) != 0) {
		return INSTALL_ERROR;
	}
	LOGINFO("Installing custom theme '%s' to '%s'\n", path, theme_path.c_str());
	PageManager::RequestReload();
	return INSTALL_SUCCESS;
#endif
}

static int Prepare_Update_Binary(const char *path, ZipArchiveHandle Zip) {
	if (TWFunc::Block_Operations_Until_Reboot())
		return INSTALL_ERROR;

	char arches[PATH_MAX];
	property_get("ro.product.cpu.abilist", arches, "error");
	if (strcmp(arches, "error") == 0)
		property_get("ro.product.cpu.abi", arches, "error");
	vector<string> split = TWFunc::split_string(arches, ',', true);
	std::vector<string>::iterator arch;
	std::string base_name = UPDATE_BINARY_NAME;
	base_name += "-";
	ZipEntry64 binary_entry;
	std::string update_binary_string(UPDATE_BINARY_NAME);
	if (FindEntry(Zip, update_binary_string, &binary_entry) != 0) {
		for (arch = split.begin(); arch != split.end(); arch++) {
			std::string temp = base_name + *arch;
			std::string binary_name(temp.c_str());
			if (FindEntry(Zip, binary_name, &binary_entry) != 0) {
				std::string binary_name(temp.c_str());
				break;
			}
		}
	}
	LOGINFO("Extracting updater binary '%s'\n", UPDATE_BINARY_NAME);
	unlink(TMP_UPDATER_BINARY_PATH);
	android::base::unique_fd fd(
		open(TMP_UPDATER_BINARY_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755));
	if (fd == -1) {
		return INSTALL_ERROR;
	}
	int32_t err = ExtractEntryToFile(Zip, &binary_entry, fd);
	if (err != 0) {
		LOGERR("Could not extract '%s'\n", UPDATE_BINARY_NAME);
		return INSTALL_ERROR;
	}

	// -------------- OrangeFox: start ---------------- //
	int Fox_Ret = Fox_Prepare_Update_Binary(path, Zip);
	if (Fox_Ret != INSTALL_SUCCESS) {
	   return Fox_Ret;
	}
	// -------------- OrangeFox: end ---------------- //

	// If exists, extract file_contexts from the zip file
	std::string file_contexts("file_contexts");
	ZipEntry64 file_contexts_entry;
	if (FindEntry(Zip, file_contexts, &file_contexts_entry) != 0) {
		LOGINFO("Zip does not contain SELinux file_contexts file in its root.\n");
	} else {
		const string output_filename = "/file_contexts";
		LOGINFO("Zip contains SELinux file_contexts file in its root. Extracting to %s\n", output_filename.c_str());
		android::base::unique_fd fd(
			open(output_filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644));
		if (fd == -1) {
			return INSTALL_ERROR;
		}
		if (ExtractEntryToFile(Zip, &file_contexts_entry, fd)) {
			LOGERR("Could not extract '%s'\n", output_filename.c_str());
			return INSTALL_ERROR;
		}
	}
	return INSTALL_SUCCESS;
}


static int Run_Update_Binary(const char *path, int* wipe_cache, zip_type ztype) {
	int ret_val, pipe_fd[2], status, zip_verify;
	int aroma_running = 0;
	char buffer[1024];
	FILE* child_data;
	pipe(pipe_fd);

	std::vector<std::string> args;
    if (ztype == UPDATE_BINARY_ZIP_TYPE) {
		ret_val = update_binary_command(path, 0, pipe_fd[1], &args);
    } else if (ztype == AB_OTA_ZIP_TYPE) {
		ret_val = abupdate_binary_command(path, 0, pipe_fd[1], &args);
	} else {
		LOGERR("Unknown zip type %i\n", ztype);
		ret_val = INSTALL_CORRUPT;
	}
    if (ret_val) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret_val;
    }

	// Convert the vector to a NULL-terminated char* array suitable for execv.
	const char* chr_args[args.size() + 1];
	chr_args[args.size()] = NULL;
	for (size_t i = 0; i < args.size(); i++)
		chr_args[i] = args[i].c_str();

	pid_t pid = fork();
	if (pid == 0) {
		close(pipe_fd[0]);
		execve(chr_args[0], const_cast<char**>(chr_args), environ);
		printf("E:Can't execute '%s': %s\n", chr_args[0], strerror(errno));
		_exit(-1);
	}
	close(pipe_fd[1]);

	*wipe_cache = 0;

	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
	child_data = fdopen(pipe_fd[0], "r");
	while (fgets(buffer, sizeof(buffer), child_data) != NULL) {
		char* command = strtok(buffer, " \n");
		if (command == NULL) {
			continue;
		} else if (strcmp(command, "progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			char* seconds_char = strtok(NULL, " \n");

			float fraction_float = strtof(fraction_char, NULL);
			int seconds_float = strtol(seconds_char, NULL, 10);

			if (zip_verify)
				DataManager::ShowProgress(fraction_float * (1 - VERIFICATION_PROGRESS_FRACTION), seconds_float);
			else
				DataManager::ShowProgress(fraction_float, seconds_float);
		} else if (strcmp(command, "set_progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			float fraction_float = strtof(fraction_char, NULL);
			DataManager::_SetProgress(fraction_float);
		} else if (strcmp(command, "ui_print") == 0) {
			char* display_value = strtok(NULL, "\n");
	  		if (display_value) {
	      		     if (strcmp(display_value, "AROMA Filemanager Finished...") == 0 && (aroma_running == 1)) {
		  		aroma_running = 0;
		  		gui_changeOverlay("");
		  		TWFunc::copy_file(Fox_aroma_cfg, Fox_sdcard_aroma_cfg, 0644);
			     }
	      		    gui_print("%s", display_value);
	      		    if (strcmp(display_value, "(c) 2013-2015 by amarullz.com") == 0 && (aroma_running == 0)) {
		  		aroma_running = 1;
		  		gui_changeOverlay("black_out");
		  		TWFunc::copy_file(Fox_aroma_cfg, Fox_sdcard_aroma_cfg, 0644);
			     }
	    		}
	  		else {
	      			gui_print("\n");
	    		}
		} else if (strcmp(command, "wipe_cache") == 0) {
			*wipe_cache = 1;
		} else if (strcmp(command, "clear_display") == 0) {
			// Do nothing, not supported by TWRP
		} else if (strcmp(command, "log") == 0) {
			printf("%s\n", strtok(NULL, "\n"));
		} else {
			LOGERR("unknown command [%s]\n", command);
		}
	}
	fclose(child_data);

	int waitrc = TWFunc::Wait_For_Child(pid, &status, "Updater");

  	// Should never happen, but in case of crash or other unexpected condition
  	if (aroma_running == 1) {
      		gui_changeOverlay("");
    	}

  	// if updater-script doesn't find the correct device
  	if (WEXITSTATUS (status) == TW_ERROR_WRONG_DEVICE) {
       		gui_print_color("error", "\nPossible causes of this error:\n  1. Wrong device\n  2. Wrong firmware\n  3. Corrupt zip\n  4. System not mounted\n  5. Bugged updater-script.\n\nSearch online for \"error %i\". ",
       			TW_ERROR_WRONG_DEVICE);
       		gui_print_color("error", "Check \"/tmp/recovery.log\", and look above, for the specific cause of this error.\n\n");
     	}

  	if (waitrc != 0) {
      		set_miui_install_status(OTA_CORRUPT, false);
      		return INSTALL_ERROR;
    	}

	return INSTALL_SUCCESS;
}

// Any process whose executable lives on /vendor keeps the vendor dm open even
// after the mount is gone, and libdm's DM_DEV_REMOVE then fails with EBUSY.
// wlan_bringup starts qrtr-ns/cnss-daemon with nohup, so ctl.stop cannot reach
// them; sweep /proc until no /vendor executable is left.
static void Fox_Kill_Vendor_Processes() {
	for (int pass = 0; pass < 50; pass++) {
		bool found = false;
		DIR* proc = opendir("/proc");
		if (proc == nullptr)
			return;
		struct dirent* entry;
		while ((entry = readdir(proc)) != nullptr) {
			if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
				continue;
			pid_t pid = atoi(entry->d_name);
			if (pid <= 1)
				continue;
			std::string exe = std::string("/proc/") + entry->d_name + "/exe";
			char target[PATH_MAX] = {0};
			ssize_t len = readlink(exe.c_str(), target, sizeof(target) - 1);
			if (len > 0 && strncmp(target, "/vendor/", 8) == 0) {
				found = true;
				kill(pid, SIGKILL);
			}
		}
		closedir(proc);
		if (!found)
			return;
		usleep(100000);
	}
	LOGERR("Fox: processes from /vendor are still present after the kill sweep\n");
}

// The vendor TEE/vibrator services hold /vendor, and a package that rewrites
// dynamic partitions needs the vendor dm released before its updater runs.
// Only such a package stops them (see Fox_Zip_Touches_Dynamic_Partitions and
// the OTA flag); a plain zip keeps them alive, because a restarted HAL answers
// gatekeeper but the synthetic-password unwrap fails until keystore2 rebinds.
// Returns true when at least one running service was stopped.
static bool Fox_Stop_Vendor_Holders() {
	// wpa_supplicant is a supervised service: killing it lets init respawn it
	// straight back onto /vendor, so it has to be stopped by name. wlan_bringup
	// is the oneshot that starts it and may still be inside its wait loop.
	// Both are no-ops without the WLAN stack (the properties are unset).
	// Never gate this on OF_ENABLE_WLAN: libtwrpinstall is a Soong module and
	// does not inherit the define (orangefox.mk adds it to the recovery module
	// only), so the gate would be silently dead code.
	std::vector<const char*> services = {"vendor.qseecomd", "gatekeeper-1-0", "keymaster-4-0", "vendor.qti.vibrator",
					     "wpa_supplicant", "wlan_bringup"};
	bool stopped = false;
	bool wlan_stopped = false;
	for (const char* svc : services) {
		std::string prop = std::string("init.svc.") + svc;
		char value[PROP_VALUE_MAX] = {0};
		property_get(prop.c_str(), value, "");
		if (strcmp(value, "running") != 0)
			continue;
		stopped = true;
		if (strcmp(svc, "wpa_supplicant") == 0 || strcmp(svc, "wlan_bringup") == 0)
			wlan_stopped = true;
		property_set("ctl.stop", svc);
	}
	if (!stopped)
		return false;
	if (wlan_stopped)
		LOGINFO("Fox: stopped the WLAN stack for the install; the next boot restores it\n");
	for (int i = 0; i < 50; i++) {
		bool running = false;
		for (const char* svc : services) {
			std::string prop = std::string("init.svc.") + svc;
			char value[PROP_VALUE_MAX] = {0};
			property_get(prop.c_str(), value, "");
			if (strcmp(value, "running") == 0) {
				running = true;
				break;
			}
		}
		if (!running)
			break;
		usleep(100000);
	}
	Fox_Kill_Vendor_Processes();
	return true;
}

// Restarts what Fox_Stop_Vendor_Holders stopped. The services cannot start
// while /vendor is unmounted, so remount it first when the install left it
// detached. Called from every exit path of TWinstall_zip.
// keystore2 keeps binder proxies to the stopped HALs; a restarted HAL is a new
// instance, so the spblob unwrap fails ("Begin Operation failed") until
// keystore2 rebinds. Verified on device: this restart makes the same PIN
// decrypt successfully in the same session.
static void Fox_Restart_Vendor_Holders() {
	property_set("ctl.start", "vendor.qseecomd");
	for (int i = 0; i < 20; i++) {
		char value[PROP_VALUE_MAX] = {0};
		property_get("init.svc.vendor.qseecomd", value, "");
		if (strcmp(value, "running") == 0)
			break;
		usleep(100000);
	}
	property_set("ctl.start", "gatekeeper-1-0");
	property_set("ctl.start", "keymaster-4-0");
	property_set("ctl.start", "vendor.qti.vibrator");
	bool running = false;
	for (int i = 0; i < 20; i++) {
		char value[PROP_VALUE_MAX] = {0};
		property_get("init.svc.gatekeeper-1-0", value, "");
		if (strcmp(value, "running") == 0) {
			running = true;
			break;
		}
		usleep(100000);
	}
	property_set("ctl.stop", "keystore2");
	property_set("ctl.start", "keystore2");
	bool keystore = false;
	for (int i = 0; i < 20; i++) {
		char value[PROP_VALUE_MAX] = {0};
		property_get("init.svc.keystore2", value, "");
		if (strcmp(value, "running") == 0) {
			keystore = true;
			break;
		}
		usleep(100000);
	}
	if (running && keystore)
		LOGINFO("Fox: restarted the vendor TEE services and keystore2 after the install\n");
	else
		LOGERR("Fox: the TEE restart is incomplete (gatekeeper=%d keystore2=%d); decrypt fails until the next boot\n",
		       (int)running, (int)keystore);
}

// A package that rewrites dynamic partitions must have the vendor dm released,
// not merely unmounted: open files on /vendor keep the device open, and libdm's
// DM_DEV_REMOVE then fails with EBUSY even though the mount is already gone.
// AOSP and LineageOS OTAs name the operand list differently, so accept either.
static bool Fox_Zip_Touches_Dynamic_Partitions(ZipArchiveHandle Zip) {
	const char* signals[] = {
		"dynamic_partitions_info.txt",
		"dynamic_partitions_op_list",
		"super_empty.img",
		"unsparse_super_empty.img",
	};
	for (const char* name : signals) {
		ZipEntry64 entry;
		if (FindEntry(Zip, name, &entry) == 0)
			return true;
	}
	return false;
}

// A package that carries a recovery image overwrites the partition OrangeFox
// lives on — every LineageOS OTA does. That used to be handled only for the
// mapped '@block.map' install in OpenRecoveryScript::Install_Command, so a
// manual flash from the GUI or `fox flash` silently replaced the recovery.
// Decide from the package here, at the one point every install path goes
// through, and keep an exact dd copy: the repacker would rebuild the image
// with the compiled kernel instead of restoring what was actually installed.
static bool Fox_Zip_Ships_Recovery(ZipArchiveHandle Zip) {
	const char* signals[] = {
		"recovery.img",
		"recovery.transfer.list",
	};
	for (const char* name : signals) {
		ZipEntry64 entry;
		if (FindEntry(Zip, name, &entry) == 0)
			return true;
	}
	return false;
}

class FoxRecoverySurvival
{
public:
	// Returns false when the package writes recovery but the copy failed; the
	// caller must abort rather than let the package replace the recovery.
	bool Save_If_Package_Writes_Recovery(ZipArchiveHandle Zip) {
		if (!Fox_Zip_Ships_Recovery(Zip))
			return true;
		if (DataManager::GetIntValue(TW_AUTO_REFLASHTWRP_VAR) == 0)
			return true;
		// This tree's TWPartition has no block-device getter; use the same
		// by-name node as the flash recipe and init.recovery.qcom.rc.
		device = "/dev/block/bootdevice/by-name/recovery";
		if (TWFunc::Exec_Cmd("dd if=" + device + " of=/tmp/fox-recovery-backup.img bs=4M", false) != 0) {
			LOGERR("Fox: unable to back up the recovery partition\n");
			return false;
		}
		backed_up = true;
		LOGINFO("Fox: backed up the recovery partition before the install\n");
		return true;
	}

	~FoxRecoverySurvival()
	{
		if (!backed_up)
			return;
		if (TWFunc::Exec_Cmd("dd if=/tmp/fox-recovery-backup.img of=" + device + " bs=4M; sync", false) != 0) {
			LOGERR("Fox: unable to restore the recovery partition; backup kept at /tmp/fox-recovery-backup.img\n");
			return;
		}
		if (TWFunc::Exec_Cmd("cmp -s /tmp/fox-recovery-backup.img " + device, false) != 0) {
			LOGERR("Fox: the restored recovery differs from the backup; kept at /tmp/fox-recovery-backup.img\n");
			return;
		}
		unlink("/tmp/fox-recovery-backup.img");
		LOGINFO("Fox: restored the OrangeFox recovery image after the install\n");
	}

private:
	bool backed_up = false;
	std::string device;
};

class FoxVendorHolderRestart
{
public:
	bool stopped = false;

	~FoxVendorHolderRestart()
	{
		if (!stopped)
			return;
		if (!PartitionManager.Is_Mounted_By_Path("/vendor"))
			PartitionManager.Mount_By_Path("/vendor", false);
		Fox_Restart_Vendor_Holders();
	}
};

int TWinstall_zip(const char *path, int *wipe_cache, bool check_for_digest)
{
  int ret_val, zip_verify = 1, unmount_system = 1, reflashtwrp = 0, unmount_vendor = 1;
  bool run_rom_scripts = false;
  FoxVendorHolderRestart vendor_restart;

  if (TWFunc::Block_Operations_Until_Reboot())
	return INSTALL_CORRUPT;

  if (strcmp(path, "error") == 0)
    {
      LOGERR("Failed to get adb sideload file: '%s'\n", path);
      return INSTALL_CORRUPT;
    }

  if (DataManager::GetIntValue(FOX_INSTALL_PREBUILT_ZIP) == 1)
     {
         DataManager::SetValue(FOX_ZIP_INSTALLER_CODE, 0); // internal zip = standard zip installer
         DataManager::SetValue(FOX_ZIP_INSTALLER_TREBLE, 0);
     }    
  else   
    {
	gui_msg(Msg("installing_zip=Installing zip file '{1}'")(path));
	if (strlen(path) < 9 || strncmp(path, "/sideload", 9) != 0) {
		string digest_str;
		string Full_Filename = path;

		if (check_for_digest) {
			gui_msg("check_for_digest=Checking for Digest file...");
			if (*path != '@' && !twrpDigestDriver::Check_File_Digest(Full_Filename)) {
				LOGERR("Aborting zip install: Digest verification failed\n");
				return INSTALL_CORRUPT;
			}
		}
	}
    }

  DataManager::GetValue(TW_UNMOUNT_SYSTEM, unmount_system);
  DataManager::GetValue(TW_UNMOUNT_VENDOR, unmount_vendor);

#ifndef TW_OEM_BUILD
  DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
#endif

  DataManager::SetProgress(0);

	auto package = Package::CreateMemoryPackage(path);
	if (!package) {
		return INSTALL_CORRUPT;
	}

	if (zip_verify) {
		gui_msg("verify_zip_sig=Verifying zip signature...");
		static constexpr const char* CERTIFICATE_ZIP_FILE = "/system/etc/security/otacerts.zip";
		std::vector<Certificate> loaded_keys = LoadKeysFromZipfile(CERTIFICATE_ZIP_FILE);
		if (loaded_keys.empty()) {
			LOGERR("Failed to load keys\n");
			return -1;
		}
		LOGINFO("%zu key(s) loaded from %s\n", loaded_keys.size(), CERTIFICATE_ZIP_FILE);

		ret_val = verify_file(package.get(), loaded_keys, std::bind(&DataManager::SetProgress, std::placeholders::_1));
		if (ret_val != VERIFY_SUCCESS) {
			LOGINFO("Zip signature verification failed: %i\n", ret_val);
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		} else {
			gui_msg("verify_zip_done=Zip signature verified successfully.");
		}
    }
    
    ZipArchiveHandle Zip = package->GetZipArchiveHandle();
    if (!Zip) {
      set_miui_install_status(OTA_CORRUPT, true);
      gui_err("zip_corrupt=Zip file is corrupt!");
      return INSTALL_CORRUPT;
    }

    // Save the recovery partition before a package that ships its own can
    // overwrite it; the guard puts it back on every exit path.
    FoxRecoverySurvival recovery_survival;
    if (!recovery_survival.Save_If_Package_Writes_Recovery(Zip)) {
      gui_err("fox_recovery_backup_failed=Unable to back up the recovery partition; install aborted");
      return -1;
    }

    if (unmount_system) {
	if (PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path())) {
		gui_msg("unmount_system=Unmounting System...");
		if (PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false)) {
			//unlink(PartitionManager.Get_Android_Root_Path().c_str());
			//mkdir(PartitionManager.Get_Android_Root_Path().c_str(), 0755);
		}
		else {
			gui_msg("unmount_system_err=Failed to unmount System");
		        return -1;
		}
	}
   }

   if (unmount_vendor) {
	if (PartitionManager.Is_Mounted_By_Path("/vendor")) {
		gui_msg("unmount_vendor=Unmounting Vendor...");
		// The fstab mounts firmware_mnt/dsp/bt_firmware as separate mounts;
		// the partition list does not always map them, so umount(2) them
		// directly, otherwise the parent /vendor stays busy.
		umount("/vendor/firmware_mnt");
		umount("/vendor/dsp");
		umount("/vendor/bt_firmware");
		// A package that rewrites super/vendor needs the vendor dm released,
		// so the services running from it are stopped before the unmount. A
		// plain zip keeps them: they do not block the unmount, and stopping
		// them costs the session's decrypt path (see the restart helper).
		// The package itself is the signal, not how the install was started:
		// a mapped OTA carries the same operand list as a manual flash.
		if (Fox_Zip_Touches_Dynamic_Partitions(Zip))
			vendor_restart.stopped = Fox_Stop_Vendor_Holders();
		bool vendor_unmounted = PartitionManager.UnMount_By_Path("/vendor", false);
		if (!vendor_unmounted) {
			// /vendor is a read-only image and is restarted on the next boot;
			// lazy detach is safer than an arbitrary EBUSY retry loop.
			if (umount2("/vendor", MNT_DETACH) == 0) {
				LOGINFO("Fox: detached busy /vendor before the install\n");
				vendor_unmounted = true;
			} else {
				LOGERR("Fox: /vendor detach failed: %s\n", strerror(errno));
			}
		}
		if (!vendor_unmounted) {
			gui_msg("unmount_vendor_err=Failed to unmount Vendor");
			return -1;
		}
	}
   }

   // DJ9, 20200622: try to avoid a situation where blockimg will bomb out when trying to create a stash
   if (TWFunc::Path_Exists("/cache/.") && !TWFunc::Path_Exists("/cache/recovery/.") && !TWFunc::Path_Exists("/data/cache/.")) {
	LOGINFO("Recreating the /cache/recovery/ folder ...\n");
	if (!TWFunc::Recursive_Mkdir("/cache/recovery", false))
	   LOGERR("Could not create /cache/recovery - blockimg may have problems with creating stashes\n");
   }
   // DJ9

  time_t start, stop;
  time(&start);

  std::string update_binary_name(UPDATE_BINARY_NAME);
  ZipEntry64 update_binary_entry;
  if (FindEntry(Zip, update_binary_name, &update_binary_entry) == 0) {
		LOGINFO("Update binary zip\n");
		// Additionally verify the compatibility of the package.
		if (!Fox_Skip_Treble_Compatibility_Check() && !verify_package_compatibility(Zip)) {
			gui_err("zip_compatible_err=Zip Treble compatibility error!");
			ret_val = INSTALL_CORRUPT;
		} else {
			ret_val = Prepare_Update_Binary(path, Zip);
			if (ret_val == INSTALL_SUCCESS) {
				usleep(32);
				run_rom_scripts = ((DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0) // only run after flashing a ROM
	  			&& (DataManager::GetIntValue(FOX_INSTALL_PREBUILT_ZIP) != 1)); // don't run for built-in zips

				if (run_rom_scripts && TWFunc::Path_Exists(FOX_PRE_ROM_FLASH_SCRIPT)) {
					TWFunc::RunFoxScript(FOX_PRE_ROM_FLASH_SCRIPT, path);
	  			}

				ret_val = Run_Update_Binary(path, wipe_cache, UPDATE_BINARY_ZIP_TYPE);

				if (DataManager::GetIntValue("fox_processing_asserts") != 0) {
					TWFunc::Fox_Property_Set("ro.product.device", DataManager::GetStrValue("fox_product_device"));
					DataManager::SetValue("fox_processing_asserts", "0");
					LOGINFO("\nDevice code name restored\n");
				}
			}
		}
	} else {
		std::string ab_binary_name(AB_OTA);
		ZipEntry64 ab_binary_entry;
		if (FindEntry(Zip, ab_binary_name, &ab_binary_entry) == 0) {
			LOGINFO("AB zip\n");
			gui_msg(Msg(msg::kHighlight, "flash_ab_inactive=Flashing A/B zip to inactive slot: {1}")(PartitionManager.Get_Active_Slot_Display()=="A"?"B":"A"));
			// We need this so backuptool can do its magic
			bool system_mount_state = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
			bool vendor_mount_state = PartitionManager.Is_Mounted_By_Path("/vendor");
			PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			PartitionManager.Mount_By_Path("/vendor", false);
			TWFunc::copy_file("/system/bin/sh", "/tmp/sh", 0755);
			mount("/tmp/sh", "/system/bin/sh", "auto", MS_BIND, NULL);

			run_rom_scripts = true;
			usleep(32);

			if (run_rom_scripts && TWFunc::Path_Exists(FOX_PRE_ROM_FLASH_SCRIPT)) {
				TWFunc::RunFoxScript(FOX_PRE_ROM_FLASH_SCRIPT, path);
			}

			TWFunc::IsRecoveryOverwritten(true);

			ret_val = Run_Update_Binary(path, wipe_cache, AB_OTA_ZIP_TYPE);

			DataManager::SetValue(FOX_ZIP_INSTALLER_CODE, 1); // mark as custom ROM install

			umount("/system/bin/sh");
			unlink("/tmp/sh");
			if (!vendor_mount_state)
				PartitionManager.UnMount_By_Path("/vendor", false);
			if (!system_mount_state)
				PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
			if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
				PartitionManager.Unlock_Block_Partitions();
				PartitionManager.Prepare_All_Super_Volumes();
				gui_warn("mount_vab_partitions=Devices on super may not mount until after rebooting recovery.");
			}
			gui_warn("flash_ab_reboot=To flash additional zips, please reboot recovery to switch to the updated slot.");
		} else {
			std::string binary_name("ui.xml");
			ZipEntry64 binary_entry;
			if (FindEntry(Zip, binary_name, &binary_entry) == 0) {
				LOGINFO("OrangeFox theme zip\n");
				ret_val = Install_Theme(path, Zip);
			} else {
				ret_val = INSTALL_CORRUPT;
			}
		}
   }

  time(&stop);
  int total_time = (int) difftime(stop, start);

  if (ret_val == INSTALL_CORRUPT)
    {
        set_miui_install_status(OTA_CORRUPT, true);
        gui_err("invalid_zip_format=Invalid zip file format!");
    }
  else
  if (ret_val == INSTALL_ERROR)
     {
	set_miui_install_status(OTA_ERROR, false);
     }
  else // success - so let us see whether we need to run OTA_BAK
  {
     // if MIUI-specific features have been disabled
     if (Fox_Skip_OTA()) // yes
     {
         //LOGINFO("OrangeFox: not running the incremental OTA backup (OTA_BAK).\n");
     }  
     else // else let us proceed with the OTA stuff
     if (DataManager::GetIntValue(FOX_INCREMENTAL_OTA_FAIL) != 1)
     {
      	if (DataManager::GetIntValue(FOX_INCREMENTAL_PACKAGE) == 1 && DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0)
      	  {
      	    if (TWinstall_Run_OTA_BAK (true)) // true, because the value of Fox_Zip_Installer_Code to be set
      	      {
	        if (Fox_OTA_Backup_Stock_Boot_Image()) // whether to create an additional backup of the stock boot image
	           {
	      		usleep(2048);
	      		string ota_folder = DataManager::GetStrValue("ota_bak_folder");
	      		usleep(2048);
	      		if (ota_folder.empty())
	      		   ota_folder = FOX_OTA_PATH;
			string ota_bootimg = ota_folder + "/boot.img";
			if (TWFunc::Path_Exists(boot_bak_img)) {
			   if (TWFunc::copy_file(boot_bak_img, ota_bootimg, 0644) == 0) {
			   	LOGINFO("OrangeFox: stock boot image extracted into the OTA directory.\n");
			     }
			   unlink(boot_bak_img.c_str());
		 	}
	           }
      	      }
      	  }

      	DataManager::SetValue(FOX_METADATA_PRE_BUILD, 0);
      	DataManager::SetValue(FOX_MIUI_ZIP_TMP, 0);
      	DataManager::SetValue(FOX_INCREMENTAL_OTA_FAIL, 0);
      	DataManager::SetValue(FOX_LOADED_FINGERPRINT, 0);
      	DataManager::SetValue(FOX_RUN_SURVIVAL_BACKUP, 0);
      
     } // end of OTA stuff
    LOGINFO("Install took %i second(s).\n", total_time);
   }

   if (ret_val == INSTALL_SUCCESS)
      set_miui_install_status(OTA_SUCCESS, false);

   usleep(32);
   if (DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE) != 0) // just flashed a ROM
   {
      usleep(16);
      TWFunc::Check_OrangeFox_Overwrite_FromROM(false, path);
   }

   if (run_rom_scripts && TWFunc::Path_Exists(FOX_POST_ROM_FLASH_SCRIPT)) {
   	usleep(2048);
   	TWFunc::RunFoxScript(FOX_POST_ROM_FLASH_SCRIPT, path);
   	sleep(1);
   	DataManager::SetValue("found_fox_overwriting_rom", "0");
   	TWFunc::Fox_Property_Set("found_fox_overwriting_rom", "");
   }

  return ret_val;
}
