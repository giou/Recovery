/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	Unified secret store for the OrangeFox network features.

	Secrets live in a single JSON file on the (FBE-encrypted) user data
	partition: /data/media/0/Fox/secrets.json (mode 0600). Every sensitive
	value is itself encrypted with AES-256-GCM using a device-bound key.

	NOTE ON THE THREAT MODEL:
	The encryption key is derived from device properties (see fox_secrets.cpp).
	The derivation logic ships inside recovery, so anyone who controls the
	booted recovery can reproduce the key. This protects against casual
	viewing and against pulling the raw file off the flash; it is NOT
	hardware-backed secrecy against an attacker running this recovery.
*/

#ifndef _FOX_SECRETS_HPP
#define _FOX_SECRETS_HPP

#include <string>
#include <vector>

class Fox_Secrets
{
public:
	// Shared remote-control password (used by both ttyd basic-auth and the
	// REST/remote HTTP API). An empty password clears it.
	static bool SetRemotePassword(const std::string& password);
	static bool ClearRemotePassword();
	// Returns the decrypted password. False if none configured or on failure.
	static bool GetRemotePassword(std::string& out);
	static bool HasRemotePassword();

	// Saved WLAN networks (replaces the legacy wlan_saved.enc store).
	static bool SetWlanNetwork(const std::string& ssid,
	                           const std::string& password,
	                           const std::string& encryption);
	static bool GetWlanNetwork(const std::string& ssid,
	                           std::string& password,
	                           std::string& encryption);
	static bool DeleteWlanNetwork(const std::string& ssid);
	// Lists the SSIDs of all saved networks.
	static bool ListWlanNetworks(std::vector<std::string>& ssids);

	// SoftAP / hotspot configuration (a single stored entry). The password may
	// be empty for an open hotspot; it is encrypted at rest like the WLAN
	// client credentials above.
	static bool SetApConfig(const std::string& ssid, const std::string& password);
	static bool GetApConfig(std::string& ssid, std::string& password);
	static bool HasApConfig();

	// Authorized network-ADB devices. The store keeps a small registry of the
	// hosts that have paired (their adb RSA public key, a friendly name parsed
	// from the key's trailing comment, and a last-seen unix timestamp). The
	// public key itself is not secret, so the registry is plaintext; it exists
	// so `web adb list/forget` can present and revoke devices by name.
	struct AdbDevice {
		std::string fingerprint;  // short SHA-256 of the key, used as the id
		std::string name;         // friendly name (key comment, e.g. user@host)
		std::string key;          // the stored adb public key line
		long long last_seen = 0;  // unix epoch seconds
	};
	// Record (or refresh the timestamp of) a paired device from its adb pubkey.
	static bool AddAdbDevice(const std::string& key, const std::string& name = "");
	static bool ListAdbDevices(std::vector<AdbDevice>& out);
	// Remove the device whose fingerprint (full or prefix) or name matches 'id'.
	// On success 'removed_key' carries the stored pubkey so the caller can also
	// strip it from the adb_keys files. Returns false when no device matched.
	static bool DeleteAdbDevice(const std::string& id, std::string& removed_key);
};

#endif // _FOX_SECRETS_HPP
