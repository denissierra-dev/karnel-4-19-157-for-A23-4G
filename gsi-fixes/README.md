# Samsung Galaxy A23 4G (`SM-A235M` / `a23ub`) — GSI Runtime Fixes

Workarounds and recovery scripts for running **LineageOS 23.2 / Android 16 (PHH-based) GSIs** on top of the stock Samsung **Android 11 vendor (`RP1A` / VNDK 30)** and custom Knox-stripped `4.19.157-perf` kernel.

---

## Included Script

- [`patch_rw_system_gsi.sh`](./patch_rw_system_gsi.sh): Idempotent TWRP recovery script that patches `/system/bin/rw-system.sh` and `/system/bin/phh-on-data.sh` in-place (preserving inode, permissions `0755`, and SELinux context, and creating `/sdcard/*.bak` once).

### How to Apply in TWRP (e.g., after flashing a new GSI)

1. Push the script to your phone:
   ```sh
   adb push gsi-fixes/patch_rw_system_gsi.sh /sdcard/patch_rw_system_gsi.sh
   ```
2. Reboot to **TWRP**.
3. Go to **Mount**, check **System**, and make sure **Mount system partition read-only** is **unchecked**.
4. Go to **Advanced → Terminal** and run:
   ```sh
   sh /sdcard/patch_rw_system_gsi.sh
   ```
   Optional flags:
   - `--all` (default): applies all three fixes
   - `--c2-only`: applies only the Codec2/storage unlock fix
   - `--bright-only`: applies only the brightness HAL fix
   - `--sim-only`: applies only the Samsung RIL SIM UICC auto-recovery fix
5. Reboot to **System**.

---

## What Each Fix Solves

### 1. Internal Storage & SD Auto-Mount / User Unlock (`--c2-only`)
- **Symptom:** `/storage/emulated/0` fails to mount (`VolumeInfo{emulated;0}` stuck unmounted, `System unlocked users: []`), Camera/Gallery cannot save, Settings → Storage shows nothing.
- **Root Cause:** During boot, `StorageManagerService` queries `MediaCodecList` → `mediaserver32` → `samsung.software.media.c2@1.0-service`. On Android 16 GSI with VNDK v30 `libminijail.so`, Samsung's Android 11 `/vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy` traps (`SIGSYS`, `sig=31`) because modern bionic/GSI calls (`mremap` with `MREMAP_MAYMOVE`, `fsetxattr`, `setsockopt`, `sendmsg`, `recvfrom`, `uname`, `prlimit64`, `statx`, `clone3`, `rseq`, `membarrier`, scheduler/socket/epoll syscalls) are missing. `mediaserver` hangs waiting for Codec2, freezing `StorageManagerService` before it unlocks user `0`.
- **Fix:** Binds a patched copy at `/mnt/phh/sec_c2_base.policy` over `/vendor/etc/seccomp_policy/samsung.software.media.c2-base-policy` on boot in `rw-system.sh`, keeping `/vendor` untouched on disk.

### 2. Display Brightness Slider & Backlight Blackout (`--bright-only`)
- **Symptom:** With *Enable extended brightness range* off, the screen is ~9× dimmer up to ~83% and **turns completely black** near 84% (`256/306`); turning *Enable extended brightness range* on used to drop brightness to `0`.
- **Root Cause (two parts):**
  1. **GSI `rw-system.sh`:** Sets `persist.sys.qcom-brightness=306` from `panel0-backlight/max_brightness`. In `LightsService`, values `> 255` make AOSP pass a raw 24-bit integer (`0..306`, so `256 = 0x000100`, `R=0, G=1, B=0`) to Samsung's `SehLight` AIDL HAL, which computes `(77*R + 150*G + 29*B) >> 8`. At `256`, that equals `0`, shutting off the panel backlight IC (`ss_blic_ctrl(vdd, false)`). Setting `persist.sys.qcom-brightness -1` for `samsung/a23` switches `LightsService` to standard `(v0, v0, v0)` RGB (`0..255`).
  2. **Kernel (`commit a3c66848b`):** `ss_dsi_panel_sysfs.c` registered `/sys/class/backlight/panel` with `props = NULL`, reporting `max_brightness = 0` to `LightsService` (`mSamsungMaxBrightness = 0`). Passing `backlight_properties` and syncing `vdd->bl_dev->props.max_brightness` to `max_bl_level` (`306`) fixes **Treble Settings → Samsung features → Enable extended brightness range** for the full `0..306` hardware range.

### 3. SIM Stuck Disabled / `NOT_READY` After Toggling "Use SIM" Off (`--sim-only`)
- **Symptom:** Turning off the SIM toggle in Android Settings leaves the SIM stuck disabled (`NOT_READY`), unable to re-enable even across reboots.
- **Root Cause:** Turning off the toggle sends `ENABLE_UICC_APPLICATIONS false` to Samsung `rild`, which sets persistent property `persist.radio.uicc.enablement0=0`, while `SubscriptionManagerService` marks `simSlotIndex=-1`. Re-enabling sets `areUiccApplicationsEnabled=1`, but `GsmCdmaPhone` skips sending `ENABLE_UICC_APPLICATIONS true` back to `rild` because `simSlotIndex == -1`.
- **Fix:** Appends an `early-boot` guard to `/system/bin/phh-on-data.sh` (runs after `/data` persistent properties load and before `ril-daemon` starts) that resets `persist.radio.uicc.enablement0` from `0` to `1` on `samsung/a23`. *(Tip: use Airplane Mode instead of disabling the SIM slot toggle.)*

---

## Companion Kernel Commits (`main`)

- `a7898426d`: Import patched `SM-A235M` `4.19.157` kernel source (Knox-stripped + stack fix)
- `65646b215`: Disable `CONFIG_MODULE_SIG` for stock `wlan` module (`qca_cld3_wlan.ko`) + CI caching
- `e01c08b9a`: Allow loading stock vendor modules across Knox-stripped `MODVERSIONS` & preserve KABI offsets
- `a3c66848b`: `display: expose panel max_brightness on /sys/class/backlight/panel`
