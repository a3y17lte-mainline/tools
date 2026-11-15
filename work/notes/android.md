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
| external_mesa | src/android/external/mesa | lineage-24.0 |
| external_minigbm | src/android/external/minigbm | lineage-24.0 |

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

- Series: 136 commits of src/linux master..a3y17lte (regenerated 2026-11-14 with the audio
  commits and pin-switches, 190 steps; the card2 pin-switches commit needed the 6.18 helper
  signatures (card, goto err): resolution in rr-cache).
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
- Regenerated 2026-11-15 (219 steps, tip 3fcf1690d804): series of 160 incl. bus DVFS; prereqs
  + 4fc945d8fdfc/752925401476 (I2S, sound card). The reboot mode (dtsi + PMU binding), MUIC
  D+/D- routing (folded into "extcon: s2m: Add S2MU004 support") and fuel gauge presence/temp
  (binding + driver, DTS folded into "Add MUIC, battery and thermistor") are mainline commits
  now (tracker groups BL, BM, Z); the ANDROID: set is 0001-0008. Manual steps a regeneration
  will hit again: "clk: samsung: exynos7870: Support CMUs in a power domain" (git am cannot
  build its 3-way base: apply with --reject, add #include <linux/pm_runtime.h> by hand) and
  "dt-bindings: soc: samsung: exynos-pmu: Allow the normal reboot mode" (empty on 6.18, which
  lacks the mode restriction: checkout --ours, git am --skip, continue). exynos-bus conflict is
  in rr-cache.
- ANDROID: 0008 C ashmem (android15-6.6 driver ported: shrinker_alloc, mm_get_unmapped_area,
  ashmem_area_*() and ashmem_memfd_ioctl() like the Rust driver); CONFIG_ASHMEM=y builds it
  when RUST is off. W=1 clean. Not run on the phone yet. sys.use_memfd dropped with it
  (device bbcef86): libcutils uses ashmem, its default at vendor API 23. If ashmem fails on
  the phone: setprop sys.use_memfd true + setprop ctl.restart zygote recovers it live.
- Optional later: 65c132fa8c93 (dw_mmc DMA threshold, needs adapting to the pre-refactor struct).

## S-Boot direct boot (facts from TWRP's image + sboot.bin strings; not yet tested with our kernel)

- First recovery boots (2026-11-15): LineageOS recovery runs (display, touch, keys, battery
  level). Found on the way: CONFIG_LSM had no selinux (first-stage init aborts: no selinuxfs);
  EXT4_FS_SECURITY missing (ext4 mounts fail with EOPNOTSUPP under SELinux: /cache);
  a plain reboot returned to recovery: S-Boot follows PMU INFORM3, the vendor kernel writes
  0x12345670 there on every normal reboot -> mode-normal in the reboot-mode node (ANDROID:
  patch 0006 for now; TODO the same in the mainline series, it is an exynos7870.dtsi fix).
  Not done: vendor also writes INFORM2 = 0x12345678 on reboot ("LPM mode prevention": no
  charging-mode boot when a cable is in).
- Reboot mode, complete picture (S-Boot s5p_check_reboot_mode, 0x6de08): S-Boot keeps
  REBOOT_MODE in its env (PARAM) and only changes it when INFORM3 = 0x1234567x at the next
  boot. Android passes its reason as the restart command ("userrequested,recovery"), which is
  no mode -> ANDROID: patch 0007 writes "normal" for unknown commands; 0008 writes "normal"
  when the driver registers, so a watchdog reset / hang in recovery boots BOOT (Alpine) again
  (the user's request; confirm on the phone that S-Boot honours it after a watchdog reset).
  "Reboot to bootloader" (mode-bootloader 0x1234567d) boots BOOT normally.
- No adb from the recovery (2026-11-15): everything was set up (adbd, gadget bound to
  13600000.usb, typec device/sink, dwc3 mode device, DCTL run) but UDC "not attached", DSTS
  link = suspend. Cause: USB D+/D- go through the S2MU004 MUIC switches, which S-Boot leaves
  open on a normal boot; our U-Boot closes them (a3y17lte_muic_route_usb), which is why Alpine
  always worked. Values U-Boot leaves: CTRL1 0xc7 = 0x12 (bit4 set, bit2 clear), SWCTRL 0xca =
  0x24. ANDROID: patch 0009 does the same in extcon-s2m's S2MU004 init. TODO: fold into the
  mainline S2MU004 extcon commit (group with the MUIC), it is needed without U-Boot.
- Sanity check before the first full build (2026-11-15), fixed: nonsecure KeyMint binary had
  no SELinux label outside its APEX (init starts no service without a domain: no FBE, no
  boot); CONFIG_UNICODE was off (f2fs casefold, which emulated_storage.mk makes vold format
  /data with). Checked and fine: HAL versions against compatibility_matrix.202604 (all in
  range), FBE speed (CPU has aes/pmull/sha2, CRYPTO_AES_ARM64_CE_BLK=y: keep encryption),
  module names (the recovery build's kati + soong pass covered them).
  First full build (2026-11-15, 2h15): everything built, ota_from_target_files failed ("META/
  ab_partitions.txt is required for ab_update"): Android 17's board_config.mk turns an unset
  AB_OTA_UPDATER into true. BoardConfig now sets it false (device 9102785); the images of that
  build were A/B-flavoured (ro.build.ab_update, update_engine, super metadata): rebuild.
  Android may rewrite RECOVERY from boot (install-recovery): fine by the user (2026-11-15),
  RECOVERY holds Lineage recovery anyway.
  First boot (2026-11-15): bootloop, init rebooted "bootloader,bootstrap-apexd-failed"; cause not
  seen (S-Boot's loglevel=4 is an early param, our bootconfig loglevel never applied: now a
  kernel.sysctl.kernel.printk bootconfig entry + printk.devkmsg=on). The installed BOOT was a
  plain mkbootimg image (no DTBH, bootconfig, SEANDROID): the OTA tools repack boot.img unless
  BOARD_CUSTOM_BOOTIMG := true puts ours into target-files BOOTABLE_IMAGES/ (device 0a23a82).
  Kernel: CPUSETS_V1 was off ("Unknown subsys name 'cpuset'"), added with FUSE_BPF, incfs,
  ANON_VMA_NAME, MGLRU, NLS_UTF8 (from a gki_defconfig diff). BOOT is mmcblk0p10.
  Second full build (2026-11-15): BOOTS (sys.boot_completed=1, all our HALs running). Fixed
  after it: system_server "Failed to create ashmem" (no /dev/ashmem; libcutils uses memfd by
  itself only for ro.vendor.api_level >= 202604, ours is 23) -> sys.use_memfd=true on product
  (vendor may not set use_memfd_prop); lmkd exiting ("Old kill strategy can only be used with
  v1 cgroup hierarchy") -> dropped ro.lmk.use_minfree_levels; Bluetooth abort on LE_ADV_FILTER
  0xfd57 -> bluetooth.core.le.vendor_capabilities.enabled=false. Device d6a5882..058d81e.
  adb root needs the LineageOS developer option; as shell, setprop works while permissive.
  Display + Mesa + HWUI + HWC render fine (screenshot of the ANR dialog). NFC: first NfcService
  brought the chip up (NCI_Version 16), was ANR-killed during the busy first boot, and every
  restart hung in startup after the routing tables (HAL not deadlocked: binder dump answered,
  no worker thread) -> the persistent app froze the phone; shell cannot stop a persistent
  system app (disable-user ignored, uninstall/adbroot need root/system). NFC is gated off in
  device.mk (A3_ENABLE_NFC) until debugged with Rooted debugging + debuggerd -j.
  zram: swapon_all failed on max_comp_streams (no such sysfs file on 6.x): option dropped.
- Third build (2026-11-15): boots in ~1 min, usable ("okay, not great"). CPU/GPU/DRAM clocks OK
  (MIF pinned at 902 MHz without bus DVFS). All layers are CLIENT (GPU) composition: mainline
  exynos7 DECON exposes 2 windows (primary + CURSOR, no blend/alpha props, immutable zpos);
  the 7870 DECON-INT has 3 (stock max_win = 3; vendor WINCHMAP0 / VIDW_ADD0 / BLENDE). Plan:
  3 windows, overlays, pixel blend mode + alpha, mutable zpos (mainline first). JANK_COMPOSER
  frames of 50-100 ms: measure GPU composition vs the command-mode commit first.
  Open issues found that day:
  - Bluetooth showed no devices: the stack's main thread sat in AServiceManager_waitForService
    for IBluetoothAudioProviderFactory/default (Hearing Aid init). bluetooth.audio-impl's VINTF
    fragment declares it, but only the old combined audio service called
    createIBluetoothAudioProviderFactory(). Fixed: our audio service calls it (device cacd142).
    Not yet: an A2DP output path (bluetooth module in the audio HAL + policy config).
  - Audio dead, no sound card: "samsung-i2s: Failed to add route Secondary Playback(*) ->
    Playback Mixer", card -ENODEV. ACK 6.18 lacks 4fc945d8fdfc (no secondary DAI on i2sv7)
    and 752925401476 (op_clk vs runtime PM): added to kernel-prereqs.txt (both cherry-pick
    cleanly on the 6.18 base); needs a backport-kernel regeneration.
  - Browser gray: the page loads, the WebView renderer never draws. Renderer log: "ashmem:
    readlink(/proc/self/fd/N) failed: Operation not permitted" + "Unable to stat ashmem device":
    libcutils identifies memfds by readlink, Chromium's renderer sandbox denies it, and the
    fallback needs /dev/ashmem. sys.use_memfd alone is not enough: Android still expects
    /dev/ashmem. ACK 6.18 has only the Rust ashmem (ASHMEM depends on RUST; android16-6.12
    too, the C driver ends at android15-6.6). Options: CONFIG_RUST in our kernel build (needs
    rustc + bindgen + rust-src for the arm64 build host in LOS kernel.mk) or carry the
    android15-6.6 C ashmem as an ANDROID: patch (port to the 6.18 shrinker API). With
    /dev/ashmem present, sys.use_memfd may be droppable again.
  Open for the first boot: HAL sources are only syntax-checked here; drm_hwcomposer with the
  command-mode panel; HIDDEN/CACHE/USERDATA are formatted by the first boot (formattable).
- Fourth build (2026-11-15, regenerated kernel + C ashmem): Bluetooth scans, the browser
  draws (ashmem), no boot cursor, sound card registered. Bus DVFS works: MIF follows the
  fastest cluster (275-902 MHz, many transitions); INT/DISP stay at 133 MHz (powersave, as
  stock); the power HAL boost ends correctly (min back to 343 MHz).
  Sound was still dead, but not in the audio path: Codec2 picked HIDL (media.c2.hal.selection
  defaults to hidl below ro.vendor.api_level 202404). Without hwservicemanager that finds no
  store, and media.swcodec registered its AIDL store with no components. Only
  c2.android.inproc.aac.decoder (ApexCodecs) was left, so "Failed to create audio/vorbis
  decoder" and AudioFlinger "Frames written: 0". Fixed with media.c2.hal.selection=aidl in
  vendor.prop (device 66c9790); tested live (setprop as root, restart media.swcodec,
  mediaserver and zygote; the codec list is built by the lazy media.codeclist.generator).
  Sound confirmed by the user.
  Next, planned (stashed plan): our own light HAL for the touch-key LED (the LineageOS HAL
  only matches button-backlight*, so dumpsys lights is empty and the keys stay lit), and 3
  DECON windows with blending (composition efficiency 0.0025: everything goes through the
  GPU, even status bar + app).
- Fuel gauge (ANDROID: patches 0010/0011, for the mainline series too): PRESENT = 1, TEMP from
  the thermistor IIO channel ("batt-temp"), TECHNOLOGY + design values through
  monitored-battery (power supply core). Not run on the phone yet.
- Debug technique that worked: a second cpio (xz) appended to the ramdisk with a replacement
  init.recovery rc + script, writing to /dev/pmsg0 (pstore pmsg, read on Alpine as
  /sys/fs/pstore/pmsg-ramoops-0): one "STEP name" line before each read. The console buffer
  (16 KiB) is useless while recovery's battery polling floods it with avc denials (fixed by
  sepolicy/vendor/recovery.te). The recovery ramdisk has sh, toybox, getevent, no awk.
- CACHE holds pmOS_boot: the recovery mounts it rw as /cache at start and exit; a cache wipe
  or factory reset there erases it.
- S-Boot size limits (disassembly of the boot-image loader in mmcblk0boot0, 0x5cda0-0x5ce1c;
  hit 2026-11-15 with the first LineageOS recovery, "Invalid RAMDISK LENGTH"): page-aligned
  ramdisk <= 0xe00000 (14 MiB); kernel + ramdisk + 0x1000 <= 0x3d7ff00, the same again with
  the dt added. So BOARD_RAMDISK_USE_XZ (recovery ramdisk 15.1 MB gzip -> 8.9 MB xz, kernel
  RD_XZ=y); mkbootimg_samsung.py refuses images over the limits.
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

Decided 2026-11-15: pmOS is dropped (no image backups of pmOS or Alpine). HIDDEN (stock
/preload, 60 MiB, copy in backups/20261108-*) becomes /metadata (fstab formattable). Alpine for
driver tests: U-Boot swapped onto RECOVERY with a3-flash when needed, Alpine on the microSD.
First install: new recovery via a3-flash -> Format data -> sideload of the full zip from the
recovery (run by the user). The LOS updater (bootable/deprecated-ota, UpdateDynamicPartitions)
creates super from the zip's unsparse_super_empty.img when SYSTEM has no LP metadata.

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

Real: health example (power_supply), usb example (/sys/class/typec), bluetooth default (HCI user channel via mgmt), wifi, LineageOS
light (scans /sys/class/backlight), keymint/gatekeeper .nonsecure.
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
- audio (2026-11-14, aidl/audio): the AOSP example services are APEX-only and their primary
  module is the emulator's (StreamPrimary paces and skips buffers). Ours links
  libaudioserviceexampleimpl (Config, r_submix, usb) with its own "default" module: StreamAlsa
  streams (like USB) on card "a3y17lte" PCM 0 (found via /proc/asound/a3y17lte), 48 kHz S16
  stereo only; connected devices pick a libaudioroute path (mixer_paths.xml: speaker, earpiece,
  headphones, main-mic MIC1, second-mic MIC2, headset-mic MIC3). Control names and enum items
  checked against the phone's card. Jack: ASoC input device "a3y17lte Headset"
  (config_useDevInputEventForAudioJack). The loudspeaker's "Speaker Switch" is a card pin switch
  (mainline group BD: audio-graph-card2 parses pin-switches; tested on Alpine). Open:
  "Mono Mix" for the earpiece is (L+R)/2 (verify by ear).
- power (2026-11-14, aidl/power, IPower V7): cpufreq policy limits of policy0/4 (init rc chowns
  scaling_min/max_freq to system). INTERACTION boost: min = first OPP >= 1.0 GHz (1014000) for
  durationMs (default 100 ms, max 2 s). LAUNCH: min = max OPP, 5 s cap. LOW_POWER: max =
  1014000. Boosts ignored when not interactive / device idle / low power. No hint sessions
  (everything ADPF returns EX_UNSUPPORTED_OPERATION).
- effects (2026-11-14, aidl/audio-effect): AOSP's EffectFactory.cpp + EffectMain.cpp copied
  unchanged (the AOSP binary is installable:false, no filegroup); outside an APEX they fall back
  to /vendor/etc/audio_effects_config.xml and /vendor/lib64/soundfx. Libraries: bundle, downmix,
  dynamics processing, loudness enhancer, pre-processing, reverb, visualizer. VERIFY at the
  first build that these library modules install to /vendor outside the APEX.
- sensors (2026-11-14, aidl/sensors, ISensors V3): Sensors.h/.cpp = AOSP default service (FMQ,
  wake lock queue; libsensorsexampleimpl is not visible to devices), Sensor.h/.cpp ours.
  - IIO devices by name (ssp-accelerometer, -gyroscope, -magnetometer, -pressure, -light,
    -proximity). Buffer sensors: all scan elements on (the drivers push whole samples), layout
    from scan_elements/*_type + *_index, scale from sysfs, rate through the sampling_frequency
    attribute (10..200 ms). current_timestamp_clock is set to boottime; if that fails the
    arrival time is used.
  - Units: accel/gyro as IIO; mag Gauss x 100 = uT; pressure kPa x 10 = hPa; light lux,
    on-change. Mag status is ACCURACY_MEDIUM (the hub does not pass its accuracy on).
  - Proximity: IIO event fd (rising = near 0 cm, falling = far 5 cm), wake-up + on-change; the
    kernel sends the current state when the events are switched on.
  - Tested 2026-11-14 on Alpine with a harness around Sensor.cpp (static aarch64 build in a
    scratch Alpine root): accel z 9.6 m/s2 at 21 ms and 100 ms periods, gyro, mag |B| ~95 uT
    (uncalibrated after boot), pressure 992.5 hPa, light, prox far at enable; boottime
    timestamps; no events after disable. The barometer delivers every 180 ms whatever is set
    (minDelay 180 ms).
  - ueventd lists every attribute the HAL writes (the attribute name is literal).
  - Not done: step counter, significant motion, tilt/pickup (the hub has them, the kernel
    driver does not); uncalibrated variants; whether the proximity interrupt wakes the phone
    from suspend on Android.
- NFC (2026-11-14, aidl/nfc, INfc V2): libnfc-nci needs raw NCI, so the Android kernel has
  ANDROID: "nfc: s3fwrn5: Add a raw NCI character device" (tools/android/kernel-patches/0005,
  CONFIG_NFC_S3FWRN5_RAW=y): the chip is not registered with the NCI core; /dev/s3fwrn5 gives
  one NCI packet per read/write, one opener, ioctl S3FWRN5_RAW_IOC_POWER (0 off, 1 power cycle
  into NCI mode), off on close. No firmware mode from userspace.
  - HAL: reader thread does power on + FW_CFG `2F 28 01 12` (wait for `4F 28 01 00`, one
    retry with a power cycle), then OPEN_CPLT; all events and data come from that thread.
  - getConfig: nfaProprietaryCfg from stock (15693 = 0x80, MIFARE = 0x81, B' 0x81, dual 0x82,
    Kovio 0x8a, polls 0x70/0x74, listen 0xf4); stock's extra byte 0x90 has no AIDL field
    (protocolChineseId left 0xff). All routes to the host; libnfc-nci.conf in /vendor/etc with
    no EEs. Features: nfc, nfc.hce, com.nxp.mifare.
  - NOT tested on hardware: the kernel patch is compile-tested only (Alpine runs the mainline
    kernel, which keeps the NCI core path). HCE is new ground: drop the feature if it fails.
  - The wake (FIRM) line stays asserted while NFC is on, as with the kernel NCI stack; stock
    toggles it for sleep. Measure the idle drain.
- SELinux labels (2026-11-14, no rules): contexts for the new services, /dev/s3fwrn5 =
  nfc_device; genfs: sensor hub SPI path (sysfs_a3_sensors), GPU/bus devfreq
  (sysfs_a3_devfreq), wlan0 (sysfs_net), gpio-keys and hall wakeup (sysfs_wakeup). Open: nodes
  behind an I2C adapter (charger/fuel gauge/TCPC/RTC wakeup sources, typec, extcon) have i2c-N
  in the path, which follows probe order: needs i2c aliases in the DT first. To finish after
  the bus DVFS backport (the user's order, 2026-11-14): those nodes, plus the devfreq paths
  re-read from the final series (bus-mif).
- Gatekeeper is installed as the vendor APEX com.android.hardware.gatekeeper.nonsecure (the
  service binary is installable:false); KeyMint's nonsecure binary installs on its own.
- Bus DVFS (INT/DISP exynos-bus, MIF later): nothing needed in Android userspace. When the
  mainline series is final: backport-kernel, check ARM_EXYNOS_BUS_DEVFREQ + passive governor in
  a3y17lte_defconfig, check trans_stat under schedutil.
