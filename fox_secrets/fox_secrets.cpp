/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_secrets.hpp"

#include <cstdio>
#include <fcntl.h>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <cutils/properties.h>

#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <json/json.h>

#ifndef LOGINFO
#define LOGINFO(...) printf(__VA_ARGS__)
#endif
#ifndef LOGERR
#define LOGERR(...) fprintf(stderr, __VA_ARGS__)
#endif

namespace {

const char* kFoxDir       = "/data/media/0/Fox";
const char* kSecretsFile  = "/data/media/0/Fox/secrets.json";
// Legacy per-line WLAN store, imported once into secrets.json.
const char* kLegacyWlan   = "/data/media/0/Fox/wlan/wlan_saved.enc";

std::mutex g_mutex;

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

std::string HexEncode(const unsigned char* data, size_t len)
{
	static const char* hex = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; ++i) {
		unsigned char c = data[i];
		out.push_back(hex[(c >> 4) & 0x0F]);
		out.push_back(hex[c & 0x0F]);
	}
	return out;
}

std::string HexEncodeString(const std::string& in)
{
	return HexEncode(reinterpret_cast<const unsigned char*>(in.data()), in.size());
}

bool HexDecode(const std::string& hex, std::vector<unsigned char>& out)
{
	if (hex.size() % 2 != 0)
		return false;

	out.clear();
	out.reserve(hex.size() / 2);

	for (size_t i = 0; i < hex.size(); i += 2) {
		char h = hex[i];
		char l = hex[i + 1];
		int hi = -1, lo = -1;

		if (h >= '0' && h <= '9') hi = h - '0';
		else if (h >= 'a' && h <= 'f') hi = h - 'a' + 10;
		else if (h >= 'A' && h <= 'F') hi = h - 'A' + 10;

		if (l >= '0' && l <= '9') lo = l - '0';
		else if (l >= 'a' && l <= 'f') lo = l - 'a' + 10;
		else if (l >= 'A' && l <= 'F') lo = l - 'A' + 10;

		if (hi < 0 || lo < 0)
			return false;

		out.push_back(static_cast<unsigned char>((hi << 4) | lo));
	}
	return true;
}

bool HexDecodeString(const std::string& hex, std::string& out)
{
	std::vector<unsigned char> decoded;
	if (!HexDecode(hex, decoded))
		return false;
	out.assign(reinterpret_cast<const char*>(decoded.data()), decoded.size());
	return true;
}

// ---------------------------------------------------------------------------
// Crypto
//
// The key material below MUST stay byte-for-byte identical to the legacy WLAN
// store (formerly OF_GetWlanCryptoKey in wlan.cpp) so that networks saved by
// older builds can still be decrypted during the one-time import.
// ---------------------------------------------------------------------------

bool GetCryptoKey(unsigned char key[32])
{
	char serial[PROPERTY_VALUE_MAX]  = {0};
	char device[PROPERTY_VALUE_MAX]  = {0};
	char product[PROPERTY_VALUE_MAX] = {0};

	property_get("ro.serialno", serial, "");
	property_get("ro.product.device", device, "");
	property_get("ro.product.name", product, "");

	std::string material;
	material += "OrangeFox-WLAN-Saved-v1|";
	material += serial;
	material += "|";
	material += device;
	material += "|";
	material += product;
	material += "|INFINITI-WLAN";

	SHA256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), key);
	return true;
}

bool Encrypt(const std::string& plain, std::string& iv_hex, std::string& cipher_hex, std::string& tag_hex)
{
	unsigned char iv[12];
	if (RAND_bytes(iv, sizeof(iv)) != 1)
		return false;

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	// Derive only after the fallible setup above, so no key copy survives an
	// early return; it is wiped at 'done:' for the same reason.
	unsigned char key[32];
	GetCryptoKey(key);

	bool ok = false;
	std::vector<unsigned char> cipher;
	cipher.resize(plain.size() + 16);

	int len = 0;
	int cipher_len = 0;
	unsigned char tag[16];

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto done;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1)
		goto done;
	if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1)
		goto done;

	if (!plain.empty()) {
		if (EVP_EncryptUpdate(ctx, cipher.data(), &len,
		                      reinterpret_cast<const unsigned char*>(plain.data()),
		                      plain.size()) != 1) {
			goto done;
		}
		cipher_len = len;
	}

	if (EVP_EncryptFinal_ex(ctx, cipher.data() + cipher_len, &len) != 1)
		goto done;
	cipher_len += len;

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1)
		goto done;

	cipher.resize(cipher_len);

	iv_hex = HexEncode(iv, sizeof(iv));
	cipher_hex = HexEncode(cipher.data(), cipher.size());
	tag_hex = HexEncode(tag, sizeof(tag));
	ok = true;

done:
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	return ok;
}

bool Decrypt(const std::string& iv_hex, const std::string& cipher_hex, const std::string& tag_hex, std::string& plain)
{
	std::vector<unsigned char> iv, cipher, tag;
	if (!HexDecode(iv_hex, iv) || !HexDecode(cipher_hex, cipher) || !HexDecode(tag_hex, tag))
		return false;
	if (iv.size() != 12 || tag.size() != 16)
		return false;

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return false;

	// Derive only after the fallible setup above, so no key copy survives an
	// early return; it is wiped at 'done:' for the same reason.
	unsigned char key[32];
	GetCryptoKey(key);

	bool ok = false;
	std::vector<unsigned char> out;
	out.resize(cipher.size() + 16);

	int len = 0;
	int plain_len = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		goto done;
	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), NULL) != 1)
		goto done;
	if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv.data()) != 1)
		goto done;

	if (!cipher.empty()) {
		if (EVP_DecryptUpdate(ctx, out.data(), &len, cipher.data(), cipher.size()) != 1)
			goto done;
		plain_len = len;
	}

	if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1)
		goto done;
	if (EVP_DecryptFinal_ex(ctx, out.data() + plain_len, &len) != 1)
		goto done;
	plain_len += len;
	out.resize(plain_len);

	plain.assign(reinterpret_cast<const char*>(out.data()), out.size());
	ok = true;

done:
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	return ok;
}

// ---------------------------------------------------------------------------
// JSON document load/save (callers must hold g_mutex)
// ---------------------------------------------------------------------------

void ImportLegacyWlan(Json::Value& root)
{
	std::ifstream ifs(kLegacyWlan);
	if (!ifs.is_open())
		return;

	bool imported = false;
	std::string line;
	while (std::getline(ifs, line)) {
		// Trim trailing CR/whitespace.
		while (!line.empty() &&
		       (line.back() == '\r' || line.back() == '\n' ||
		        line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty())
			continue;

		// Legacy format: ssid_hex|enc_hex|iv_hex|cipher_hex|tag_hex
		std::vector<std::string> parts;
		std::stringstream ss(line);
		std::string part;
		while (std::getline(ss, part, '|'))
			parts.push_back(part);
		if (parts.size() < 5)
			continue;

		std::string ssid, enc;
		if (!HexDecodeString(parts[0], ssid) || ssid.empty())
			continue;
		if (!HexDecodeString(parts[1], enc) || enc.empty())
			enc = "WPA2";

		// The legacy ciphertext was produced with the same key, so we keep the
		// iv/cipher/tag verbatim rather than decrypting and re-encrypting.
		Json::Value net;
		net["enc"]    = enc;
		net["iv"]     = parts[2];
		net["cipher"] = parts[3];
		net["tag"]    = parts[4];
		root["wlan"][ssid] = net;
		imported = true;
	}
	ifs.close();

	if (imported)
		LOGINFO("Fox_Secrets: imported legacy WLAN networks from %s\n", kLegacyWlan);
}

Json::Value Load()
{
	Json::Value root(Json::objectValue);

	std::ifstream ifs(kSecretsFile);
	if (ifs.is_open()) {
		Json::CharReaderBuilder builder;
		std::string errs;
		if (!Json::parseFromStream(builder, ifs, &root, &errs)) {
			LOGERR("Fox_Secrets: failed to parse %s: %s\n", kSecretsFile, errs.c_str());
			root = Json::Value(Json::objectValue);
		}
		ifs.close();
	}

	if (!root.isObject())
		root = Json::Value(Json::objectValue);

	// One-time migration of the legacy WLAN store.
	if (!root.isMember("wlan") && access(kLegacyWlan, F_OK) == 0) {
		ImportLegacyWlan(root);
		root["version"] = 1;
	}

	if (!root.isMember("version"))
		root["version"] = 1;

	return root;
}

bool Save(Json::Value& root)
{
	mkdir(kFoxDir, 0700);

	std::string tmp = std::string(kSecretsFile) + ".tmp";

	Json::StreamWriterBuilder writer;
	writer["indentation"] = "  ";
	std::string text = Json::writeString(writer, root);

	{
		// Create the temp file 0600 from the outset (O_EXCL: never follow a
		// pre-existing file/symlink) so the ciphertext is never briefly readable
		// in the window between writing it and a post-hoc chmod.
		unlink(tmp.c_str());
		int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			LOGERR("Fox_Secrets: cannot open %s for write\n", tmp.c_str());
			return false;
		}
		FILE* f = fdopen(fd, "w");
		if (!f) {
			close(fd);
			unlink(tmp.c_str());
			return false;
		}
		size_t written = fwrite(text.data(), 1, text.size(), f);
		if (written != text.size() || fflush(f) != 0) {
			fclose(f);
			unlink(tmp.c_str());
			return false;
		}
		fclose(f);
	}

	if (rename(tmp.c_str(), kSecretsFile) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	chmod(kSecretsFile, 0600);

	// Retire the legacy file after a successful migration write.
	if (access(kLegacyWlan, F_OK) == 0)
		rename(kLegacyWlan, (std::string(kLegacyWlan) + ".imported").c_str());

	return true;
}

bool StoreEncrypted(Json::Value& slot, const std::string& plain)
{
	std::string iv, cipher, tag;
	if (!Encrypt(plain, iv, cipher, tag))
		return false;
	slot = Json::Value(Json::objectValue);
	slot["iv"]     = iv;
	slot["cipher"] = cipher;
	slot["tag"]    = tag;
	return true;
}

bool LoadEncrypted(const Json::Value& slot, std::string& plain)
{
	if (!slot.isObject())
		return false;
	if (!slot.isMember("iv") || !slot.isMember("cipher") || !slot.isMember("tag"))
		return false;
	return Decrypt(slot["iv"].asString(), slot["cipher"].asString(),
	               slot["tag"].asString(), plain);
}

} // namespace

// ---------------------------------------------------------------------------
// Remote password
// ---------------------------------------------------------------------------

bool Fox_Secrets::SetRemotePassword(const std::string& password)
{
	if (password.empty())
		return ClearRemotePassword();

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!StoreEncrypted(root["remote"]["password"], password))
		return false;
	return Save(root);
}

bool Fox_Secrets::ClearRemotePassword()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (root.isMember("remote"))
		root["remote"].removeMember("password");
	return Save(root);
}

bool Fox_Secrets::GetRemotePassword(std::string& out)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("remote") || !root["remote"].isMember("password"))
		return false;
	if (!LoadEncrypted(root["remote"]["password"], out))
		return false;
	return !out.empty();
}

bool Fox_Secrets::HasRemotePassword()
{
	std::string tmp;
	return GetRemotePassword(tmp);
}

// ---------------------------------------------------------------------------
// WLAN networks
// ---------------------------------------------------------------------------

bool Fox_Secrets::SetWlanNetwork(const std::string& ssid,
                                 const std::string& password,
                                 const std::string& encryption)
{
	if (ssid.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	Json::Value net(Json::objectValue);
	net["enc"] = encryption.empty() ? "WPA2" : encryption;

	std::string iv, cipher, tag;
	if (!Encrypt(password, iv, cipher, tag))
		return false;
	net["iv"]     = iv;
	net["cipher"] = cipher;
	net["tag"]    = tag;

	root["wlan"][ssid] = net;
	return Save(root);
}

bool Fox_Secrets::GetWlanNetwork(const std::string& ssid,
                                 std::string& password,
                                 std::string& encryption)
{
	password.clear();
	encryption.clear();
	if (ssid.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("wlan") || !root["wlan"].isMember(ssid))
		return false;

	const Json::Value& net = root["wlan"][ssid];
	encryption = net.isMember("enc") ? net["enc"].asString() : "WPA2";
	return LoadEncrypted(net, password);
}

bool Fox_Secrets::DeleteWlanNetwork(const std::string& ssid)
{
	if (ssid.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("wlan") || !root["wlan"].isMember(ssid))
		return true;
	root["wlan"].removeMember(ssid);
	return Save(root);
}

bool Fox_Secrets::ListWlanNetworks(std::vector<std::string>& ssids)
{
	ssids.clear();

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("wlan") || !root["wlan"].isObject())
		return false;

	const Json::Value& wlan = root["wlan"];
	for (const std::string& ssid : wlan.getMemberNames()) {
		if (!ssid.empty())
			ssids.push_back(ssid);
	}
	return !ssids.empty();
}

// ---------------------------------------------------------------------------
// SoftAP / hotspot configuration
// ---------------------------------------------------------------------------

bool Fox_Secrets::SetApConfig(const std::string& ssid, const std::string& password)
{
	if (ssid.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	Json::Value ap(Json::objectValue);
	ap["ssid"] = ssid;

	// The password is encrypted even when empty (an open hotspot) so the slot
	// shape stays uniform with the WLAN client entries.
	std::string iv, cipher, tag;
	if (!Encrypt(password, iv, cipher, tag))
		return false;
	ap["iv"]     = iv;
	ap["cipher"] = cipher;
	ap["tag"]    = tag;

	root["ap"] = ap;
	return Save(root);
}

bool Fox_Secrets::GetApConfig(std::string& ssid, std::string& password)
{
	ssid.clear();
	password.clear();

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("ap") || !root["ap"].isObject())
		return false;

	const Json::Value& ap = root["ap"];
	ssid = ap.isMember("ssid") ? ap["ssid"].asString() : "";
	// An empty password is valid (open hotspot); ignore the decrypt result.
	LoadEncrypted(ap, password);
	return !ssid.empty();
}

bool Fox_Secrets::HasApConfig()
{
	std::string ssid, password;
	return GetApConfig(ssid, password);
}

// ---------------------------------------------------------------------------
// Authorized network-ADB devices
// ---------------------------------------------------------------------------

namespace {

// An adb public key line is "<base64-key> <comment>"; the base64 token is the
// stable key material we fingerprint, the comment is a friendly name.
std::string AdbKeyToken(const std::string& key)
{
	size_t sp = key.find(' ');
	return sp == std::string::npos ? key : key.substr(0, sp);
}

std::string AdbKeyComment(const std::string& key)
{
	size_t sp = key.find(' ');
	if (sp == std::string::npos)
		return "";
	std::string comment = key.substr(sp + 1);
	while (!comment.empty() && (comment.back() == '\n' || comment.back() == '\r' ||
	                            comment.back() == ' ' || comment.back() == '\t'))
		comment.pop_back();
	return comment;
}

// Short, stable id for a key: first 16 hex chars of SHA-256(base64 token).
std::string AdbFingerprint(const std::string& key)
{
	std::string token = AdbKeyToken(key);
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest);
	return HexEncode(digest, sizeof(digest)).substr(0, 16);
}

} // namespace

bool Fox_Secrets::AddAdbDevice(const std::string& key, const std::string& name)
{
	if (key.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	std::string fp = AdbFingerprint(key);
	std::string friendly = name.empty() ? AdbKeyComment(key) : name;
	if (friendly.empty())
		friendly = "device-" + fp.substr(0, 6);

	Json::Value entry(Json::objectValue);
	entry["name"] = friendly;
	entry["key"] = key;
	entry["last_seen"] = static_cast<Json::Int64>(time(nullptr));
	root["adb_devices"][fp] = entry;
	return Save(root);
}

bool Fox_Secrets::ListAdbDevices(std::vector<AdbDevice>& out)
{
	out.clear();

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("adb_devices") || !root["adb_devices"].isObject())
		return false;

	const Json::Value& devices = root["adb_devices"];
	for (const std::string& fp : devices.getMemberNames()) {
		const Json::Value& entry = devices[fp];
		AdbDevice dev;
		dev.fingerprint = fp;
		dev.name = entry.isMember("name") ? entry["name"].asString() : "";
		dev.key = entry.isMember("key") ? entry["key"].asString() : "";
		dev.last_seen = entry.isMember("last_seen") ? entry["last_seen"].asInt64() : 0;
		out.push_back(dev);
	}
	return !out.empty();
}

bool Fox_Secrets::DeleteAdbDevice(const std::string& id, std::string& removed_key)
{
	removed_key.clear();
	if (id.empty())
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	Json::Value root = Load();
	if (!root.isMember("adb_devices") || !root["adb_devices"].isObject())
		return false;

	Json::Value& devices = root["adb_devices"];
	std::string match;
	// Exact fingerprint or friendly-name match wins outright.
	for (const std::string& fp : devices.getMemberNames()) {
		const Json::Value& entry = devices[fp];
		std::string name = entry.isMember("name") ? entry["name"].asString() : "";
		if (fp == id || name == id) {
			match = fp;
			break;
		}
	}
	// Otherwise allow a fingerprint prefix, but only if it is unambiguous — never
	// revoke a device because a short prefix happened to match several keys.
	if (match.empty() && id.size() >= 4) {
		int prefix_hits = 0;
		std::string prefix_match;
		for (const std::string& fp : devices.getMemberNames()) {
			if (fp.compare(0, id.size(), id) == 0) {
				prefix_match = fp;
				if (++prefix_hits > 1)
					break;
			}
		}
		if (prefix_hits == 1)
			match = prefix_match;
	}
	if (match.empty())
		return false;
	removed_key = devices[match].isMember("key") ? devices[match]["key"].asString() : "";

	devices.removeMember(match);
	return Save(root);
}
