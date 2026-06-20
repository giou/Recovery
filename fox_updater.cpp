#include "fox_updater.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <cutils/properties.h>
#include <json/json.h>

#include "data.hpp"
#include "partitions.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "twinstall.h"

using std::string;
using std::vector;

namespace {

static const string API_BASE = "https://api.orangefox.download";
static const string STATE_DIR = "/tmp/fox_update";
static const string DOWNLOAD_DIR = "/sdcard/OrangeFoxUpdates";
static const string TMP_DOWNLOAD_DIR = "/tmp/OrangeFoxUpdates";
static const string USER_AGENT = "OrangeFoxRecovery-FoxUpdater/1.0";

struct FoxRelease {
	string id;          // Internal API release ID
	string build_id;    // fox_id / build_id
	string device_id;
	string version;
	string type;
	string filename;
	string md5;
	string size;
	string date;
	string variant;
	string download_url;
	vector<string> changelog;
	vector<string> notes;
	vector<string> bugs;
};

struct ReleaseContext {
	string version;
	string type;
	string device_id;
};

static FoxRelease g_current_release;
static FoxRelease g_selected_release;
static vector<FoxRelease> g_variants;
static int g_variant_index = 0;

static string Trim(const string& in) {
	size_t start = 0;
	while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])))
		start++;

	size_t end = in.size();
	while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])))
		end--;

	return in.substr(start, end - start);
}

static bool ContainsNoCase(const string& s, const string& needle) {
	string a = s;
	string b = needle;

	std::transform(a.begin(), a.end(), a.begin(), ::tolower);
	std::transform(b.begin(), b.end(), b.begin(), ::tolower);

	return a.find(b) != string::npos;
}

static bool FileExists(const string& path) {
	return access(path.c_str(), F_OK) == 0;
}

static bool ReadFile(const string& path, string& out) {
	std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
	if (!ifs)
		return false;

	std::ostringstream ss;
	ss << ifs.rdbuf();
	out = ss.str();
	return true;
}

static bool WriteFile(const string& path, const string& value) {
	std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
	if (!ofs)
		return false;

	ofs << value;

	if (value.empty() || value[value.size() - 1] != '\n')
		ofs << '\n';

	return true;
}

static string ReadFileTrimmed(const string& path, const string& fallback = "") {
	string data;
	if (!ReadFile(path, data))
		return fallback;
	return Trim(data);
}

static bool MkDirOne(const string& path) {
	if (path.empty())
		return false;

	if (mkdir(path.c_str(), 0755) == 0)
		return true;

	if (errno == EEXIST)
		return true;

	return false;
}

static bool MkDirs(const string& path) {
	if (path.empty())
		return false;

	string current;

	if (path[0] == '/')
		current = "/";

	size_t i = 0;

	while (i < path.size()) {
		while (i < path.size() && path[i] == '/')
			i++;

		size_t j = i;
		while (j < path.size() && path[j] != '/')
			j++;

		if (j <= i)
			break;

		if (!current.empty() && current[current.size() - 1] != '/')
			current += "/";

		current += path.substr(i, j - i);

		if (!MkDirOne(current))
			return false;

		i = j;
	}

	return true;
}

static void EnsureStateDir() {
	MkDirs(STATE_DIR);
}

static void SetVar(const string& key, const string& value) {
	DataManager::SetValue(key, value);
}

static string GetVar(const string& key) {
	string value;
	DataManager::GetValue(key, value);
	return Trim(value);
}

static void StateWrite(const string& name, const string& value) {
	EnsureStateDir();
	WriteFile(STATE_DIR + "/" + name, value);
}

static string StateRead(const string& name, const string& fallback = "") {
	return ReadFileTrimmed(STATE_DIR + "/" + name, fallback);
}

static void SetStateVar(const string& var, const string& file, const string& value) {
	SetVar(var, value);
	StateWrite(file, value);
}

static void Status(const string& message) {
	gui_print("%s\n", message.c_str());
	SetStateVar("fox_update_status", "status.txt", message);
}

static string ShellQuote(const string& s) {
	string out = "'";

	for (char c : s) {
		if (c == '\'')
			out += "'\\''";
		else
			out.push_back(c);
	}

	out += "'";
	return out;
}

static int RunCommand(const string& command, string* output = nullptr) {
	string full = command + " 2>&1";

	FILE* pipe = popen(full.c_str(), "r");
	if (!pipe)
		return 127;

	char buffer[4096];

	while (fgets(buffer, sizeof(buffer), pipe)) {
		if (output)
			(*output) += buffer;
		else
			gui_print("%s", buffer);
	}

	int rc = pclose(pipe);

	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);

	return rc;
}

static string UrlEncode(const string& in) {
	static const char hex[] = "0123456789ABCDEF";
	string out;

	for (unsigned char c : in) {
		if ((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[(c >> 4) & 0xF]);
			out.push_back(hex[c & 0xF]);
		}
	}

	return out;
}

static bool HttpGetToFile(const string& url, const string& out_path) {
	unlink(out_path.c_str());

	string cmd =
		"if command -v curl >/dev/null 2>&1; then "
		"curl -L --fail --connect-timeout 20 --retry 2 "
		"-A " + ShellQuote(USER_AGENT) + " "
		"-o " + ShellQuote(out_path) + " " + ShellQuote(url) + "; "
		"elif command -v wget >/dev/null 2>&1; then "
		"wget -U " + ShellQuote(USER_AGENT) + " "
		"-O " + ShellQuote(out_path) + " " + ShellQuote(url) + "; "
		"elif command -v busybox >/dev/null 2>&1; then "
		"busybox wget -U " + ShellQuote(USER_AGENT) + " "
		"-O " + ShellQuote(out_path) + " " + ShellQuote(url) + "; "
		"else "
		"echo 'No HTTPS downloader found. Add curl or wget to recovery.'; "
		"exit 127; "
		"fi";

	string output;
	int ret = RunCommand(cmd, &output);

	if (!output.empty())
		gui_print("%s", output.c_str());

	if (ret != 0)
		return false;

	struct stat st;
	if (stat(out_path.c_str(), &st) != 0)
		return false;

	return st.st_size > 0;
}

static bool ApiGet(const string& path, const string& out_path) {
	string url = API_BASE + path;

	gui_print("FoxUpdater API: %s\n", url.c_str());

	return HttpGetToFile(url, out_path);
}

static const Json::Value* ObjGet(const Json::Value& v, const string& key) {
	if (!v.isObject() || !v.isMember(key))
		return nullptr;

	return &v[key];
}

static bool ObjHas(const Json::Value& v, const string& key) {
	return ObjGet(v, key) != nullptr;
}

static string JsonAsString(const Json::Value* v, const string& fallback = "") {
	if (!v)
		return fallback;

	if (v->isString() || v->isNumeric() || v->isBool() || v->isNull())
		return v->asString();

	return fallback;
}

static string ObjString(const Json::Value& v, const string& key, const string& fallback = "") {
	return JsonAsString(ObjGet(v, key), fallback);
}

static string ObjStringAny(const Json::Value& v, const vector<string>& keys, const string& fallback = "") {
	for (const string& key : keys) {
		string s = ObjString(v, key, "");
		if (!s.empty())
			return s;
	}
	return fallback;
}

static vector<string> ObjStringArray(const Json::Value& v, const string& key) {
	vector<string> out;

	const Json::Value* item = ObjGet(v, key);
	if (!item)
		return out;

	if (item->isString()) {
		out.push_back(item->asString());
		return out;
	}

	if (!item->isArray())
		return out;

	for (Json::ArrayIndex i = 0; i < item->size(); i++) {
		const Json::Value& entry = (*item)[i];
		string s = JsonAsString(&entry, "");
		if (!Trim(s).empty())
			out.push_back(s);
	}

	return out;
}

static bool ParseJsonFile(const string& path, Json::Value& root) {
	string data;

	if (!ReadFile(path, data))
		return false;

	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;

	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(data.data(), data.data() + data.size(), &root, &errors);
}

static bool LooksLikeReleaseObject(const Json::Value& v) {
	if (!v.isObject())
		return false;

	bool has_id = ObjHas(v, "id") || ObjHas(v, "_id") || ObjHas(v, "build_id");
	bool has_release_marker =
		ObjHas(v, "build_id") ||
		ObjHas(v, "filename") ||
		ObjHas(v, "mirrors") ||
		ObjHas(v, "md5") ||
		ObjHas(v, "device_id");

	bool has_release_data =
		ObjHas(v, "version") ||
		ObjHas(v, "type") ||
		ObjHas(v, "date") ||
		ObjHas(v, "filename");

	return has_id && has_release_marker && has_release_data;
}

static string InferVariantFromFilename(const string& filename) {
	if (filename.empty())
		return "";

	vector<string> parts;

	if (ContainsNoCase(filename, "miui"))
		parts.push_back("MIUI");

	if (ContainsNoCase(filename, "oos") || ContainsNoCase(filename, "oxygen"))
		parts.push_back("OOS");

	if (ContainsNoCase(filename, "coloros"))
		parts.push_back("ColorOS");

	if (ContainsNoCase(filename, "los") || ContainsNoCase(filename, "lineage"))
		parts.push_back("LineageOS");

	if (ContainsNoCase(filename, "aosp"))
		parts.push_back("AOSP");

	if (ContainsNoCase(filename, "a17") || ContainsNoCase(filename, "android17"))
		parts.push_back("A17");
	else if (ContainsNoCase(filename, "a16") || ContainsNoCase(filename, "android16"))
		parts.push_back("A16");
	else if (ContainsNoCase(filename, "a15") || ContainsNoCase(filename, "android15"))
		parts.push_back("A15");
	else if (ContainsNoCase(filename, "a14") || ContainsNoCase(filename, "android14"))
		parts.push_back("A14");

	if (parts.empty())
		return "";

	string out;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i)
			out += " / ";
		out += parts[i];
	}

	return out;
}

static string PickMirrorUrl(const Json::Value& v) {
	const Json::Value* mirrors = ObjGet(v, "mirrors");

	if (!mirrors)
		return "";

	if (mirrors->isObject()) {
		string dl = ObjString(*mirrors, "DL", "");
		if (!dl.empty())
			return dl;

		for (const string& key : mirrors->getMemberNames()) {
			const Json::Value& mirror = (*mirrors)[key];
			string value = JsonAsString(&mirror, "");
			if (!value.empty())
				return value;

			if (mirror.isObject()) {
				string url = ObjStringAny(mirror, {"url", "download_url", "href"}, "");
				if (!url.empty())
					return url;
			}
		}
	}

	if (mirrors->isArray()) {
		for (Json::ArrayIndex i = 0; i < mirrors->size(); i++) {
			const Json::Value& item = (*mirrors)[i];
			string value = JsonAsString(&item, "");
			if (!value.empty())
				return value;

			if (item.isObject()) {
				string url = ObjStringAny(item, {"url", "download_url", "href"}, "");
				if (!url.empty())
					return url;
			}
		}
	}

	return "";
}

static FoxRelease ParseRelease(const Json::Value& obj, const ReleaseContext& ctx) {
	FoxRelease r;

	r.id = ObjStringAny(obj, {"id", "_id"}, "");
	r.build_id = ObjStringAny(obj, {"build_id"}, "");
	r.device_id = ObjStringAny(obj, {"device_id"}, ctx.device_id);
	r.version = ObjStringAny(obj, {"version"}, ctx.version);
	r.type = ObjStringAny(obj, {"type"}, ctx.type);
	r.filename = ObjStringAny(obj, {"filename"}, "");
	r.md5 = ObjStringAny(obj, {"md5"}, "");
	r.size = ObjStringAny(obj, {"size"}, "");
	r.date = ObjStringAny(obj, {"date"}, "");

	r.variant = ObjStringAny(obj, {"variant"}, "");

	if (r.variant.empty())
		r.variant = InferVariantFromFilename(r.filename);

	if (r.variant.empty())
		r.variant = r.filename.empty() ? "default" : r.filename;

	r.download_url = PickMirrorUrl(obj);

	string url = ObjStringAny(obj, {"url"}, "");
	if (r.download_url.empty() && url.find(".zip") != string::npos)
		r.download_url = url;

	r.changelog = ObjStringArray(obj, "changelog");
	r.notes = ObjStringArray(obj, "notes");
	r.bugs = ObjStringArray(obj, "bugs");

	return r;
}

static void CollectReleasesRecursive(const Json::Value& v, vector<FoxRelease>& out, ReleaseContext ctx) {
	if (v.isObject()) {
		string inherited_version = ObjStringAny(v, {"version"}, "");
		string inherited_type = ObjStringAny(v, {"type"}, "");
		string inherited_device = ObjStringAny(v, {"device_id"}, "");

		if (!inherited_version.empty())
			ctx.version = inherited_version;

		if (!inherited_type.empty())
			ctx.type = inherited_type;

		if (!inherited_device.empty())
			ctx.device_id = inherited_device;

		if (LooksLikeReleaseObject(v)) {
			FoxRelease r = ParseRelease(v, ctx);
			out.push_back(r);
		}

		for (const string& key : v.getMemberNames())
			CollectReleasesRecursive(v[key], out, ctx);

		return;
	}

	if (v.isArray()) {
		for (Json::ArrayIndex i = 0; i < v.size(); i++)
			CollectReleasesRecursive(v[i], out, ctx);
	}
}

static vector<FoxRelease> DedupReleases(const vector<FoxRelease>& in) {
	vector<FoxRelease> out;
	vector<string> seen;

	for (const FoxRelease& r : in) {
		string key;

		if (!r.id.empty())
			key = "id:" + r.id;
		else if (!r.build_id.empty())
			key = "build:" + r.build_id;
		else
			key = "file:" + r.filename;

		if (key == "file:")
			continue;

		if (std::find(seen.begin(), seen.end(), key) != seen.end())
			continue;

		seen.push_back(key);
		out.push_back(r);
	}

	return out;
}

static double ReleaseDateNumber(const FoxRelease& r) {
	if (r.date.empty())
		return 0.0;

	char* end = nullptr;
	double value = strtod(r.date.c_str(), &end);

	if (end == r.date.c_str())
		return 0.0;

	return value;
}

static void SortReleasesByDateDesc(vector<FoxRelease>& releases) {
	std::sort(releases.begin(), releases.end(), [](const FoxRelease& a, const FoxRelease& b) {
		double da = ReleaseDateNumber(a);
		double db = ReleaseDateNumber(b);

		if (da != db)
			return da > db;

		return a.version > b.version;
	});
}

static string PropertyFirst(const vector<string>& props) {
	char value[PROPERTY_VALUE_MAX];

	for (const string& prop : props) {
		memset(value, 0, sizeof(value));
		property_get(prop.c_str(), value, "");

		string s = Trim(value);
		if (!s.empty())
			return s;
	}

	return "";
}

static string ShellGetProp(const string& prop_name) {
	string output;
	string cmd = "getprop " + ShellQuote(prop_name);

	int ret = RunCommand(cmd, &output);
	if (ret != 0)
		return "";

	return Trim(output);
}

static string DetectCurrentBuildId() {
	string override = GetVar("fox_update_current_build_id_override");
	if (!override.empty()) {
		gui_print("FoxUpdater: using override build_id: %s\n", override.c_str());
		return override;
	}

	string prop = PropertyFirst({
		"ro.build.fox_id",
		"ro.orangefox.build_id",
		"ro.orangefox.fox_id",
		"ro.fox.build_id",
		"ro.fox.fox_id",
		"ro.orangefox.id"
	});

	if (!prop.empty()) {
		gui_print("FoxUpdater: found build_id from property API: %s\n", prop.c_str());
		return prop;
	}

	/*
	 * Recovery property_get() can be weird depending on init/property context.
	 * Shell getprop works on your device, so use it as fallback.
	 */
	vector<string> shell_props = {
		"ro.build.fox_id",
		"ro.orangefox.build_id",
		"ro.orangefox.fox_id",
		"ro.fox.build_id",
		"ro.fox.fox_id",
		"ro.orangefox.id"
	};

	for (const string& p : shell_props) {
		string value = ShellGetProp(p);
		if (!value.empty()) {
			gui_print("FoxUpdater: found build_id from getprop %s: %s\n", p.c_str(), value.c_str());
			return value;
		}
	}

#ifdef FOX_UPDATER_BUILD_ID
	if (strlen(FOX_UPDATER_BUILD_ID) > 0) {
		gui_print("FoxUpdater: using compiled build_id: %s\n", FOX_UPDATER_BUILD_ID);
		return string(FOX_UPDATER_BUILD_ID);
	}
#endif

	gui_print("FoxUpdater: no fox_id/build_id found\n");
	return "";
}

static void ResetRuntimeState() {
	EnsureStateDir();

	SetStateVar("fox_update_status", "status.txt", "Press Check for Updates");
	SetStateVar("fox_update_available", "available.txt", "0");
	SetStateVar("fox_update_downloaded", "downloaded.txt", "0");
	SetStateVar("fox_update_need_variant", "need_variant.txt", "0");
	SetStateVar("fox_update_variant_count", "variant_count.txt", "0");
	SetStateVar("fox_update_variant_index", "variant_index.txt", "0");
	SetStateVar("fox_update_variant_text", "variant_text.txt", "None");

	SetStateVar("fox_update_latest_version", "latest_version.txt", "Unknown");
	SetStateVar("fox_update_latest_type", "latest_type.txt", "Unknown");
	SetStateVar("fox_update_latest_build_id", "latest_build_id.txt", "");
	SetStateVar("fox_update_latest_release_id", "latest_release_id.txt", "");
	SetStateVar("fox_update_latest_filename", "latest_filename.txt", "Unknown");
	SetStateVar("fox_update_latest_md5", "latest_md5.txt", "Unknown");
	SetStateVar("fox_update_latest_size", "latest_size.txt", "Unknown");
	SetStateVar("fox_update_latest_variant", "latest_variant.txt", "Unknown");
	SetStateVar("fox_update_latest_url", "latest_url.txt", "");
	SetStateVar("fox_update_zip_path", "zip_path.txt", "");

	WriteFile(STATE_DIR + "/changelog.txt", "No changelog loaded\n");
	SetVar("of_file_to_read", STATE_DIR + "/changelog.txt");
}

static string ReleaseOneLine(const FoxRelease& r) {
	string text;

	if (!r.variant.empty())
		text += r.variant;
	else
		text += "default";

	if (!r.type.empty())
		text += " / " + r.type;

	if (!r.version.empty())
		text += " / " + r.version;

	return text;
}

static void WriteChangelogForRelease(const FoxRelease& r) {
	std::ofstream ofs((STATE_DIR + "/changelog.txt").c_str(), std::ios::out | std::ios::trunc);

	if (!ofs) {
		LOGERR("FoxUpdater: failed to write changelog\n");
		return;
	}

	bool wrote = false;

	for (const string& raw : r.changelog) {
		string line = Trim(raw);
		if (line.empty())
			continue;

		if (!line.empty() && line[0] == '-')
			ofs << line << "\n";
		else
			ofs << "- " << line << "\n";

		wrote = true;
	}

	if (!r.notes.empty()) {
		if (wrote)
			ofs << "\n";

		ofs << "Notes:\n";

		for (const string& raw : r.notes) {
			string line = Trim(raw);
			if (!line.empty())
				ofs << "- " << line << "\n";
		}

		wrote = true;
	}

	if (!r.bugs.empty()) {
		if (wrote)
			ofs << "\n";

		ofs << "Known bugs:\n";

		for (const string& raw : r.bugs) {
			string line = Trim(raw);
			if (!line.empty())
				ofs << "- " << line << "\n";
		}

		wrote = true;
	}

	if (!wrote)
		ofs << "No changelog provided\n";

	SetVar("of_file_to_read", STATE_DIR + "/changelog.txt");
}

static void SaveCurrentReleaseVars(const FoxRelease& r) {
	SetStateVar("fox_update_current_build_id", "current_build_id.txt", r.build_id);
	SetStateVar("fox_update_current_release_id", "current_release_id.txt", r.id);
	SetStateVar("fox_update_current_type", "current_type.txt", r.type.empty() ? "Unknown" : r.type);
	SetStateVar("fox_update_current_version", "current_version.txt", r.version.empty() ? "Unknown" : r.version);
	SetStateVar("fox_update_device_id", "device_id.txt", r.device_id);
}

static void SaveSelectedReleaseVars(const FoxRelease& r) {
	SetStateVar("fox_update_latest_version", "latest_version.txt", r.version.empty() ? "Unknown" : r.version);
	SetStateVar("fox_update_latest_type", "latest_type.txt", r.type.empty() ? "Unknown" : r.type);
	SetStateVar("fox_update_latest_build_id", "latest_build_id.txt", r.build_id);
	SetStateVar("fox_update_latest_release_id", "latest_release_id.txt", r.id);
	SetStateVar("fox_update_latest_filename", "latest_filename.txt", r.filename.empty() ? "Unknown" : r.filename);
	SetStateVar("fox_update_latest_md5", "latest_md5.txt", r.md5.empty() ? "Unknown" : r.md5);
	SetStateVar("fox_update_latest_size", "latest_size.txt", r.size.empty() ? "Unknown" : r.size);
	SetStateVar("fox_update_latest_variant", "latest_variant.txt", r.variant.empty() ? "default" : r.variant);
	SetStateVar("fox_update_latest_url", "latest_url.txt", r.download_url);

	WriteChangelogForRelease(r);
}

static void SaveVariantPreview() {
	int count = static_cast<int>(g_variants.size());

	if (count <= 0) {
		g_variant_index = 0;
		SetStateVar("fox_update_variant_count", "variant_count.txt", "0");
		SetStateVar("fox_update_variant_index", "variant_index.txt", "0");
		SetStateVar("fox_update_variant_text", "variant_text.txt", "None");
		return;
	}

	if (g_variant_index < 0)
		g_variant_index = count - 1;

	if (g_variant_index >= count)
		g_variant_index = 0;

	const FoxRelease& r = g_variants[g_variant_index];

	SetStateVar("fox_update_variant_count", "variant_count.txt", std::to_string(count));
	SetStateVar("fox_update_variant_index", "variant_index.txt", std::to_string(g_variant_index + 1));
	SetStateVar("fox_update_variant_text", "variant_text.txt", ReleaseOneLine(r));

	SetStateVar("fox_update_variant_filename", "variant_filename.txt", r.filename.empty() ? "Unknown" : r.filename);
	SetStateVar("fox_update_variant_md5", "variant_md5.txt", r.md5.empty() ? "Unknown" : r.md5);
	SetStateVar("fox_update_variant_build_id", "variant_build_id.txt", r.build_id);
	SetStateVar("fox_update_variant_release_id", "variant_release_id.txt", r.id);
}

static void RefreshVarsFromState() {
	EnsureStateDir();

	SetVar("fox_update_status", StateRead("status.txt", "Press Check for Updates"));
	SetVar("fox_update_available", StateRead("available.txt", "0"));
	SetVar("fox_update_downloaded", StateRead("downloaded.txt", "0"));
	SetVar("fox_update_need_variant", StateRead("need_variant.txt", "0"));

	string allow_beta = GetVar("fox_update_allow_beta");
	if (allow_beta.empty())
		allow_beta = StateRead("allow_beta.txt", "0");
	SetVar("fox_update_allow_beta", allow_beta);

	SetVar("fox_update_current_build_id", StateRead("current_build_id.txt", ""));
	SetVar("fox_update_current_release_id", StateRead("current_release_id.txt", ""));
	SetVar("fox_update_current_type", StateRead("current_type.txt", "Unknown"));
	SetVar("fox_update_current_version", StateRead("current_version.txt", "Unknown"));

	SetVar("fox_update_device_id", StateRead("device_id.txt", ""));
	SetVar("fox_update_device_name", StateRead("device_name.txt", "Unknown"));
	SetVar("fox_update_device_codename", StateRead("device_codename.txt", "Unknown"));

	SetVar("fox_update_latest_version", StateRead("latest_version.txt", "Unknown"));
	SetVar("fox_update_latest_type", StateRead("latest_type.txt", "Unknown"));
	SetVar("fox_update_latest_build_id", StateRead("latest_build_id.txt", ""));
	SetVar("fox_update_latest_release_id", StateRead("latest_release_id.txt", ""));
	SetVar("fox_update_latest_filename", StateRead("latest_filename.txt", "Unknown"));
	SetVar("fox_update_latest_md5", StateRead("latest_md5.txt", "Unknown"));
	SetVar("fox_update_latest_size", StateRead("latest_size.txt", "Unknown"));
	SetVar("fox_update_latest_variant", StateRead("latest_variant.txt", "Unknown"));
	SetVar("fox_update_latest_url", StateRead("latest_url.txt", ""));
	SetVar("fox_update_zip_path", StateRead("zip_path.txt", ""));

	SetVar("fox_update_variant_count", StateRead("variant_count.txt", "0"));
	SetVar("fox_update_variant_index", StateRead("variant_index.txt", "0"));
	SetVar("fox_update_variant_text", StateRead("variant_text.txt", "None"));
	SetVar("fox_update_variant_filename", StateRead("variant_filename.txt", "Unknown"));
	SetVar("fox_update_variant_md5", StateRead("variant_md5.txt", "Unknown"));

	SetVar("of_file_to_read", STATE_DIR + "/changelog.txt");
}

static bool LoadSelectedFromState(FoxRelease& r) {
	r.id = StateRead("latest_release_id.txt", "");
	r.build_id = StateRead("latest_build_id.txt", "");
	r.version = StateRead("latest_version.txt", "");
	r.type = StateRead("latest_type.txt", "");
	r.filename = StateRead("latest_filename.txt", "");
	r.md5 = StateRead("latest_md5.txt", "");
	r.size = StateRead("latest_size.txt", "");
	r.variant = StateRead("latest_variant.txt", "");
	r.download_url = StateRead("latest_url.txt", "");

	return !r.download_url.empty() && !r.filename.empty() && r.filename != "Unknown";
}

static bool LoadCurrentFromState(FoxRelease& r) {
	r.id = StateRead("current_release_id.txt", "");
	r.build_id = StateRead("current_build_id.txt", "");
	r.version = StateRead("current_version.txt", "");
	r.type = StateRead("current_type.txt", "");
	r.device_id = StateRead("device_id.txt", "");

	return !r.id.empty() && !r.build_id.empty() && !r.device_id.empty();
}

static bool IsSameRelease(const FoxRelease& a, const FoxRelease& b);

static string AvailableReleaseLine(const FoxRelease& r) {
	string line = ReleaseOneLine(r);

	if (!r.size.empty())
		line += " / " + r.size;

	return line;
}

static vector<FoxRelease> LoadAvailableReleasesFromState() {
	vector<FoxRelease> releases;
	Json::Value root;
	string releases_json_path = STATE_DIR + "/available_releases.json";

	if (!ParseJsonFile(releases_json_path, root))
		return releases;

	ReleaseContext ctx;
	CollectReleasesRecursive(root, releases, ctx);
	releases = DedupReleases(releases);
	SortReleasesByDateDesc(releases);

	vector<FoxRelease> filtered;

	for (const FoxRelease& r : releases) {
		if (r.download_url.empty() || r.filename.empty())
			continue;

		if (!g_current_release.device_id.empty() && !r.device_id.empty() && r.device_id != g_current_release.device_id)
			continue;

		if (!g_current_release.id.empty() && IsSameRelease(r, g_current_release))
			continue;

		filtered.push_back(r);
	}

	return filtered;
}

static bool Md5Matches(const string& path, const string& expected) {
	if (expected.empty() || expected == "Unknown")
		return true;

	string cmd = "md5sum " + ShellQuote(path) + " | awk '{print $1}'";
	string output;

	int ret = RunCommand(cmd, &output);
	if (ret != 0)
		return false;

	string actual = Trim(output);
	string exp = Trim(expected);

	std::transform(actual.begin(), actual.end(), actual.begin(), ::tolower);
	std::transform(exp.begin(), exp.end(), exp.begin(), ::tolower);

	gui_print("Expected MD5: %s\n", exp.c_str());
	gui_print("Actual MD5:   %s\n", actual.c_str());

	return actual == exp;
}

static string SafeFilename(string filename) {
	filename = Trim(filename);

	if (filename.empty() || filename == "Unknown")
		filename = "OrangeFox-update.zip";

	for (char& c : filename) {
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	}

	if (filename.find(".zip") == string::npos)
		filename += ".zip";

	return filename;
}

static string PickDownloadDir() {
	PartitionManager.Mount_By_Path("/data", false);
	PartitionManager.Mount_By_Path("/sdcard", false);

	MkDirs(DOWNLOAD_DIR);

	if (FileExists(DOWNLOAD_DIR) && access(DOWNLOAD_DIR.c_str(), W_OK) == 0)
		return DOWNLOAD_DIR;

	MkDirs(TMP_DOWNLOAD_DIR);
	return TMP_DOWNLOAD_DIR;
}

static void SaveDeviceVarsFromJson(const Json::Value& root, const string& fallback_id) {
	vector<const Json::Value*> objects;

	std::function<void(const Json::Value&)> collect = [&](const Json::Value& v) {
		if (v.isObject()) {
			objects.push_back(&v);
			for (const string& key : v.getMemberNames())
				collect(v[key]);
		} else if (v.isArray()) {
			for (Json::ArrayIndex i = 0; i < v.size(); i++)
				collect(v[i]);
		}
	};

	collect(root);

	const Json::Value* device = nullptr;

	for (const Json::Value* obj : objects) {
		string id = ObjStringAny(*obj, {"id", "_id"}, "");
		if (!fallback_id.empty() && id == fallback_id) {
			device = obj;
			break;
		}
	}

	if (!device) {
		for (const Json::Value* obj : objects) {
			if (ObjHas(*obj, "codenames") || ObjHas(*obj, "model_names") || ObjHas(*obj, "codename") || ObjHas(*obj, "model_name")) {
				device = obj;
				break;
			}
		}
	}

	if (!device) {
		SetStateVar("fox_update_device_name", "device_name.txt", "Unknown");
		SetStateVar("fox_update_device_codename", "device_codename.txt", "Unknown");
		return;
	}

	string name = ObjStringAny(*device, {"full_name", "name", "model_name"}, "");

	if (name.empty()) {
		string oem = ObjStringAny(*device, {"oem_name"}, "");
		string model = ObjStringAny(*device, {"model_name"}, "");
		if (!oem.empty() && !model.empty())
			name = oem + " " + model;
	}

	if (name.empty()) {
		vector<string> names = ObjStringArray(*device, "model_names");
		if (!names.empty())
			name = names[0];
	}

	string codename = ObjStringAny(*device, {"codename"}, "");

	if (codename.empty()) {
		vector<string> codenames = ObjStringArray(*device, "codenames");
		if (!codenames.empty())
			codename = codenames[0];
	}

	SetStateVar("fox_update_device_name", "device_name.txt", name.empty() ? "Unknown" : name);
	SetStateVar("fox_update_device_codename", "device_codename.txt", codename.empty() ? "Unknown" : codename);
}

static bool IsReleaseAllowedByBetaSetting(const FoxRelease& r, bool allow_beta) {
	string type = r.type;
	std::transform(type.begin(), type.end(), type.begin(), ::tolower);

	if (type == "beta")
		return allow_beta;

	if (type == "stable")
		return true;

	return true;
}

static bool IsSameRelease(const FoxRelease& a, const FoxRelease& b) {
	if (!a.id.empty() && !b.id.empty() && a.id == b.id)
		return true;

	if (!a.build_id.empty() && !b.build_id.empty() && a.build_id == b.build_id)
		return true;

	return false;
}

static void SelectRelease(const FoxRelease& r) {
	g_selected_release = r;

	SaveSelectedReleaseVars(r);

	SetStateVar("fox_update_available", "available.txt", "1");
	SetStateVar("fox_update_downloaded", "downloaded.txt", "0");
	SetStateVar("fox_update_need_variant", "need_variant.txt", "0");

	Status("Update selected: " + ReleaseOneLine(r));
}

} // namespace

namespace FoxUpdater {

int Refresh() {
	RefreshVarsFromState();
	return 0;
}

int Init() {
	EnsureStateDir();

	string build_id = DetectCurrentBuildId();

	if (build_id.empty()) {
		Status("Current fox_id/build_id is missing");
		RefreshVarsFromState();
		return 1;
	}

	SetStateVar("fox_update_current_build_id", "current_build_id.txt", build_id);

	string current_json_path = STATE_DIR + "/current_release.json";

	Status("Getting current release by fox_id");

	if (!ApiGet("/releases/get?build_id=" + UrlEncode(build_id), current_json_path)) {
		Status("Failed to get current release by build_id");
		RefreshVarsFromState();
		return 1;
	}

	Json::Value current_root;

	if (!ParseJsonFile(current_json_path, current_root)) {
		Status("Failed to parse current release response");
		RefreshVarsFromState();
		return 1;
	}

	vector<FoxRelease> releases;
	ReleaseContext ctx;
	CollectReleasesRecursive(current_root, releases, ctx);
	releases = DedupReleases(releases);

	if (releases.empty()) {
		Status("Current release was not found in API response");
		RefreshVarsFromState();
		return 1;
	}

	g_current_release = releases[0];

	if (g_current_release.build_id.empty())
		g_current_release.build_id = build_id;

	SaveCurrentReleaseVars(g_current_release);

	string allow_beta = GetVar("fox_update_allow_beta");

	string current_type = g_current_release.type;
	std::transform(current_type.begin(), current_type.end(), current_type.begin(), ::tolower);

	if (current_type == "beta") {
		SetStateVar("fox_update_allow_beta", "allow_beta.txt", "1");
	} else if (allow_beta.empty()) {
		SetStateVar("fox_update_allow_beta", "allow_beta.txt", "0");
	}

	if (!g_current_release.device_id.empty()) {
		string device_json_path = STATE_DIR + "/device.json";

		if (ApiGet("/devices/get?id=" + UrlEncode(g_current_release.device_id), device_json_path)) {
			Json::Value device_root;
			if (ParseJsonFile(device_json_path, device_root))
				SaveDeviceVarsFromJson(device_root, g_current_release.device_id);
		}
	}

	Status("Current release loaded");
	RefreshVarsFromState();
	return 0;
}

int Check() {
	ResetRuntimeState();

	string build_id = DetectCurrentBuildId();
	bool have_current =
		!g_current_release.id.empty() &&
		!g_current_release.device_id.empty() &&
		(build_id.empty() || g_current_release.build_id == build_id);

	if (!have_current) {
		FoxRelease cached;
		if (LoadCurrentFromState(cached) && (build_id.empty() || cached.build_id == build_id)) {
			g_current_release = cached;
			have_current = true;
			gui_print("FoxUpdater: using cached current release\n");
		}
	}

	if (!have_current) {
		int init_ret = Init();
		if (init_ret != 0) {
			RefreshVarsFromState();
			return init_ret;
		}
	}

	if (g_current_release.id.empty()) {
		Status("Current internal release ID is missing");
		RefreshVarsFromState();
		return 1;
	}

	if (g_current_release.device_id.empty()) {
		Status("Current device ID is missing");
		RefreshVarsFromState();
		return 1;
	}

	string allow_beta = GetVar("fox_update_allow_beta");
	bool allow_beta_bool = allow_beta == "1";

	string type_query;
	if (allow_beta_bool)
		type_query = "&type=stable&type=beta";
	else
		type_query = "&type=stable";

	string query =
		"?device_id=" + UrlEncode(g_current_release.device_id) +
		type_query +
		"&sort=date_desc"
		"&after_release_id=" + UrlEncode(g_current_release.id) +
		"&limit=5"
		"&group=true";

	string releases_json_path = STATE_DIR + "/available_releases.json";

	Status("Checking for newer releases");

	if (!ApiGet("/releases/" + query, releases_json_path)) {
		Status("Failed to query newer releases");
		RefreshVarsFromState();
		return 1;
	}

	Json::Value root;

	if (!ParseJsonFile(releases_json_path, root)) {
		Status("Failed to parse newer releases response");
		RefreshVarsFromState();
		return 1;
	}

	vector<FoxRelease> found;
	ReleaseContext ctx;
	CollectReleasesRecursive(root, found, ctx);
	found = DedupReleases(found);

	vector<FoxRelease> filtered;

	for (const FoxRelease& r : found) {
		if (IsSameRelease(r, g_current_release))
			continue;

		if (!r.device_id.empty() && r.device_id != g_current_release.device_id)
			continue;

		if (!IsReleaseAllowedByBetaSetting(r, allow_beta_bool))
			continue;

		if (r.download_url.empty())
			continue;

		if (r.filename.empty())
			continue;

		filtered.push_back(r);
	}

	SortReleasesByDateDesc(filtered);

	if (filtered.empty()) {
		SetStateVar("fox_update_available", "available.txt", "0");
		SetStateVar("fox_update_need_variant", "need_variant.txt", "0");
		Status("No newer release found");
		RefreshVarsFromState();
		return 0;
	}

	string newest_version = filtered[0].version;
	g_variants.clear();

	for (const FoxRelease& r : filtered) {
		if (!newest_version.empty()) {
			if (r.version == newest_version)
				g_variants.push_back(r);
		} else {
			g_variants.push_back(r);
			break;
		}
	}

	if (g_variants.empty())
		g_variants.push_back(filtered[0]);

	g_variant_index = 0;
	SaveVariantPreview();

	if (g_variants.size() == 1) {
		SelectRelease(g_variants[0]);
		RefreshVarsFromState();
		return 0;
	}

	SetStateVar("fox_update_available", "available.txt", "0");
	SetStateVar("fox_update_downloaded", "downloaded.txt", "0");
	SetStateVar("fox_update_need_variant", "need_variant.txt", "1");

	Status("Multiple update variants found");
	RefreshVarsFromState();
	return 0;
}

int VariantPrev() {
	if (g_variants.empty()) {
		Status("No variants loaded");
		RefreshVarsFromState();
		return 1;
	}

	g_variant_index--;
	SaveVariantPreview();
	Status("Selected variant: " + GetVar("fox_update_variant_text"));
	RefreshVarsFromState();
	return 0;
}

int VariantNext() {
	if (g_variants.empty()) {
		Status("No variants loaded");
		RefreshVarsFromState();
		return 1;
	}

	g_variant_index++;
	SaveVariantPreview();
	Status("Selected variant: " + GetVar("fox_update_variant_text"));
	RefreshVarsFromState();
	return 0;
}

int SelectVariant() {
	if (g_variants.empty()) {
		Status("No variants loaded");
		RefreshVarsFromState();
		return 1;
	}

	if (g_variant_index < 0 || g_variant_index >= static_cast<int>(g_variants.size()))
		g_variant_index = 0;

	SelectRelease(g_variants[g_variant_index]);
	RefreshVarsFromState();
	return 0;
}

int PrepareAvailableReleases() {
	EnsureStateDir();

	vector<FoxRelease> releases = LoadAvailableReleasesFromState();
	std::ofstream ofs((STATE_DIR + "/available_list.txt").c_str(), std::ios::out | std::ios::trunc);

	if (!ofs) {
		Status("Failed to prepare release list");
		RefreshVarsFromState();
		return 1;
	}

	if (releases.empty()) {
		ofs << "No newer releases found\n";
		SetStateVar("fox_update_available_count", "available_count.txt", "0");
	} else {
		for (const FoxRelease& r : releases)
			ofs << AvailableReleaseLine(r) << "\n";

		SetStateVar("fox_update_available_count", "available_count.txt", std::to_string(releases.size()));
	}

	RefreshVarsFromState();
	return 0;
}

int SelectAvailableRelease() {
	string selected = Trim(GetVar("fox_update_release_selection"));

	if (selected.empty() || selected == "No newer releases found" || selected == "Unable to open file!") {
		Status("No release selected");
		RefreshVarsFromState();
		return 1;
	}

	vector<FoxRelease> releases = LoadAvailableReleasesFromState();

	for (const FoxRelease& r : releases) {
		if (selected == AvailableReleaseLine(r) || selected == ReleaseOneLine(r)) {
			SelectRelease(r);
			RefreshVarsFromState();
			return 0;
		}
	}

	Status("Selected release not found");
	RefreshVarsFromState();
	return 1;
}

int Download() {
	EnsureStateDir();

	if (g_selected_release.download_url.empty()) {
		FoxRelease from_state;
		if (LoadSelectedFromState(from_state))
			g_selected_release = from_state;
	}

	if (GetVar("fox_update_need_variant") == "1") {
		Status("Select an update variant first");
		RefreshVarsFromState();
		return 1;
	}

	if (g_selected_release.download_url.empty()) {
		Status("No selected release to download");
		RefreshVarsFromState();
		return 1;
	}

	string out_dir = PickDownloadDir();
	string filename = SafeFilename(g_selected_release.filename);
	string out_path = out_dir + "/" + filename;
	string tmp_path = out_path + ".download";

	unlink(tmp_path.c_str());

	Status("Downloading " + filename);

	gui_print("URL: %s\n", g_selected_release.download_url.c_str());
	gui_print("Output: %s\n", out_path.c_str());

	if (!HttpGetToFile(g_selected_release.download_url, tmp_path)) {
		unlink(tmp_path.c_str());
		Status("Download failed");
		RefreshVarsFromState();
		return 1;
	}

	struct stat st;
	if (stat(tmp_path.c_str(), &st) != 0 || st.st_size <= 0) {
		unlink(tmp_path.c_str());
		Status("Downloaded file is empty");
		RefreshVarsFromState();
		return 1;
	}

	if (!Md5Matches(tmp_path, g_selected_release.md5)) {
		unlink(tmp_path.c_str());
		Status("MD5 verification failed");
		RefreshVarsFromState();
		return 1;
	}

	if (rename(tmp_path.c_str(), out_path.c_str()) != 0) {
		unlink(tmp_path.c_str());
		Status("Failed to save downloaded ZIP");
		RefreshVarsFromState();
		return 1;
	}

	SetStateVar("fox_update_zip_path", "zip_path.txt", out_path);
	SetStateVar("fox_update_downloaded", "downloaded.txt", "1");

	Status("Downloaded: " + out_path);
	RefreshVarsFromState();
	return 0;
}

int Install() {
	EnsureStateDir();

	string zip_path = StateRead("zip_path.txt", "");

	if (zip_path.empty() || !FileExists(zip_path)) {
		Status("Downloaded ZIP not found");
		RefreshVarsFromState();
		return 1;
	}

	size_t slashpos = zip_path.find_last_of('/');
	string zip_dir = (slashpos == string::npos) ? "/" : zip_path.substr(0, slashpos);
	string zip_name = (slashpos == string::npos) ? zip_path : zip_path.substr(slashpos + 1);

	Status("Opening install confirmation");

	DataManager::SetValue("tw_file", zip_name);
	DataManager::SetValue("tw_filename", zip_path);
	DataManager::SetValue("install_back", "fox_updates");
	DataManager::SetValue("tw_zip_location", zip_dir);
	DataManager::SetValue("fox_install_built_in_zip", "0");

	RefreshVarsFromState();
	return 0;
}

} // namespace FoxUpdater
