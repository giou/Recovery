# Fox RPC schema — the op/args contract

**Single source of truth** for the Fox RPC wire shape, shared by the two ends:

- **engine** — `bootable/recovery/fox_fifo/fox_fifo.cpp` reads these fields via
  the `arg_*` helpers in `Cmd_*` handlers (`Fox_Fifo::Run_Command` dispatches on
  `op`).
- **client** — `external/foxcli` builds them in `Command::to_request()`
  (`src/args.rs`). The raw `fox fifo send <tokens>` / `fox rpc` paths bypass the
  typed builder.

A request is `{"v":1, "id":"<opt>", "op":"<op>", "args":{…}}`. Responses are an
NDJSON event stream (`log`/`progress`/`data`/`error`, terminated by `result`).
**When you change a field here, change both ends and this table together.**

## Generic group shape

The networking family (`web`, `webserver`, `adbd`, `control`, `ota`, `mtp`,
`screenstream`) shares a positional-ish shape so the engine handlers stay
uniform:

| field | type | meaning |
|---|---|---|
| `action` | string | the subcommand verb (e.g. `enable`, `https`, `pair`) |
| `subaction` | string | the nested verb (e.g. `enable` under `https`; the id for `adbd forget`) |
| `values` | string[] | any further tokens (e.g. `["regen"]`, `["set","*"]`) |
| `port` | int | `--port` |
| `timeout` | int | `--timeout` (adbd pair) |
| `fox_id`, `type` | string | OTA filters |
| `fps` | int | screenstream rate |
| `no_auth` | bool | adbd no-auth mode |
| `generate` | bool | `web password --generate` |

## Per-op fields

| op | args |
|---|---|
| `status`, `sideload`, `reflash`, `log`, `storages` | — |
| `mount`, `unmount` (`umount`) | `paths`: string[] |
| `flash` (`install`) | `zips`: string[]; `verify`: bool?; `reflash`: bool?; `unmount_system`: bool; `unmount_vendor`: bool |
| `backup` | `parts`: string[]; `name`; `path`; `storage`; `compress`: bool; `digest`: bool (default true); `encrypt`: bool; `password`: string? |
| `restore` | `parts`: string[]; `name`; `path`; `digest_check`: bool (default true); `password`: string? |
| `decrypt` | `password`; `user`: int |
| `wipe` | `parts`: string[] |
| `format_data` | `confirm`: bool |
| `reboot` | `target` |
| `wlan` (`wifi`) | `action`; `ssid`; `password` |
| `partition` | `path`; `action`; `fs` |
| `addons` | `action`; `names`: string[] |
| `internal` | `action`; `name`; `value`; `page` |
| `ota` | `action`; `fox_id`; `type` |
| `mtp` | `action` |
| `web` | generic group + `generate` (password value rides in `subaction`/`values`) |
| `webserver` | generic group (`action`=group, `subaction`=verb, `values`=rest) |
| `adbd` | generic group; `device` accepted as an alias for the forget id |
| `control` | `action` |
| `input` | `action`; `key`; `code`: int; `x`,`y`,`x2`,`y2`,`duration`: int |
| `screencap` | `path`; `base64`: bool |
| `screenstream` | `action`; `fps`: int |
| `ors` | `path` |
| `ors-cmd` | `raw`: string[] |

Notes:
- `wifi` is an alias for `wlan` accepted on both ends.
- `input`/`screencap`(base64)/`screenstream` are intercepted by
  `Fox_Command_Dispatcher::dispatch_control` and consumed typed by
  `Fox_Remote_Input` / `Fox_Screen_Stream`; they do not reach `Run_Command`.
