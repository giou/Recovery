# Remote control & the HTTP server

Scope: the remote-control stack — REST dashboard + screen mirror + input
injection, over WLAN and over plain USB/adb. Sources: `fox_webserver/`,
`fox_fifo/fox_input.*`, `fox_fifo/fox_screen*`, `fox_fifo/fox_remote_*`.

The WebUSB/FIFO remote-control engine is **always built** (it works over plain
adb with no network). The HTTP server, ttyd, NAS, and the WLAN UI actions are
gated by `OF_ENABLE_WLAN`. The old `OF_ENABLE_REMOTE` flag was **deleted**.
libmicrohttpd was removed — `fox_httpd` is a hand-written POSIX-socket server.

## Engine dispatch (typed, no argv)

The engine consumes a parsed Fox RPC request — an `op` plus a typed JSON `args`
object — directly (`Fox_Fifo::Run_Command` → `Cmd_*(const Json::Value&)`). There
is no argv round-trip; request *formatting* is the client's job (foxcli). Only
the three positional control verbs (`input`/`screencap`/`screenstream`) are still
flattened to a small token list, by `Fox_Protocol::ControlArgv`, because
`Fox_Remote_Input` / `Fox_Screen_Stream` consume them that way.

## The `fox web` CLI tree

foxcli groups the networking surface under one verb; each maps to an engine op:

- `fox web {enable|disable|status}` → op `web` — umbrella: `enable` brings up the
  HTTP server + ttyd + `Fox_Adbd::StartSecure(5555)` (refused unless a password
  is set).
- `fox web password [<value>|clear|--generate]` → op `web` — see
  `docs/security.md`.
- `fox web webserver {enable|disable|status|http …|https …|redirect …|cert …|cors …|terminal …}`
  → op `webserver` (`Cmd_Webserver`). The HTTP server + its transports/policy.
- `fox web adb {enable|disable|status|pair|list|forget <id>}` → op `adbd`
  (`Cmd_Adbd`). Network ADB + the paired-device registry.
- `fox web webserver` (screen/input plane) is gated separately by the `control`
  op: when off, `/screen.png`, `/stream.mjpeg`, `/api/input/*` return 403; the
  REST/job plane is unaffected. Does NOT gate the `input` / `screencap` verbs
  (those need adb anyway). State: `Fox_Httpd::SetControlEnabled/ControlEnabled`
  (`g_control_enabled`, default true); `/api/status` returns a `control` bool.

`fox wifi` is an alias for `fox wlan` (both ends accept it).

## HTTP / HTTPS listeners (the `webserver` group)

The plaintext HTTP server (`Fox_Httpd`) and the TLS HTTPS server (`Fox_Httpsd`,
both in `fox_webserver/`) are **two independent listeners** that can run at the
same time on separate ports (HTTP default 80, HTTPS default 443). They share the
exact same request handlers (`Fox_Http::*` in `fox_http_routes/_api/_screen/
_static`); the transport difference is hidden behind `Fox_Io::Conn`
(`fox_io.*`), which wraps either a plain socket fd or an established `SSL*`.
The same auth token applies to both.

The transports and the CORS policy are nested under the `webserver` verb
(handlers `Cmd_Http` / `Cmd_Https` / `Cmd_HttpRedirect` / `Cmd_Cors` in
`fox_fifo/fox_fifo.cpp`):

- `webserver http {enable|disable|status} [--port N]` — the plaintext listener
  on its own. Independent of `web` (which additionally brings up ttyd + adbd).
- `webserver https {enable|disable|status|cert} [--port N]` — the TLS listener.
  TLS 1.2+ only. `enable` ensures a self-signed cert exists (generating one if
  missing), then starts the listener. `cert regen` forces a fresh keypair+cert
  and reloads a running listener. `status` prints the cert's SHA-256 fingerprint
  (colon hex) for browser pinning.
- `webserver redirect {enable|disable|status}` — a **mode of the plaintext
  listener**: when on, plaintext (non-TLS) requests get a `301 Moved
  Permanently` to the HTTPS endpoint and nothing else (no auth, no static, no
  API — the redirect is public metadata). Persisted across reboots as
  `/data/media/0/Fox/tls/redirect.enabled`. Requires `webserver http` +
  `webserver https` to both be running to actually serve redirects.
- `webserver cors {set <origin>|reset|status}` — inspect or override the
  cross-origin allow-list (see "CORS" in `docs/security.md`). In-memory only.

(These are protocol verbs over the FIFO — e.g. `fox fifo send webserver https
enable` — not a standalone CLI.)

**Self-signed cert** (`Fox_Tls_Cert`, `fox_tls_cert.*`): ECDSA P-256, self-signed
with SHA-256, 10-year validity, CN/SAN = WLAN IP (when available) + `localhost`.
Stored at `/data/media/0/Fox/tls/server.{crt,key}` (mode 0600), generated once
and reused across reboots so a browser can accept/pin it. Threat model identical
to `Fox_Secrets` (FBE-encrypted user data, device-bound, NOT hardware-backed).
Browsers will warn about the self-signed cert until the operator trusts it.

No new dependency: BoringSSL `libssl` was already linked into recovery for the
adbd pairing code (`Android.mk`); HTTPS reuses it. The TLS accept path
(`fox_httpsd.cpp`) does `SSL_CTX_new(TLS_server_method())` + per-conn
`SSL_accept()` with a 5s handshake timeout, then hands the `SSL*` to
`Fox_Http::HandleConnection` wrapped in a `Fox_Io::Conn`.

## ttyd — source-built, FIFO-launched, no init service

`ttyd` (the web terminal) is **built from sources** at `external/ttyd`
(`OrangeFox16/android_external_ttyd`, branch `fox_16.0`); the Soong module is
`ttyd_src` with `stem: "ttyd"`, so it lands at `/system/bin/ttyd`. The
`orangefox.mk` WLAN block requires that clone (same `$(wildcard …Android.bp)`
guard pattern as `wpa_supplicant_8`) and adds `ttyd_src` to
`TWRP_REQUIRED_MODULES`. There is no longer a checked-in prebuilt binary.

**Launch model:** ttyd is **never** started by `init`. The fork ships a
`ttyd.rc` that gates `start/stop ttyd` on `init.svc.microhttpd`; that file is
**deliberately not installed**, so ttyd is launched purely on demand:

- `fox terminal start` / `fox web enable` → `Fox_Ttyd::Start()` in
  `fox_webserver/fox_ttyd.cpp`, which fork/execs `/system/bin/ttyd` with HTTP
  basic-auth `fox:<remote-password>` read from `Fox_Secrets`. It refuses to
  start without a password (secure default — see `docs/security.md`).
- The dashboard "Start/Stop ttyd" buttons hit the same path via the job layer.
- The GUI "Remote Web Access" switch calls the `foxweb` GUI action
  (`gui/action.cpp`), which delegates to `Fox_Fifo::Cmd_Web` — the same code
  path as `fox web enable/disable`, so the two can't drift.

`ttyd_path()` in `fox_ttyd.cpp` still probes `/system/bin/ttyd` first, matching
the install path, so no runtime path change was needed for the source build.

## The HTTP server (`fox_webserver/fox_httpd.*`)

Tiny embedded HTTP server exposing the recovery engine as a remote dashboard:
REST control plane over the async job layer (`Fox_Jobs`), MJPEG screen stream
(`Fox_Screen`), touch/key injection (`Fox_Remote_Input`). API-only by default;
serves a static dashboard bundle from `/system/etc/fox/dashboard` on the ramdisk
when present (`ServeStatic()`/`guess_mime()` in `fox_http_static.cpp`). The build
packs that bundle only when `FOX_REMOTE_DASHBOARD=1`. The old hard-coded
`fox_httpd_page.h` / `fox_webusb.html` were deleted — don't bring them back.
All connection I/O (headers, body, MJPEG stream) goes through `Fox_Io::Conn`
(`fox_io.*`) so the same code path serves plaintext and TLS.

**Auth:** token-gated (see `docs/security.md`). The shared remote password is
used as the token if set, else a random per-boot token.

## Jobs (`fox_webserver/fox_jobs.*`, `fox_job_dispatcher.*`)

`Fox_Jobs` turns the synchronous, single-command `fox` engine into an async,
pollable job model: submit a command, get an id back immediately, stream output
+ a final exit code. The engine itself stays serial — recovery has one action
thread and mutates global state (PartitionManager, DataManager), so `Fox_Jobs`
is a **serialized queue**, not a parallel executor. One job runs at a time,
gated by the same active-command/`tw_busy` guard the FIFO path uses.

Threading:
- `Submit()` / `Get*()` / `List*()` are called from the HTTP server thread.
- `BeginNext()` / `NotifyExit()` / `FinishActive()` are called from the GUI
  thread (`Fox_Job_Dispatcher` + the `foxcmd` action), which owns dispatch.

## Critical integration facts — read before touching capture/graphics/input

These are non-obvious and have caused real bugs.

### Screen capture MUST happen on the GUI thread

Between `PageManager::Render()` and `gr_flip()`. `gr_save_screenshot()` reads
`gr_mem_surface`, whose `.data` pointer `gr_flip()` repoints to the back buffer.
OrangeFox also only renders on change. Capturing from the HTTP thread therefore
(a) reads the off-screen buffer and (b) races flip → split/half-white frames.

So `Fox_Screen::CaptureFrameIfWanted()` /
`Fox_Screen_Service::CaptureRenderedFrameIfWanted()` /
`Fox_Screen_Stream::CaptureRenderedFrameIfDue()` run GUI-side (see the calls at
`gui/gui.cpp` ~773–775 and ~804–806); the HTTP thread only `GetLatest()`. This
also makes the stream event-driven ("capture on change").

### Do NOT add new exported symbols to libminuitwrp

(e.g. a memory-stream screenshot variant.) When the recovery binary and the
`libminuitwrp.so` in the ramdisk fall out of sync this produces
`CANNOT LINK EXECUTABLE ... cannot locate symbol` at boot (no splash). Reuse the
already-exported `gr_save_screenshot()` by writing to a tmpfs path
(`/tmp/.fox_screen.png`) and reading it back. All 3 graphics backends
(fbdev/overlay/drm) use a single persistent in-memory `gr_draw`/`draw_buf`
surface; `flip()` just memcpys it to hardware — so capture-before-flip = current
frame.

### Input injection needs no GUI changes

`minuitwrp/events.cpp` `ev_get()` re-stats `/dev/input` every ~2s and re-runs
`ev_init()` when mtime changes. A `/dev/uinput` virtual touchscreen/keyboard is
auto-detected; events flow through `vk_modify` (handles ABS_MT_POSITION_X/Y,
ABS_MT_TRACKING_ID, ABS_MT_SLOT, BTN_TOUCH, SYN). Set the uinput ABS range to
`0..gr_fb_width/height` so `check_mouse` calibration is identity.

### MJPEG framing — end each frame with its own boundary

Browsers commit a `multipart/x-mixed-replace` part only when the boundary that
*follows* it arrives (Content-Length is ignored for render timing). Emit a
leading `--boundary\r\n` once, then end *each* frame with its own trailing
`\r\n--boundary\r\n` immediately (`<headers><data>\r\n--boundary\r\n`). Sending
the boundary at the start of the next frame = perpetual one-frame lag. Fixed in
`handle_stream`.

### Rate-limit floor

Keep the per-frame floor small (`kMinIntervalMs` ~60) so discrete page changes
capture promptly; idle refresh is slow (`kIdleForceMs` ~1000); a `g_pending`
flag flushes a floor-skipped change within ~60ms instead of waiting for the idle
tick. Don't make the idle/floor interval large or changes get dropped
("one frame behind").

### Job dispatch runs on the GUI thread

Via the `foxcmd` action path. `Fox_Job_Dispatcher::Poll()` is called from the
main loop (`gui/gui.cpp` ~749).

## WebUSB (adb) path — near-zero device code

The browser uses ya-webadb (`@yume-chan/adb`) over WebUSB → ADB built-in
`tcp:<port>` forward service → reaches `fox_httpd` on localhost (it binds
`INADDR_ANY`). Bootstrap: browser runs `shell:fox remote start`, parses
`port=`/`token=` from stdout, then speaks plain HTTP over
`adb.createSocket("tcp:port")`. REST screen uses live MJPEG (`/stream.mjpeg`);
USB screen polls `/screen.png` (can't stream multipart over a single adb socket).

## No-network USB path

( avoids the loopback TCP stack `tcp:` uses). ADB `shell:`/`exec:`/`sync:` are
pure USB streams. `fox input {tap|swipe|key|down|move|up}` (drives `Fox_Input`
uinput) and `fox screencap [path]` (writes PNG via `gr_save_screenshot`, default
`/tmp/.fox_screen.png`) let a client control the device over plain `adb shell` +
`sync/pull` with no HTTP/IP. Both always built.

## Related

- `docs/security.md` — auth token, ttyd password requirement, Fox_Secrets.
- `docs/fox-protocol.md` — the `fox` FIFO command protocol these commands ride on.
- `gui/AGENTS.md` — the GUI side (`foxcmd` action, `gui_set_FILE` output sink).
