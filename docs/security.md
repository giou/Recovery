# Security model & secrets

Scope: the OrangeFox network/remote-access security surface — `Fox_Httpd`,
`Fox_Secrets`, `Fox_Ttyd`, network adbd. All of this is under `OF_ENABLE_WLAN`.
Read this before changing anything in `fox_webserver/`, `fox_secrets/`,
`fox_adbd.cpp`, or the auth/token paths.

## The HTTP server grants root — token-gate everything

**`Fox_Httpd` grants unauthenticated root with decryption powers.** Every data
endpoint requires a per-session bearer token. Bind only on trusted networks.

## CORS — restricted by default, override for development

The HTTP/HTTPS responses no longer send a blanket `Access-Control-Allow-Origin:
*`. The policy lives in `Fox_Httpd::ResolveCorsOrigin()`; `HandleConnection`
resolves it once per connection and every reply echoes the result (or no CORS
header at all when the origin is disallowed). The default allow-list is:

- the device-local origins — its WLAN IP and `localhost`/`127.0.0.1` (so the
  on-device dashboard works same-origin), and
- the official `orangefox.tech` and `orangefox.download` domains **and their
  subdomains** (so a hosted dashboard can drive a device on the LAN).

A matching `Origin` is echoed back verbatim (with `Vary: Origin`); anything else
gets no `Access-Control-Allow-Origin` header, so the browser blocks the
cross-origin read. The token still gates the data itself — CORS is defence in
depth, not the primary control.

`webserver cors set <origin>` installs a developer override: `set "*"` echoes a
wildcard to any origin, any other value adds that exact origin to the default
list. `webserver cors reset` restores the default. The override is **in-memory
only** (deliberately not persisted) so a reboot returns to the safe default.

## Shared remote password (one secret, two transports)

Managed under the `fox web` umbrella verb (`Cmd_Web` in `fox_fifo/fox_fifo.cpp`):

- used as **ttyd basic-auth** (`ttyd -c fox:<pw>`), and
- as the **REST API token** (`g_token`).

`Fox_Httpd::AuthIsPassword()` lets status output print `auth=remote-password`
without leaking the value. With no password set, the token falls back to a random
per-boot `make_token()`, compared via constant-time `ct_equal()`.

**ttyd REQUIRES a password** (secure default) — `Fox_Ttyd::Start` returns false
with an error if none is set.

Password management (the `web password` verb / `Cmd_Web`):

- `web password` (no value) — prints the current password (`FOX_WEB_PASSWORD`
  block; `password_set=0` when none). The caller explicitly asked to see it.
- `web password <value>` — set it (never echoed back).
- `web password --generate` — mint a 20-char random alphanumeric password from
  `/dev/urandom`, store it, and return it once.
- `web password clear` — remove it.

## Authorized network-ADB devices (Fox_Secrets registry)

`web adb pair` runs the Android pairing server; on success the host's adb RSA
public key is appended to `adb_keys` **and** recorded in a Fox_Secrets registry
(`adb_devices` in `secrets.json`): `{fingerprint, name, key, last_seen}`. The
fingerprint is the first 16 hex chars of SHA-256 over the key's base64 token;
the name is the key's trailing comment (e.g. `user@host`). The public key is not
secret, so the registry is plaintext.

- `web adb list` — `FOX_ADB_DEVICES` block of `fingerprint\tname\tlast_seen`.
- `web adb forget <fingerprint|name>` — removes the entry from the registry and
  best-effort strips the matching key line from the `adb_keys` files, so the
  host can no longer authenticate.

## Fox_Secrets — the unified AES-256-GCM store

`fox_secrets/fox_secrets.{cpp,hpp}` owns `/data/media/0/Fox/secrets.json`
(mode 0600, atomic tmp+rename). Every value is AES-256-GCM encrypted with a
device-bound key (`GetCryptoKey`, SHA256 of `ro.serialno`+device+product).

API: `Set/Get/Clear RemotePassword`, `Set/Get/Delete/ListWlanNetwork`.
JSON schema: `{version, remote:{password:{iv,cipher,tag}}, wlan:{<ssid>:{enc,iv,cipher,tag}}}`.

**Threat model (documented in code, do not oversell):** the encryption key is
derived from device properties and the derivation logic ships inside recovery,
so anyone who controls the booted recovery can reproduce the key. This protects
against casual viewing and against pulling the raw file off the flash; it is NOT
hardware-backed secrecy against an attacker running this recovery.

## CRITICAL — do not change this string

The key-material string `"OrangeFox-WLAN-Saved-v1|...|INFINITI-WLAN"` in
`fox_secrets.cpp` **MUST stay byte-identical** — it decrypts the legacy
`wlan_saved.enc` during one-time import. Editing it breaks migration.

## wlan.cpp migration note

`wlan.cpp`'s old `OF_Save/Load/Delete/GetEncryptedSavedSsids` are now thin
wrappers over `Fox_Secrets`. All the old hex/crypto/secure-file helpers were
removed. Watch for `-Werror unused-function` if you revert them.

## Build/linking gotchas — network adbd pairing libs

`fox_adbd.cpp` uses `PairingServerCtx` from `<adb/pairing/pairing_server.h>`.

**Link the pairing libraries STATICALLY.** Recovery has no APEX/.so search path,
so the shared variants fail at runtime with
`library "libadb_pairing_server.so" not found` (their impl only ships inside the
com.android.adbd APEX, which recovery doesn't have). In `Android.mk`:

```
LOCAL_STATIC_LIBRARIES += libadb_pairing_server_static \
    libadb_pairing_connection_static libadb_pairing_auth_static \
    libadb_crypto_static libadb_tls_connection_static \
    libadb_protos_static libadb_sysdeps libcrypto_utils
```

- **kati does NOT propagate the transitive static closure** of Make→Soong
  static-lib refs, so the ENTIRE closure must be listed explicitly. All are
  `recovery_available: true`.
- `libadb_tls_connection_static` needs boringssl `libssl` (recovery links
  `libcrypto` but not `libssl` → add `libssl` to `LOCAL_SHARED_LIBRARIES`).
- Do NOT add `//apex_available: platform` to the *stub* pairing cc_library
  modules (`libadb_pairing_auth/connection/server`) — soong rejects it
  ("Stub libraries should have a single apex_available"). `recovery_available: true`
  + visibility on the `*_defaults` are what enable recovery linking.
- Visibility is NOT enforced for Make→Soong refs, so a `//bootable/recovery`
  Make module can use minadbd-only-visible libs without `.bp` visibility edits.
- `libminadbd_services` / `libadbd.recovery` / `minadbd` are
  `TWRP_REQUIRED_MODULES` (separate binaries in the image) — they do NOT provide
  symbols to the `recovery` binary.

## Related

- `docs/remote-control.md` — how the HTTP server fits into the remote-control stack.
- `docs/wlan.md` — WLAN secrets usage.
