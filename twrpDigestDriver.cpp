/*
	Copyright 2013 to 2017 TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

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


#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "data.hpp"
#include "partitions.hpp"
#include "set_metadata.h"
#include "twrpDigestDriver.hpp"
#include "twrp-functions.hpp"
#include "twcommon.h"
#include "variables.h"
#include "gui/gui.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrpDigest/twrpSHA.hpp"

std::vector<string> PartFilenames;

bool twrpDigestDriver::Check_File_Digest(const string& Filename) {
	twrpDigest *digest;
	string digestfile = Filename, file_name = Filename;
	string digest_str;
	bool use_sha2 = false;

#ifndef TW_NO_SHA2_LIBRARY

	digestfile += ".sha2";
	if (TWFunc::Path_Exists(digestfile)) {
		digest = new twrpSHA256();
		use_sha2 = true;
	}
	else {
		digestfile = Filename + ".sha256";
		if (TWFunc::Path_Exists(digestfile)) {
			digest = new twrpSHA256();
			use_sha2 = true;
		} else {
			digest = new twrpMD5();
			digestfile = Filename + ".md5";
			if (!TWFunc::Path_Exists(digestfile)) {
				digestfile = Filename + ".md5sum";
			}
		}
	}
#else
	digest = new twrpMD5();
	digestfile = Filename + ".md5";
	if (!TWFunc::Path_Exists(digestfile)) {
		digestfile = Filename + ".md5sum";
	}

#endif

	if (!TWFunc::Path_Exists(digestfile)) {
		delete digest;
		gui_msg(Msg(msg::kWarning, "no_digest=Skipping Digest check: no Digest file found"));
		return true;
	}


	if (TWFunc::read_file(digestfile, digest_str) != 0) {
		gui_msg("digest_error=Digest Error!");
		delete digest;
		return false;
	}

	if (!stream_file_to_digest(file_name, digest)) {
		delete digest;
		return false;
	}
	string digest_check = digest->return_digest_string();
	digest_check = digest_check + "  " + TWFunc::Get_Filename(file_name);
	if (digest_check == digest_str) {
		if (use_sha2)
			LOGINFO("SHA2 Digest: %s  %s\n", digest_str.c_str(), TWFunc::Get_Filename(Filename).c_str());
		else
			LOGINFO("MD5 Digest: %s  %s\n", digest_str.c_str(), TWFunc::Get_Filename(Filename).c_str());
		gui_msg(Msg("digest_matched=Digest matched for '{1}'.")(Filename));
		delete digest;
		return true;
	}

	gui_msg(Msg(msg::kError, "digest_fail_match=Digest failed to match on '{1}'.")(Filename));
	delete digest;
	return false;
}

// Chunks of a split backup are named <base><thread><archive> using the same
// "%i%02i" scheme that tarList() writes and extractMulti()/get_size() read:
// data.f2fs.win000 = thread 0 archive 0, data.f2fs.win100 = thread 1 archive
// 0, data.f2fs.win101 = thread 1 archive 1. Threads are 0..8 (core_count is
// capped at 8; thread 0 is the unencrypted app/dalvik segment) and archives
// run 0..MAX_ARCHIVE_COUNT per thread, contiguously - so a missing index ends
// that thread's series. The digest code used to walk a single flat "%03i"
// sequence and stop at the FIRST gap, so every chunk written by threads 1..8
// (win100+) was silently never digested or digest-checked (apollon OTG
// backups, 2026-10-17: 6 of 14 chunks covered).
static std::vector<string> Split_Archive_Chunks(const string& base) {
	std::vector<string> chunks;
	char name[512];

	for (int thread = 0; thread <= 8; thread++) {
		for (int archive = 0; archive <= MAX_ARCHIVE_COUNT; archive++) {
			sprintf(name, "%s%i%02i", base.c_str(), thread, archive);
			if (!TWFunc::Path_Exists(name))
				break;
			chunks.push_back(name);
		}
	}
	return chunks;
}

bool twrpDigestDriver::Check_Digest(string Full_Filename) {
	sync();
	if (!TWFunc::Path_Exists(Full_Filename)) {
		// Split archive: check every chunk that exists. Chunks without a
		// digest file warn and pass (Check_File_Digest), so backups made by
		// the old generator still restore.
		std::vector<string> chunks = Split_Archive_Chunks(Full_Filename);
		for (size_t i = 0; i < chunks.size(); i++) {
			LOGINFO("split_filename: %s\n", chunks[i].c_str());
			if (!Check_File_Digest(chunks[i]))
				return false;
		}
		return true;
	}
	return Check_File_Digest(Full_Filename); // Single file archive
}

bool twrpDigestDriver::Write_Digest(string Full_Filename) {
	twrpDigest *digest;
	string digest_filename, digest_str;
	int use_sha2;

#ifdef TW_NO_SHA2_LIBRARY
	use_sha2 = 0;
#else
	DataManager::GetValue(TW_USE_SHA2, use_sha2);
#endif

	if (use_sha2) {
#ifndef TW_NO_SHA2_LIBRARY
		digest = new twrpSHA256();
		digest_filename = Full_Filename + ".sha2";
		if (!stream_file_to_digest(Full_Filename, digest)) {
			delete digest;
			return false;
		}
		digest_str = digest->return_digest_string();
		if (digest_str.empty()) {
			delete digest;
			return false;
		}
		LOGINFO("SHA2 Digest: %s  %s\n", digest_str.c_str(), TWFunc::Get_Filename(Full_Filename).c_str());
#endif
	}
	else  {
		digest = new twrpMD5();
		digest_filename = Full_Filename + ".md5";
		if (!stream_file_to_digest(Full_Filename, digest)) {
			delete digest;
			return false;
		}
		digest_str = digest->return_digest_string();
		if (digest_str.empty()) {
			delete digest;
			return false;
		}
		LOGINFO("MD5 Digest: %s  %s\n", digest_str.c_str(), TWFunc::Get_Filename(Full_Filename).c_str());
	}

	digest_str = digest_str + "  " + TWFunc::Get_Filename(Full_Filename) + "\n";
	LOGINFO("digest_filename: %s\n", digest_filename.c_str());

	if (TWFunc::write_to_file(digest_filename, digest_str)) {
		tw_set_default_metadata(digest_filename.c_str());
		gui_msg("digest_created= * Digest Created.");
	}
	else {
		gui_err("digest_error= * Digest Error!");
		delete digest;
		return false;
	}
	delete digest;
	return true;
}

bool twrpDigestDriver::Make_Digest(string Full_Filename) {
	string command, result;

	TWFunc::GUI_Operation_Text(TW_GENERATE_DIGEST_TEXT, gui_parse_text("{@generating_digest1}"));
	gui_msg("generating_digest2= * Generating digest...");
	if (TWFunc::Path_Exists(Full_Filename)) {
		if (!Write_Digest(Full_Filename))
			return false;
	} else {
		// Split archive: digest every chunk (see Split_Archive_Chunks).
		std::vector<string> chunks = Split_Archive_Chunks(Full_Filename);
		if (chunks.empty()) {
			LOGERR("Backup file: '%s' not found!\n", Full_Filename.c_str());
			return false;
		}
		for (size_t i = 0; i < chunks.size(); i++) {
			if (!Write_Digest(chunks[i]))
				return false;
		}
		gui_msg("digest_created= * Digest Created.");
	}
	return true;
}

bool twrpDigestDriver::stream_file_to_digest(string filename, twrpDigest* digest) {
	char buf[4096];
	int bytes;

	int fd = open(filename.c_str(), O_RDONLY);
	if (fd < 0) {
		return false;
	}
	while ((bytes = read(fd, &buf, sizeof(buf))) != 0) {
		digest->update((unsigned char*)buf, bytes);
	}
	close(fd);
	return true;
}

int twrpDigestDriver::Run_Digest() { //translate
	gui_msg("con_digest_start=[DIGEST CREATION STARTED]");

	if (PartFilenames.empty()) {
		gui_msg(Msg(msg::kError, "con_digest_error=Partition list is empty!"));
		return 1;	
	}

	DataManager::SetValue(TW_ACTION_BUSY, "1");
	bool ret_val = 0;
	int vector_size = PartFilenames.size();
  	time_t total_start, total_stop;

    DataManager::SetValue("ui_progress", 0);

  	time(&total_start);

	for (int i = 0; i < vector_size; i++) {
		gui_print("%s\n", basename(PartFilenames[i].c_str()));
        if (!twrpDigestDriver::Make_Digest(PartFilenames[i])) {
			ret_val = 1;
			break;
		}

		int progress = (int) (((float) (vector_size - i) / (float) vector_size) * 100.0);
    	DataManager::SetValue("ui_progress", progress);
    }
	PartFilenames.clear();
	DataManager::SetValue("fox_show_digest_btn", "0");
	DataManager::SetValue(TW_ACTION_BUSY, "0");

  	time(&total_stop);
  	int total_time = (int) difftime(total_stop, total_start);
	gui_msg(Msg(msg::kHighlight, "con_digest_complete=[DIGEST CREATION COMPLETED IN {1} SECONDS]") (total_time));

	return ret_val;
}

