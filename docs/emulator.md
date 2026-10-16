# Emulator / device-free iteration

Scope: how to run and iterate on OrangeFox Recovery **without a physical
device**, using the real `recovery` image booted in a **Cuttlefish virtual
device** — AOSP's `vsoc_*` virtual device. Full-fidelity boot/partition/WLAN
testing, including an emulated radio for `wpa_supplicant`.

---

## Why this is feasible (the constraint)

The rendering/input stack is decoupled from "being a phone":

- **Graphics** is a runtime-selected backend (`minuitwrp/graphics.cpp`):
  MSM overlay → DRM (`/dev/dri/card0`) → fbdev (`/dev/graphics/fb0`). All are
  plain Linux interfaces; nothing in the fbdev/DRM paths is Android-HAL-specific.
  The backend vtable (`minuitwrp/graphics.h:23-37`) is tiny: `init/flip/blank/exit`.
- **Input** is pure evdev — `opendir("/dev/input")` + read `struct input_event`
  (`minuitwrp/events.cpp:380`). The GUI consumes events through
  `ev_get()` (`minuitwrp/include/minuitwrp/minui.h:98-101`).
- **No boot coupling for first paint**: `gr_init()` runs before any partition is
  mounted; brightness/haptics are optional sysfs writes that no-op if absent.

So: anything exposing a virtio-gpu DRM node + a `/dev/input/event*` renders the
GUI unmodified.

---

## Cuttlefish virtual device

Cuttlefish is AOSP's modern virtual device (host QEMU/crosvm + virtio). It's the
right era for `fox_16.0`, exposes a **DRM** node (matches `graphics_drm.cpp`),
builds its own `recovery`/`ramdisk`, and — crucially for WLAN work — provides an
**emulated radio via `mac80211_hwsim`**, so `wpa_supplicant` (`wlan.cpp`,
`wpa_ctrl_client.cpp`) runs against a virtual AP.

### Host setup (one-time)

Requires a Linux host with **KVM** (`egrep -c '(vmx|svm)' /proc/cpuinfo` > 0,
`/dev/kvm` present, user in `kvm`/`render` groups).

```
# android-cuttlefish host package (debs):
git clone https://github.com/google/android-cuttlefish
cd android-cuttlefish
tools/buildutils/build_packages.sh
sudo dpkg -i ./cuttlefish-base_*.deb ./cuttlefish-user_*.deb
sudo apt-get install -f
sudo usermod -aG kvm,cvdnetwork,render $USER   # re-login after
```

### Device variant

Build a Cuttlefish-flavoured target from the AOSP/Lineage tree with OrangeFox
flags. Start from `vsoc_x86_64` (fastest on a PC host) or `vsoc_arm64`:

- Set `OF_ENABLE_WLAN := 1` (and the other `OF_*` flags you want) in the device
  BoardConfig so `wlan.cpp` / `fox_webserver/` / secrets are compiled in.
- Ensure the **DRM** graphics backend is enabled for the recovery build
  (Cuttlefish has no fbdev; `graphics_drm.cpp` opens `/dev/dri/card0`). Confirm
  `HAS_DRM` is set for this target.
- Theme: keep `portrait_hdpi`; Cuttlefish's default panel is large/landscape, so
  expect to pass a resolution that suits the theme (see launch flags).

### Build & launch

```
source build/envsetup.sh
lunch aosp_cd_x86_64-userdebug     # or the OrangeFox-patched cuttlefish target
m -j                                # produces recovery/ramdisk + host package

# normal boot:
launch_cvd --daemon
# boot straight into recovery:
launch_cvd --daemon --boot_slot=recovery   # or stop_cvd + relaunch with recovery ramdisk
```

- View the screen via WebRTC (`https://localhost:8443`) or `cvd_internal_*`
  display; input is mouse/keyboard through that surface.
- `adb` attaches a few seconds after boot (`adb devices` → `0.0.0.0:6520`),
  which lets you exercise the fox engine and `fox` CLI over adb exactly as on a
  device.
- Tune `--x_res/--y_res/--dpi` to match the theme; tune `--cpus/--memory_mb` for
  speed.

### WLAN in Cuttlefish

Modern Cuttlefish wires `mac80211_hwsim` + a virtual AP, so `Wlan::Enable/Scan/
Connect` can drive a real `wpa_supplicant`. This is the only device-free path
that exercises the actual supplicant flow rather than faking it.

### Phases

1. **Stock boot**: stand up the host package, boot a stock `vsoc_x86_64` to
   confirm KVM/WebRTC/adb work end-to-end.
2. **OrangeFox recovery variant**: add the device target with `OF_ENABLE_WLAN`
   and DRM; boot OFRP recovery; confirm GUI renders and adb + fox engine work.
3. **WLAN bring-up**: validate scan/connect against the emulated radio; iterate
   on `wlan.cpp` / `wpa_ctrl_client.cpp` here instead of on hardware.

---

## Quick reference

| Want to iterate on… | Loop time |
|---|---|
| Partitions, flashing, boot env, fox FIFOs | full image |
| WLAN / `wpa_supplicant` | full image (hwsim radio) |

References: TWRP's own emulator device tree
(`github.com/TeamWin/android_device_emulator_twrp`, the original fbdev-in-QEMU
proof) and `github.com/google/android-cuttlefish`.
