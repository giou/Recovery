# WLAN

Scope: WiFi bring-up and saved-network management. All under `OF_ENABLE_WLAN`.
Main source: `wlan.{cpp,hpp}` (the `Wlan` class). Secrets go through
`Fox_Secrets` (see `docs/security.md`).

## What it does

`Wlan` brings up WiFi in recovery using `wpa_supplicant` + `wpa_cli` + DHCP.
It is driven both from the GUI (`gui/action.cpp` `wlan_*` actions → `gui/theme/.../pages/wlan.xml`)
and from the `fox` CLI (`fox wlan ...` → `Fox_Fifo::Cmd_Wlan`).

Key entry points (`wlan.hpp`):
- lifecycle: `Init` / `Enable` / `Disable` / `IsEnabled`
- scanning: `Scan` / `BuildScanList`
- connecting: `Connect` / `ConnectSaved` / `ForgetSaved` / `TestConnection`
- status: `Info` / `UpdateConnectedName` / `RefreshSaved`

Internals (private): supplicant conf/service management (`EnsureSupplicantConf`,
`StartSupplicant`/`StopSupplicant`, `StartInitSupplicantService`), DHCP
(`StartDhcp`/`StopDhcp`), property polling (`WaitForProperty`), and helpers to
locate the `wpa_cli` / `ifconfig` binaries (the busybox locator is legacy —
nothing on the current paths needs it).

## Recent behavior (v17–v20)

- **IP handling:** the connect paths read the interface's IPv4 from the kernel
  (`SIOCGIFADDR`) — the external `dhcptool` never reports the lease into
  supplicant STATUS, so parsing STATUS for `ip_address` came up empty and the
  engine declared failure on a working link. `InfoLocked` publishes the IP as
  `wlan_info_ip` + the `sys.fox.wlan.ip` property (cleared on disconnect).
- **`fox wlan info`** prints from the state vars (`wlan_info_text` /
  `wlan_info_state`) — the old `/tmp/wlan/info.txt` reader printed nothing
  (nothing wrote that file).
- **`TestConnection`** uses a native TCP connect (1.1.1.1:443, 4 s) +
  `getaddrinfo` — the recovery ships no busybox/ping, and the kernel's
  `ping_group_range` blocks ICMP datagrams. `StartDhcp` also writes
  `/etc/resolv.conf` from the DHCP-provided `net.<iface>.dns1/2` (bionic reads
  it in recovery; there is no netd).
- **Fixed MAC:** `EnsureFixedMac` generates a locally-administered MAC once
  (`of_wlan_fixed_mac`) and applies it on every enable (fail-open).
- **Sockets shield:** the device tree's bring-up mounts a tmpfs over
  `/data/vendor/wifi/sockets` only (never the whole `wifi` dir — the v17
  whole-dir version hid `wlfw_cal_01.bin` and killed calibration).

## Saved networks & secrets

Saved WLAN credentials live in the unified `Fox_Secrets` store
(`/data/media/0/Fox/secrets.json`), not a separate file. The old
`OF_Save/Load/Delete/GetEncryptedSavedSsids` helpers in `wlan.cpp` are thin
wrappers over `Fox_Secrets::Set/Get/DeleteWlanNetwork`. See `docs/security.md`
for the encryption model and the **byte-identical key-material string** warning.

`fox wlan password_list` prints a tab-delimited `ssid\tencryption\tpassword`
table (decrypting each saved net via Fox_Secrets).

## Hotspot / SoftAP (`OF_WLAN_AP`)

When built with `OF_WLAN_AP=1` (on top of `OF_ENABLE_WLAN`), `Wlan` can also
*create* a WiFi hotspot instead of only joining one. It reuses the same
`wpa_supplicant` instance in AP mode (`set_network 0 mode 2`) — there is **no
`hostapd`** — and runs `dnsmasq` as the DHCP/DNS server for clients. The radio
must support AP mode at the driver level; STA and AP are mutually exclusive on a
single radio, so enabling the hotspot drops any active client connection.

Entry points (`wlan.hpp`, all under `#ifdef OF_WLAN_AP`):
- `ApEnable` / `ApDisable` / `ApIsEnabled`
- `ApSetSsid` / `ApSetPassword` (persist via `Fox_Secrets::SetApConfig`; if the
  hotspot is running they restart it to apply)
- `ApGetConfig` (SSID + password, defaulting SSID to `OrangeFox`)
- `ApListClients` → `Wlan::ApClient{ mac, ip, hostname }`, parsed from the
  dnsmasq lease file

Fixed network parameters (see the `WLAN_AP_*` / `AP_*` constants in `wlan.cpp`):
gateway `192.168.43.1/24`, DHCP pool `192.168.43.10–100`, channel 1 (2412 MHz),
lease/pid files under `/tmp/wlan/ap/`. An empty password means an open hotspot;
otherwise WPA2-PSK with an 8–63 char key.

The AP config is stored in `Fox_Secrets` under the `ap` key
(`SetApConfig`/`GetApConfig`/`HasApConfig`), password encrypted like the WLAN
client entries.

CLI (`Fox_Fifo::Cmd_Wlan`, `sub == "ap"`):

```
fox wlan ap enable | disable | status
fox wlan ap ssid <name>
fox wlan ap password <pw>      # "clear" or empty => open hotspot
fox wlan ap list               # connected devices: ip / mac / hostname
```

`ap status` emits `wlan_ap_status` and `ap list` emits `wlan_ap_clients` data
events for WebUSB/JSON callers. In the Rust `fox` client these map through the
`WlanCommand::Ap` group → `{ action: "ap", subaction: <verb>, values: [...] }`.

## Build wiring

Gated entirely by `OF_ENABLE_WLAN=1` in the device tree:
- `wlan.cpp` is added to `LOCAL_SRC_FILES` in `Android.mk` under the
  `ifeq ($(OF_ENABLE_WLAN),1)` block, with `-DOF_ENABLE_WLAN`.
- `orangefox.mk` uses the official manifest's `external/wpa_supplicant_8`
  (`android16-s2-release`) and adds `wpa_supplicant`, `wpa_cli`, `dhcptool` to
  `TWRP_REQUIRED_MODULES`, plus the wifi supplicant VINTF manifest and the
  relevant HIDL/AIDL `.so` libs to `RECOVERY_LIBRARY_SOURCE_FILES`.
- Network Storage (NAS) additionally needs `/system/bin/rclone` +
  `/system/bin/fusermount3`, installed by the official `external/rclone`
  (`fox_cloud_mount_rclone`) and `external/libfuse3`
  (`fox_cloud_mount_fusermount3`) modules. The CI fails if a wlan ramdisk
  lacks them — see the `nas` op in `docs/fox-rpc-schema.md`.
- The device tree must provide, load and manage the actual WiFi drivers.

Hotspot support adds `OF_WLAN_AP=1` (only meaningful with `OF_ENABLE_WLAN`):
- defines `-DOF_WLAN_AP`, gating all AP code in `wlan.cpp` / `fox_fifo.cpp`;
- requires `external/dnsmasq` and adds `dnsmasq` to `TWRP_REQUIRED_MODULES`
  (`RECOVERY_BINARY_SOURCE_FILES += $(TARGET_OUT_EXECUTABLES)/dnsmasq`). If your
  dnsmasq module installs somewhere other than `/system/bin`, adjust that path;
  the runtime locator also searches `/system/xbin`, `/vendor/bin`, etc.
- The device tree's WiFi driver must support AP mode.

## GUI

The WLAN UI is `gui/theme/portrait_hdpi/pages/wlan.xml` (+ templates like
`wlan_password_page`). It is driven by DataManager values — see `gui/AGENTS.md`
for the authoring model. The relevant vars (defined in
`gui/theme/portrait_hdpi/resources/vars.xml`, set by the `wlan_*` GUIActions
and `Wlan::*`): `tw_wlan_enabled`, `tw_wlan_connected`, `wlanlistdisplay`,
`wlanselectedid`, `wlanencryption`, `wlan_password`, `wlan_connected_name`,
`wlan_info_ssid`, `wlan_info_ip`, …

The statusbar WLAN icon is the `GUIWlan` object (`gui/wlan.cpp`, added to the
build conditionally in `gui/libfoxui_defaults.go`).

## Related

- `docs/security.md` — Fox_Secrets, the encryption model.
- `docs/remote-control.md` — the HTTP/remote stack that rides on top of WLAN.
