# Android 17 (phase 4): kernel backport, S-Boot direct boot, storage layout

Status 2026-11-14: planning done, repos created, kernel backport starting. Full plan:
~/.claude/plans/meanwhile-while-the-work-zippy-biscuit.md (summarised here; this file wins).

## Decisions

- Kernel base: ACK **android17-6.18** (6.18 LTS, GKI support to 2030-07). Not 6.12.
- boot.img is loaded **directly by S-Boot** (no U-Boot on BOOT).
- **No PIT change.** super lives on SYSTEM (p19, 4,194,304,000 B).
- Alpine (driver tests) moves to **microSD**. RECOVERY holds our U-Boot as the dev/rescue slot:
  Alpine from SD, or `fastboot boot` of the RAM Linux for `a3-flash --via-linux`. TWRP leaves RECOVERY.

## Repos (GitHub org a3y17lte-mainline, created by the user 2026-11-14)

| Repo | Local path | Branch |
|---|---|---|
| kernel_samsung_exynos7870 | src/android/kernel/samsung/exynos7870 | lineage-24.0 |
| device_samsung_a3y17lte | src/android/device/samsung/a3y17lte | lineage-24.0 |
| vendor_samsung_a3y17lte | src/android/vendor/samsung/a3y17lte | lineage-24.0 |
| external_mesa (to be created) | src/android/external/mesa | lineage-24.0 |
| external_minigbm (to be created) | src/android/external/minigbm | lineage-24.0 |

- vendor_samsung_a3y17lte is **public by the user's decision** (2026-11-14): it carries the 4 firmware
  files of device/.../proprietary-files.txt (Wi-Fi/BT; the rest of work/firmware stays local).
- The kernel repo is a full single-branch clone of ACK (the GitHub repo started empty, so a
  blob:none clone could not be pushed; 4.25 GB, pushed in history steps under GitHub's 2 GB
  limit). Remotes: origin = ACK, torvalds = kernel.org (prerequisites), mainline = src/linux
  (fetch only), gh = GitHub. It never shares a worktree, .config or out dir with src/linux.
- Device tree: lineage.dependencies pulls the kernel and vendor. android-host-arm64's local
  manifest has all five: device, vendor, kernel, external/mesa, external/minigbm.
- Android tooling lives in tools/android/ and touches nothing the mainline work uses.

## Kernel: tools/android/backport-kernel (status 2026-11-14: builds clean, not booted yet)

The branch is generated, never edited by hand: ACK base -> prerequisites
(tools/android/kernel-prereqs.txt, "UPSTREAM:"/"BACKPORT:" prefixes; `exclude=` drops files the
base lacks) -> `git am -3` of the mainline series (format-patch of src/linux master..a3y17lte;
blobs come from the read-only `mainline` remote = src/linux) -> tools/android/kernel-patches/
("ANDROID:" commits). Conflict resolutions are kept by rerere in tools/android/rr-cache: a
regeneration replays them with no manual step (verified). After resolving a new conflict by
hand, `backport-kernel continue`.

- Prerequisites (50): Exynos7870 DTS (DSI, J5/J7, I2C cells, property style), the field_get()
  series, the *alloc_obj/flex helpers (ACK has kmalloc_obj itself), the mfd sec S2MPG10/11 rework +
  rtc-s5m alarm IRQ, the S2MU005 stack, ssp_sensors state, regmap REGCACHE_FLAT_S, the Samsung
  DSI panel helper header. Not taken: the S2MPG10/11 regulator chain, the 17 DRM bridge API
  commits (DECON/DSIM applied cleanly without them), hrtimer_start_range_ns_user().
- Hand resolutions (all in rr-cache): exynos-pmu kerneldoc + error path + Makefile/extern (6.18
  has no gs101 split), panel/LED Kconfig lists, ssp __packed, s3fwrn5 match table, gfp.h.
- ANDROID: commits: S-Boot model_info-* + made-up Wi-Fi/BT addresses in the DT; the defconfig;
  s2m-charger alarm_start_relative(); ssp sensors 6.18 APIs (mod_devicetable.h,
  iio_device_claim_buffer_mode()).
- Defconfig: tools/android/mkconfig = work/config-a3y17lte + Android 17 requirements
  (kernel/configs c/android-6.12, the newest published) + a3y17lte-android.config. 4 requirements
  unmet on purpose (ASHMEM Rust-only, SCHED_DEBUG/DCCP removed, CFI_CLANG is CFI).
  Image 22.3 MiB after dropping KVM (S-Boot enters at EL1), btrfs/xfs/udf/squashfs, Ethernet.
- tools/android/build-kernel = compile check only. The ROM builds the kernel from source
  (never a prebuilt). Open: LineageOS kernel.mk hard-codes linux-x86 host paths (sysroot,
  kernel-build-tools): needs a patch in android-host-arm64 for the arm64 build host.
- Optional later: 65c132fa8c93 (dw_mmc DMA threshold, needs adapting to the pre-refactor struct).

## S-Boot direct boot (facts from TWRP's image + sboot.bin strings; not yet tested with our kernel)

- Boot image header v0, page 2048, raw arm64 Image (S-Boot cannot decompress), gzip ramdisk,
  DTBH v2 dt.img chosen by model_info-chip (7870) / hw_rev (2) / hw_rev_end, optional
  SEANDROIDENFORCE trailer. No AVB, dtbo, vendor_boot or bootconfig support.
- S-Boot rewrites /chosen bootargs + initrd range and /memory (needs both nodes) and adds its own
  androidboot.* (androidboot.hardware=samsungexynos7870).
- BOOT is 32 MiB: kernel Image must stay well under ~30 MB (slim a3y17lte_defconfig, the rest as
  modules). Our parameters go into a bootconfig appended to the ramdisk (BOOT_CONFIG_FORCE), e.g.
  androidboot.super_partition=SYSTEM.
- First test: a direct-boot image on RECOVERY (key combo), not BOOT.
- sboot has a "Set WP for SYSTEM" string: re-check SYSTEM is writable after a direct boot.

## Storage layout

BOOT boot.img; RECOVERY U-Boot (later Lineage recovery); SYSTEM super (system, system_ext,
product, vendor as EROFS, one group of 4000 MiB - 4 MiB, non-A/B); CACHE /cache; HIDDEN /metadata
(needs the user's yes); USERDATA /data f2fs + FBE. EFS is not mounted.

## Graphics (decided 2026-11-14: our Mesa)

Mesa Panfrost (GLES 3.1; no Vulkan on Midgard: panvk does not support it) + minigbm +
drm_hwcomposer. The Mali blob HALs of the community trees are not usable, and AOSP/upstream Mesa
is not either: it lacks our Midgard compiler fixes and the renderonly fix.
- AOSP 17 external/mesa3d (meson2hermetic Android.bp) builds only gfxstream: left alone (other
  projects reference its modules). Our Mesa is a second tree, `external/mesa`
  (a3y17lte-mainline/external_mesa, full upstream history, branch lineage-24.0 = the a3-26.2.6
  series, same patches as the pmOS fork). It builds with upstream's android/Android.mk
  (BOARD_MESA3D_USES_MESON_BUILD, runs host meson with the Android clang): libEGL_mesa,
  libGLESv1_CM_mesa, libGLESv2_mesa, libgallium_dri. Module names do not clash with mesa3d's.
- Panfrost precompiles CL kernels (libpan) with mesa_clc + vtn_bindgen2 + panfrost_compile,
  which need LLVM + SPIRV-LLVM-Translator: android-host-arm64 setup.sh builds them natively from
  external/mesa into /usr/local/bin, the device passes -Dmesa-clc=system -Dprecomp-compiler=system.
  The same options build here in 48 s (Fedora, LLVM 23; libclc not needed).
- minigbm: our fork a3y17lte-mainline/external_minigbm replaces AOSP's (remove-project in the
  local manifest): one commit adds `exynos` to the dumb-buffer drivers (render node, WC
  non-contiguous GEM through the IOMMU; DECON scans out, Panfrost imports by PRIME). Small enough
  to send to ChromiumOS minigbm. Mesa picks the GPU with drm.gpu.vendor_name=panfrost.
- Kernel: DMABUF_HEAPS + DMABUF_HEAPS_SYSTEM added (minigbm, Codec2).
- sphal libraries (Mesa, mapper.minigbm, libdrm, libui, libgralloctypes, mapper@4.0) labelled
  same_process_hal_file: VERIFY the list with readelf -d after the first build.

## HALs (checked against android-17.0.0_r1 / lineage-24.0)

Real: health example (power_supply), usb example (/sys/class/typec), audio AIDL example (stub
+ r_submix until the codec), bluetooth default (HCI user channel via mgmt), wifi, LineageOS
light (scans /sys/class/backlight), keymint/gatekeeper .nonsecure. No-op: power example.
Gadget example hard-codes 11110000.dwc3: not used, init.usb.configfs.rc path instead.
hardware/samsung (lineage-24.0) has no dtbhtool any more: DTBH is built by the device packer.

Ours, in device/samsung/a3y17lte/aidl/ (written 2026-11-14, compile-checked against mock
headers only, not built in the tree yet):
- vibrator (IVibrator V4): regulator-haptic is an input FF device (FF_RUMBLE) at one voltage
  (LDO32 3.325 V min = max), so on/off only. One endless effect (replay.length 0: ff-memless
  plays until stopped; closing the fd erases it), timing by the HAL. TICK 10, CLICK 20,
  HEAVY_CLICK 30 ms, DOUBLE_CLICK 20 + 100 + 20 ms, strength scales 0.75/1/1.25: tune by feel.
- thermal (IThermal V3): zones by type. soc-thermal = CPU, GPU and SKIN (virtual skin sensor:
  ThermalManagerService uses SKIN only); battery-thermal = BATTERY. Cooling devices
  cpufreq-cpu0/4 (CPU), devfreq-11400000.gpu (GPU).

| Sensor | LIGHT | MODERATE | SEVERE | CRITICAL | EMERGENCY | SHUTDOWN |
|---|---|---|---|---|---|---|
| CPU/GPU (soc) | | | 76 (kernel passive) | | | 110 (kernel critical) |
| SKIN (soc, starting values) | 60 | 66 | 72 | 76 | 95 | 105 |
| BATTERY | | | 50 | | | 60 |

  2 C hysteresis; polling 5 s, 1 s once anything is above NONE. Test: emul_temp on soc-thermal.
