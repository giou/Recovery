# AGENTS.md — OrangeFox Recovery (`bootable/recovery`)

Read this first, then the specialized doc for the area you're touching.

## What this is

**OrangeFox Recovery (OFRP)** — a custom Android recovery image (the `recovery`
binary + ramdisk). The whole tree is OrangeFox code: the recovery core, the GUI,
the command engine, and all the network/remote features are maintained together.
There is no "upstream vs OrangeFox" split; treat every directory as fair game.

Major subsystems:

- **GUI** (`gui/`) — XML-themed UI rendered by `minuitwrp`. Pages, styles,
  resources and strings are all XML. **→ see [`gui/AGENTS.md`](gui/AGENTS.md)**
  for the UI authoring guide.
- **fox command engine** (`fox_fifo/`) — the modern `fox` RPC engine. JSON
  requests and NDJSON events over a FIFO pair. **→ see
  [`docs/fox-protocol.md`](docs/fox-protocol.md).**
- **USB screen streaming & input** (`fox_fifo/`) — direct USB/adb screen streaming
  (`fox_screen_stream`), framebuffer PNG capture, and uinput touch/key injection.
- **Network/remote stack** (under `OF_ENABLE_WLAN`) — HTTP REST dashboard +
  MJPEG screen mirror + HTTP input, WLAN, secrets, network adbd, ttyd, OTA,
  NAS. **→ see [`docs/remote-control.md`](docs/remote-control.md),
  [`docs/wlan.md`](docs/wlan.md), [`docs/security.md`](docs/security.md).**
- **Recovery core** — partition management (`partition*.cpp`,
  `partitionmanager.cpp`), flashing/wipe/backup (`twrp*.cpp`,
  `openrecoveryscript`), install/OTA install (`install/`).

## Project layout

```
gui/               FoxUiEngine — the OrangeFox UI engine, static lib (libfoxui). XML-driven. See gui/AGENTS.md.
  *.cpp objects.hpp        GUIObject subclasses (GUIButton/GUIWlan/GUIConsole/...)
  action.cpp               GUIAction handlers backing <action function="...">
  pages.{cpp,hpp}          XML page/template loader (PageManager)
  resources.{cpp,hpp}      image/font/string ResourceManager
  theme/                   XML themes, packed into the ramdisk as /twres
    portrait_hdpi/ui.xml      theme manifest
    portrait_hdpi/pages/      page + template definitions
    portrait_hdpi/resources/  vars.xml, images.xml, styles.xml
    portrait_hdpi/themes/     style.xml + styles/{Dark,Light,Gray,Cream,Black}.xml
    common/languages/         string resources (en.xml, ru.xml, ...)

fox_fifo/          The "fox" command engine (always built). See docs/fox-protocol.md.
  fox_fifo.{cpp,hpp}           verb dispatcher (Run_Command → Cmd_*)
  fox_channel.{cpp,hpp}        mkfifo + select() wiring into the GUI event loop
  fox_command_dispatcher.{cpp,hpp} serialized foxcmd action dispatch
  fox_protocol.{cpp,hpp}       Fox RPC JSON parse, event stream helpers, base64
  fox_input.{cpp,hpp}          /dev/uinput touch/key injection (adb-usable)
  fox_screen*.{cpp,hpp}        framebuffer→PNG capture + stream
  fox_remote_input/state.{cpp,hpp} remote input helpers + shared state

fox_webserver/     HTTP server + REST/job layer + ttyd (OF_ENABLE_WLAN only). See docs/remote-control.md.
fox_secrets/       AES-256-GCM secret store (Fox_Secrets). See docs/security.md.
fox_adbd.{cpp,hpp} Network adbd (StartSecure/StartNoAuth/StartPairing).
fox_updater.{cpp,hpp}  Built-in OTA updater (FoxUpdater).
wlan.{cpp,hpp}     WLAN via wpa_supplicant/wpa_cli. See docs/wlan.md.
nas/               NAS manager (NasManager).

orscmd/orscmd.h    FIFO paths: foxin/foxout/foxcancel/foxscreenout (+ legacy orsin/orsout).
minuitwrp/         graphics (gr_save_screenshot) + input backend.

Android.mk         Builds `recovery`; fox_fifo/* always built, network code gated by OF_ENABLE_WLAN.
orangefox.mk       OF feature flags, OF_ENABLE_WLAN wiring, dashboard packing rule.
gui/Android.bp + gui/libfoxui_defaults.go   build the GUI lib + pick the theme.

docs/              Specialized design docs (security, wlan, remote-control, fox-protocol).
```

## Build

Built inside a full Android (AOSP/Lineage) source tree, not standalone. Recovery
is a Make (kati) module; Soong (Android.bp/.go) modules are referenced from it.

- The `recovery` binary is defined in `Android.mk` (`LOCAL_MODULE := recovery`).
  It links **FoxUiEngine as a static library** `libfoxui` (built by `gui/Android.bp`,
  source list + cflags from `gui/libfoxui_defaults.go`).
- **`OF_ENABLE_WLAN=1`** (device tree) gates the network features: `wlan.cpp`,
  `fox_adbd.cpp`, `fox_secrets/`, `nas/`, `fox_updater.cpp`, and the
  `fox_webserver/` sources. It also defines `-DOF_ENABLE_WLAN` for conditional
  use of the network stack from always-built sources.
- **`fox_fifo/` is ALWAYS built**, even without WLAN — it is the command engine
  and the USB/adb remote-control path (`fox input`, `fox screencap`,
  `fox screen stream`).
- **`FOX_REMOTE_DASHBOARD=1`** additionally packs `fox_webserver/dashboard/` onto
  the ramdisk at `/system/etc/fox/dashboard` (requires `OF_ENABLE_WLAN`). Without
  it the HTTP server is API-only.
- Required external source clones (the build errors out if missing):
  `external/jsoncpp` (`libjsoncpp`, always), `external/wpa_supplicant_8`
  (`android16-s2-release`, for WLAN), and `external/ttyd`
  (`OrangeFox16/android_external_ttyd`, `fox_16.0`, for the web terminal — Soong
  module `ttyd_src`; the fork's `ttyd.rc` is NOT installed, ttyd is FIFO-launched
  only).
- libmicrohttpd was removed — `fox_httpd` is a hand-written POSIX-socket
  server, not an MHD-based one. Don't reintroduce it.
- Conditional GUI sources (`gui/wlan.cpp`, custom keyboards) are added in
  `gui/libfoxui_defaults.go`, not `gui/Android.bp`. See `gui/AGENTS.md`.

You typically don't "run" a build here — it's driven from the Android tree root.
`tests/` holds unit/fuzz/manual tests for the recovery core (package
verification); there is no test harness aimed at the fox modules.

## Conventions

- **C++ style:** follow `.clang-format` (tabs-ish, Allman-ish braces, GPL header
  block on fox_* files). Match the surrounding file's density and naming.
- **Always-built vs WLAN-only:** anything reusable over plain adb/USB (engine,
  input, screencap, screen stream) goes in `fox_fifo/` and stays unconditional.
  Network features go in `fox_webserver/` / `fox_adbd.cpp` / `wlan.cpp` and are
  `#ifdef OF_ENABLE_WLAN` where called from always-built sources and gated in
  `Android.mk`.
- The old `OF_ENABLE_REMOTE` flag was **deleted** — the remote-control engine is
  always built; the HTTP server is the part gated by WLAN.
- The Rust `fox` client lives in a **separate repo** (`external/foxcli`, gitlab),
  not here. It formats human commands into Fox RPC requests; `fox rpc` is the
  raw JSON bridge for WebUSB-style callers.

## Where to look

| Working on… | Read this |
|---|---|
| Any UI / `gui/` | [`gui/AGENTS.md`](gui/AGENTS.md) |
| The `fox` CLI / FIFOs / engine | [`docs/fox-protocol.md`](docs/fox-protocol.md) |
| HTTP server, screen mirror, input, jobs, WebUSB | [`docs/remote-control.md`](docs/remote-control.md) |
| WLAN, saved networks | [`docs/wlan.md`](docs/wlan.md) |
| Secrets, auth tokens, adbd pairing/linking | [`docs/security.md`](docs/security.md) |
| Running without a device (Cuttlefish) | [`docs/emulator.md`](docs/emulator.md) |
