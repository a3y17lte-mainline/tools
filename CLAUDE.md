# Exynos 7870 mainline bring-up: Galaxy A3 2017 (SM-A320F, a3y17lte)

## Start here (fresh chat orientation, as of 2026-11-11)

Goal: mainline Linux on the Galaxy A3 2017 (Exynos 7870), then Android 17 on that kernel, upstreaming
everything along the way. The A3 is a J5 2017 (already upstream in 7.4-rc1) with four different parts:
S2MU004 charger + USB-C, QCA9377 Wi-Fi/BT, STM32 sensor hub on SPI, command-mode S6E3AA2 panel.

Read these first:
1. This file: the safety rules below are hard rules.
2. Bring-up plan, a Claude Docs doc: https://claude.ai/artifact/Dx9Q6oYkN9K4wBfGKzKZUe. It has the
   hardware delta table (A3 vs J5 vs mainline), phases, the known mainline gaps, the security model,
   risks and sources. To read it, load `mcp__claude_ai_Claude_Docs__read` via ToolSearch and call it with
   ref {object: project, id: 68e0bf34-4bb3-416e-8e67-c58359edbe23}, then read the tab's root node. The
   tab output is ~55 KB of XML and gets saved to a file: strip the tags with a short python script.
   Its "Phase" status lines lag behind this file; this file wins.
3. Upstream tracker, an HTML artifact with a db: https://claude.ai/artifact/RQq4vqHXBeHm8Kuqsrye3r. It
   holds 15 patch groups for the 35 local kernel commits (none sent yet). Read it with ArtifactData:
   `list groups`, `list meta`.
   - Wave 1: A board DTS, B watchdog, C IST3038H start command, D TMU, E MCT/early clocks, K S2MU004,
     M A96T316 touch keys.
   - Wave 2: F CPU clocks, G cpuidle PMU, H ASV.
   - RFC: D′ thermal IRQ type, I cpufreq-dt boot OPP (ready; patch in out/upstream/).
   - Wave 3: J DVFS DT (needs A, F, H), L board fuel gauge/charger (needs A, K),
     N board hall + touch keys (needs A, M).
   - `meta/prep` is the user's pre-send checklist: their own Signed-off-by, an AI tool-use note per cover
     letter, b4/send-email, rebase, dt_binding_check.
4. Rig spec, a Claude Docs doc: https://claude.ai/artifact/Waw3bXZYPkFAiDc1SKqyrS (id
   ef95df2e-cfef-43d9-b6df-90616edcc3d3). This is a PLAN ONLY: no rig hardware exists, the user has just
   the phone. Parts are superseded:
   - Its boot.img + heimdall loop was replaced by U-Boot + fastboot + eMMC autoboot.
   - Its 172.16.42.1 address is forbidden here (see USB networking).
   - Its `rig` CLI does not exist; use the tools/ below.
5. `work/notes/` holds detailed register notes (s2mu004-regs.md), and `work/firmware/README` lists the blobs.

Working (all on v7.4-rc2 + branch a3y17lte; rebased 2026-11-09, old tip tagged a3y17lte-pre-rc2):
- Boot: autoboot from eMMC through our U-Boot. Watchdog, reboot and poweroff work.
- Connectivity: USB NCM + ssh, Wi-Fi (ath10k_sdio), BT (hci_qca).
- Storage: eMMC, microSD.
- Input: touch, keys, capacitive touch keys + LEDs, hall (lid), vibrator.
- Display (2026-11-09): Exynos DRM (DECON command mode + DSIM) + our S6E3AA2 panel driver, fbcon,
  256 calibrated brightness levels, blank/unblank. See the Display section.
- Power: S2MU004 fuel gauge + charger (watchdog kick), thermal, cpufreq 343–1586 MHz with per-chip
  ASV voltages, and cpuidle (core power-down; cluster power-down not done).
- GPU (2026-11-11): Panfrost on Mali-T830 with DVFS 343–1001 MHz (worst-case voltages, GPU cooling),
  patched Mesa + GTK on the installed postmarketOS (Phosh). See the GPU section.

Not done, roughly in planned order:
- DISPLAY RE-ENABLE FIX, awaiting confirmation (2026-11-12): intermittent "exynos-dsi: PLL failed
  to stabilize" on unblank after long idle (log work/notes/dsi-pll-unblank-dmesg-20261111.txt).
  Cause: samsung-dsim polled the PLL stable bit 3000 times without delay, while the 7870 lock timer
  is 80000 OSC cycles (~3 ms); the boot enable is unaffected (bootloader left the PLL locked).
  Fixed in kernel 02911a285c6d (poll by time, 20 ms) + 1f0792e4d555 (init returns the error),
  installed on pmOS as linux-samsung-a3y17lte 7.4_p20261112184959-r2. Not reproducible on demand
  (fails after long idle): the user watches daily use. Groups P/Q/R stay held until confirmed;
  the two DSIM commits belong in P. Blank/unblank by hand: busctl --user set-property
  org.gnome.Mutter.DisplayConfig /org/gnome/Mutter/DisplayConfig org.gnome.Mutter.DisplayConfig
  PowerSaveMode i 3 (0 = on), with DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/10000/bus.
- U-Boot paths not tested yet: the Volume Down menu, "Boot Previous Kernel" and the fallback to
  fastboot (the user considers the boot loop finished).
- postmarketOS (device package in testing, Phosh) is installed and in daily use on the phone
  (2026-11-11); see the GPU and postmarketOS section. Phase 2 closed 2026-11-09.
- Then Phase 3, after the panel: the TCPM USB-C/PD driver at i2c0 0x3c (host/OTG, role switching) and
  S2MU004 "proper charging" (MUIC charger detection, higher input current: a charger write, needs the
  user's approval).
- Suspend: needs the PMU sleep sequences from the vendor kernel.
- SPI port config + STM32 sensor hub → IIO (large).
- PDMA, ADC (battery thermistor), PWM.
- Audio COD3026X (shared with every 7870 phone).
- GPU follow-ups: per-chip GPU voltages (exynos-asv G3D), sending the Mesa/GTK patches (see GPU section).
- Android 17 (phase 4). Stretch goals: modem (Shannon 315; remoteproc + mailbox + WWAN) and cameras.
- Upstreaming: wave 1 is ready to prepare once the user wants to send.

Working style (user preferences, also in memory): short single-step phone commands, no chained wait
loops. Trust what the user reports seeing on the phone. For low-risk boot changes, build and flash BOOT
directly instead of staged test ceremonies. The user puts the phone into download mode themselves.

## Phone safety rules (hard)

- Write to the phone ONLY with `tools/a3-flash <PARTITION> <img>`. It allows BOOT and RECOVERY
  (SYSTEM/CACHE/USERDATA need `--i-mean-it`), checks boot-image magic and PIT sizes, refuses to
  run before a verified backup exists, and logs to `out/flash-log/flash-log.jsonl`. BOOT/RECOVERY go
  through Heimdall (download mode); SYSTEM/CACHE/USERDATA only with `--via-linux` (see Storage).
- Every `a3-flash` call needs the user's approval (settings `ask` rule). Never try to avoid that.
- Never touch: BOOTLOADER (sboot, in mmcblk0boot0), CM (mmcblk0boot1), PARAM, EFS, CPEFS,
  m9kefs1-3, CARRIER, RADIO, PERSISTENT, STEADY, PIT. No repartitioning, ever.
- Raw `heimdall flash`, fastboot writes, adb sideload, and dd/redirects/mkfs to phone block
  devices are blocked by `tools/guard-phone-writes` (a PreToolUse hook in ~/.claude/settings.json).
  If it blocks you, stop and ask the user. Do not rephrase the command to get past it.
- Read-only is fine: `heimdall detect|print-pit|download-pit`, `adb exec-out dd if=...`, `a3-backup`.
- After a heimdall action with `--no-reboot`, the next heimdall action needs `--resume`.

## USB networking

- NEVER use the pmOS default 172.16.42.0/24: it is the user's home LAN/gateway.
- Phone (USB gadget, NCM) = 10.78.70.1/24, PC = 10.78.70.2. Any initramfs/config we build must use this.

## Commits

- Author: Martin Štrobl <recordyletsplays@gmail.com>. No Co-Authored-By or other AI attribution, ever.
- Kernel patches: no Signed-off-by from the agent; the user adds their own before sending.
- One logical change per commit (bindings separate from DTS), checkpatch + dtbs_check clean.

## Layout

- GitHub org a3y17lte-mainline (ssh key ~/.ssh/id_ed25519, account Sap1k). Remote name `gh` in each
  repo; `origin` stays the upstream (kernel.org; U-Boot: git.u-boot-project.org, whose dev branch is
  now `main`, `master` froze at v2026.07):
  - tools: git@github.com:a3y17lte-mainline/tools.git (this folder, branch main)
  - linux: git@github.com:a3y17lte-mainline/linux.git (fork of torvalds/linux, branch a3y17lte)
  - u-boot: git@github.com:a3y17lte-mainline/u-boot.git (fork of u-boot/u-boot, branch a3y17lte on upstream main,
    rebased 2026-11-09 onto 934995f642 = v2027.01-rc1+; old tip tagged a3y17lte-pre-rebase-20261109)
  A rebased series is a force push: use `git push --force-with-lease gh a3y17lte`.
  src/ trees are blob:none partial clones: only push to repos that already have the upstream
  history (the forks), never to an empty repo.
- The project folder is a git repo (branch main, pushed to the tools repo above) tracking CLAUDE.md, tools/,
  work/notes/ and work/config-a3y17lte through an allowlist .gitignore. Commit changes to tools and notes
  there. New top-level paths stay untracked until they're added to the allowlist; never allowlist
  backups/, firmware/, work/firmware/, work/system/ or work/ssh/.

- `src/` mainline linux, pmaports, pmbootstrap, vendor kernel (lineage-24.0), vendor device tree, u-boot
- `backups/` eMMC backups (mode 700, contain IMEI/EFS: never upload anywhere). PIT: `backups/pit/a3y17lte.pit`
- `firmware/twrp/` TWRP 3.7.0_9-0 a3y17lte (GPG-verified, TeamWin key 95707D42…891A43DF)
- `tools/` a3-flash, a3-install-kernel, a3-backup, guard-phone-writes (+ .tests), build-uboot, build-kernel-img, build-rootfs, a3-sync-modules,
  mkconfig-a3y17lte, mkbootimg-samsung, phone-sh, peek/, ect-parse, initramfs/ (init, showoff.sh), probes/
  (bare-metal S-Boot payloads, asv-fuse module)
- `work/` stock DTs (ground truth, rev02 DTB = this unit), kernel config copy, initramfs tree (Alpine 3.24;
  add packages with its own apk: `cd work/initramfs && qemu-aarch64-static -L . ./sbin/apk --root . --usermode
  --arch aarch64 add PKG`; the busybox trigger error is harmless), `work/ssh/` dropbear host key
- `work/firmware/` Wi-Fi/BT/touch-key/sensor-hub blobs + README (sources, sha256, which file goes where;
  proprietary: never commit or upload). `work/system/SYSTEM.img` = stock SYSTEM (A320FLXXS9CTK2), raw, read-only
  copy: pull more files with `debugfs -R "dump /path out" work/system/SYSTEM.img`, no need to touch the phone.
- `out/images/` current u-boot-a3y17lte.img (on BOOT since 2026-11-09 13:35, 2027.01-rc1-00335, eMMC autoboot) and linux-a3y17lte-fastboot.img
- `out/upstream/` patches prepared for sending (rfc-cpufreq-dt-boot-opp: the cpufreq-dt commit, sent apart
  from the 7870 series as an RFC)
- Upstream tracker: https://claude.ai/artifact/RQq4vqHXBeHm8Kuqsrye3r (artifact db: `groups/<A..L,Dp>` with
  commits, status, version, lore link, checks, notes; `log`; `meta/series`, `meta/prep`). Update it with
  ArtifactData (read first, pin if_version) whenever patches are regrouped, sent or reviewed.
- Kernel: branch `a3y17lte` in src/linux, local commits = `git log master..a3y17lte` (local master = v7.4-rc2) (46 as of 2026-11-09; Wi-Fi/BT folded into the initial DTS)

## Display (command-mode panel, working 2026-11-09)

- Datasheet notes (written from the vendor driver, used as the reference for our clean-room driver):
  work/notes/s6e3aa2.md. Read it before touching the panel or the DECON/DSIM command-mode code.
- Pipeline: DECON (exynos7_drm_decon, i80 + hardware TE trigger, TE pin gpe0-2) -> DSIM (samsung-dsim)
  -> S6E3AA2 AMS474MM01 (drivers/gpu/drm/panel/panel-samsung-s6e3aa2.c), 720x1280 command mode, 4 lanes
  at 490 MHz. Exynos DRM removes simpledrm at bind. Needs CONFIG_EXYNOS_IOMMU (else DECON waits for its
  sysmmu until the 13 s deferred-probe timeout).
- What made it work (all in the series): DSIM CMD_TE_CTRL0/1 timers (zero = every frame ABNORMAL),
  porches programmed in BOTH DSIM and DECON in command mode (they must match; DSIM-only = garbage),
  DECON VCLK RUN_VDEN_DISABLE, panel mode vbp 10 / vfp 3 / vsa 1, backlight hooked into drm_panel.
- Brightness: /sys/class/backlight/14800000.dsi.0 (0-255, non-linear like Samsung's UI scale). Gamma for
  74 steps is computed from the panel's MTP at first prepare; per level a step + AOR + ELVSS is sent.
- Panel safety: only volatile registers are written. Never send OTP/MTP program commands.
- Re-enable bug "PLL failed to stabilize" (2026-11-11): fix installed 2026-11-12, awaiting
  confirmation; see "Not done".
- A "white" screen at AOR 0 is the panel's stale frame memory, NOT our data. To test whether pixels
  arrive, fill the framebuffer black/with a pattern and see whether the screen changes.
- Useful state: DCS 0x0A (0x9C ok), 0x0D bit 3 (all pixels off), 0x0E (0x80 TE on, bit 0 DSI error),
  0x05 (error count); DSIM INTSRC 0x14800034 (healthy: 0x01080000, bit 22 = ABNORMAL).
- Before any DRM driver (U-Boot/simpledrm): the panel only refreshes because U-Boot enables the DECON
  hardware TE auto-trigger (TRIGCON 0x6B0 = 0x2058; VIDCON0 ENVID|ENVID_F; DECON_UPDATE 0x710 |= 1).
- Code-execution probe without screen: write 0x12345671 to PMU INFORM3 (0x1048080c), 1 to PMU SWRESET
  (0x10480400) -> phone reboots into download mode (seen on USB as 04e8:685d).
- S-Boot loads custom BOOT images fine; arm64 header layout matching stock (text_offset 0x80000,
  flags 0) is what's been tested (the default in build-uboot).

## Input

- Touchscreen works: Imagis IST3038H at 0x50 on 13890000.i2c (normal I2C, not the secure gpc3 bank),
  mainline imagis driver. Also regulator-haptic and gpio-keys. Event and i2c bus numbers change between
  boots and kernels: find devices by name in /proc/bus/input/devices and /sys/bus/i2c/devices/*/name.
- Intermittent: on 1 of 2 rc2 boots imagis read chip ID 0x0 and the touchscreen failed to probe
  (never seen on rc1). Both supplies are already on from the bootloader, so the 60 ms delay in
  imagis_power_on() is not a real power cycle: first suspect if it recurs.
- Touch keys work (2026-11-09): ABOV A96T316 at 0x20 as a variant of mainline tm2-touchkey
  ("abov,a96t316-touchkey"), KEY_APPSELECT (left) / KEY_BACK (right), and an LED class device
  `tm2-touchkey` (backlights on by default). The bus is i2c-gpio on gpc8-0/1 like the vendor kernel:
  the hardware i2c2 on the same pins gets no ACK. IRQ gpa1-4. vcc = vdd_fixed_touch, vdd = LDO33
  (3.3 V, touch-key AVDD/LEDs; the J5 comment calling it a proximity sensor was wrong). The chip's FW
  is 0x0C, the same as the vendor blob, so no firmware flashing is needed (that would be a chip write).
- Hall sensors (2026-11-09): gpa2-3 = flip cover, SW_LID through gpio-keys. gpa1-3 = Samsung's
  "certify" hall, not described in the DT. Each reacts to only ONE magnet pole: when testing, flip the
  magnet over. Read raw levels with `peek 0x139f0084` (gpa2 DAT) and `peek 0x139f0064` (gpa1 DAT), bit 3.
- `i2cdetect` is blocked by the guard hook (it probes with writes); `i2cget` reads are fine.
- Kernel make calls by hand must use `LLVM=1` (as in build-kernel-img): a plain or GCC make
  silently converts out/linux/.config to GCC. If that happens, re-run tools/mkconfig-a3y17lte.
- Building older commits (per-commit/bisect checks) makes syncconfig drop options whose Kconfig
  entry doesn't exist yet (e.g. DRM_PANEL_SAMSUNG_S6E3AA2 -> black screen). Re-run mkconfig after.

## Boot loop (working as of 2026-11-08)

- `poweroff` and `reboot` work on our mainline kernel (user-confirmed). Don't re-audit power-off.
  The power button runs `poweroff` via busybox acpid (openrc `acpid`, /etc/acpi/PWRF/00000080).

- S-Boot -> our U-Boot (BOOT partition, `build-uboot`, flash with a3-flash) -> boots /boot on SYSTEM
  (sysboot mmc 0:13 /boot/extlinux/extlinux.conf: Image, a3y17lte.dtb, initramfs.gz) -> Alpine, no PC
  needed (2026-11-09). If that fails it falls through to fastboot. Hold Volume Down at power-on for the
  menu (Boot Kernel / Boot Previous Kernel / Fastboot / Reboot / Power Off). U-Boot has no MMC/FAT/ext4
  write support. U-Boot shows the previous kernel's pstore console on screen before booting.
- After a kernel release change (new rc), run `tools/a3-sync-modules` before rebooting into it,
  or Wi-Fi/BT modules fail with "version magic" errors.
- Kernel install: RAM-test with `fastboot boot` first, then `tools/a3-install-kernel` (ssh file copies into
  /boot, sha256-checked; old set kept as *.prev for extlinux-prev.conf). To RAM-test while the phone
  autoboots: Volume Down menu -> Enter Fastboot Mode.
- `fastboot boot` of a U-Boot image (Samsung v0 format from build-uboot) does NOT chainload: instant
  reset. Test U-Boot changes on BOOT via a3-flash.
  ramoops uses the common 7870 layout (32 KiB at 0x46e00000: 16 KiB console, 16 KiB pmsg, no dump
  records); U-Boot's bootcmd/config must match it. No firmware regions are reserved, like the other
  7870 boards: a 1.5 GB fill-and-verify over the freed RAM was clean (2026-11-08).
- Kernel config: `tools/mkconfig-a3y17lte` regenerates it (pmOS config + our changes); add new options
  there. Never run kernel make/olddefconfig without ARCH=arm64: it silently converts the config to x86.
- Kernel: `build-kernel-img` (ccache; builds Image, dtbs AND modules) then `fastboot boot out/images/linux-a3y17lte-fastboot.img`.
  RAM only, nothing written. Since eMMC autoboot, a crash/reset boots the INSTALLED kernel again, not
  fastboot: for a RAM test the user picks "Enter Fastboot Mode" in the Volume Down menu.
- The initramfs is a SEPARATE gzip ramdisk in the boot image (ramdisk at 0x49000000), not built into the
  kernel (INITRAMFS_SOURCE=""). With it embedded, a 38.9 MB Image (image_size 0x2510000) died before printing
  anything while 33.8 MB booted; cause unknown (2026-11-08). Keep an eye on Image size (now ~30 MB).
- A kernel that dies before ramoops starts leaves the OLD pstore log, so U-Boot shows a stale trace on
  screen. Check the timestamps before blaming the new kernel. A Power+VolDown cold reset corrupts pstore bytes.
- Boot image MUST carry real load addresses (kernel 0x40200000, dtb 0x48000000). With the AOSP default
  U-Boot runs the kernel in place and moves it up for 2 MiB alignment, over the DTB: silent instant reset.
- Phone: `ssh a3` (dropbear, key ~/.ssh/a3y17lte_ed25519, host key pinned in ~/.ssh/known_hosts_a3, listens
  on 10.78.70.1 only); serial shell /dev/ttyACM0 as fallback; network 10.78.70.1 (host 10.78.70.2 via nmcli
  con "a3-usb"), `nc 10.78.70.1 2323` dumps dmesg. U-Boot fastboot is 04e8:6602, Linux gadget is 1d6b:0104.
- Watchdog (CPUCL0, 0x10170000) runs from boot (`s3c2410_wdt.tmr_atboot=1` in the cmdline): a hang
  or panic resets the phone in ~13 s (U-Boot then autoboots eMMC). `reboot -f` works via its restart
  handler (~4 s). After `reboot`, wait for the USB link to drop before waiting for it to come back:
  the old system answers pings for a few seconds while shutting down.
- Thermal: one TMU sensor at 0x10130000 (zone soc-thermal, passive 76 C, critical 110 C). TRIMINFO
  25 C trim on this unit is 319 (9-bit), so mainline's Exynos7 efuse range would read ~274 C.
  Both CPU clusters are its cooling devices. Test trips with .../thermal_zone0/emul_temp.
  Its IRQ (SPI 435) is level-high, verified at the GIC; mainline forced rising edge until our fix.
- `fastboot devices` prints "Android Fastboot" (capital F): grep -i when waiting for it.
- `phone-sh 'reboot -f'` never returns (the serial port vanishes): wrap it in `timeout 5`, then
  fastboot boot separately. Use short separate commands; no long chained wait loops.
- After a reset VMware sometimes does not hand the phone back to the VM: if nothing shows in lsusb
  after ~20 s, ask the user to reconnect it.

## Storage

- microSD (mmc2, hybrid SIM2/SD tray): high speed 50 MHz (cap-sd-highspeed), card detect = gpa0-1 active
  low as IRQ (cd-gpios, hot-plug verified). UHS SDR50/104 untested (only an SD 2.0 card so far).
- Rootfs on SYSTEM (mmcblk0p19, 3.9 GiB), live since 2026-11-08: `tools/build-rootfs` (Alpine 3.24 + openrc +
  dropbear, ext4 label a3root, 1 GiB, grown on the phone with resize2fs). Boot it with
  `build-kernel-img --root=system` -> out/images/linux-a3y17lte-system.img (init falls back to the debug
  shell if SYSTEM has no /sbin/init). The plain linux-a3y17lte-fastboot.img still boots the RAM-only shell.
  build-rootfs also fills /boot (via `a3-install-kernel --stage`) with the last kernel build, so run
  `build-kernel-img --root=system` first; a freshly flashed rootfs then autoboots.
- Write SYSTEM/CACHE/USERDATA ONLY with `tools/a3-flash PART IMG --i-mean-it --via-linux` from a running
  mainline boot (dd over ssh + read-back sha256, raw images only; ~5 MB/s). Heimdall cannot: over the VMware
  USB link S-Boot erased SYSTEM, the session timed out and nothing was committed; the phone then showed the
  "!" update-failed screen until a clean session (re-flashing the identical U-Boot to BOOT) cleared it.
- Stock SYSTEM is gone; restore: `tools/a3-flash SYSTEM work/system/SYSTEM.img --i-mean-it --via-linux`
  (sha256 in out/system-img.sha256, verified equal to the original partition).
- Download mode from Linux: `poke 0x1048080c 0x12345671; poke 0x10480400 1` (peek/poke in /usr/local/bin).

## Wi-Fi / Bluetooth (QCA9377, working 2026-11-08)

- Wiring (from the stock DT): power switch gpf3-0 (regulator-fixed, always on: shared, and hci_qca has no
  supply for it), WLAN_EN gpd3-6 (mmc-pwrseq-simple on mmc1), BT_EN gpd4-0, BT on serial1 (UART1) at 3 Mbaud,
  32 kHz sleep clock from the S2MPU05 described as a fixed clock. Host-wake pins unused (in-band SDIO IRQ).
- Wi-Fi: ath10k_sdio, qca9377 hw1.1, SDIO high speed 50 MHz, firmware-sdio-5.bin (WLAN.TF.1.1.1-00061) +
  the A3's bdwlan30.bin as board.bin. ~2.4 MiB/s download on 2.4 GHz ch1 at -35 dBm; 5 GHz scans but untested.
- BT: hci_qca reads controller version 0x00230302 (not 0x00440302); uses the stock rampatch_tlv_tf_1.1.tlv +
  nvm_tlv_tf_1.1.bin renamed to qca/rampatch_00230302.bin / nvm_00230302.bin (linux-firmware's 00230302 files
  are an older QCA6174 patch). "Frame reassembly failed (-84)" once at power-up is harmless.
- Everything is modules (Image stays ~29 MB). `tools/a3-sync-modules [--apk]` stages modules + firmware + boot
  config into out/a3-extra and copies it onto the live rootfs over ssh (file copies only); build-rootfs uses
  the same staging. Packages for the phone (no internet of its own until Wi-Fi is up) are in out/apk-extra.
- Addresses are made up, by the user's choice (real ones are in EFS: don't use them): wlan0 02:a3:17:00:00:02,
  hci0 02:A3:17:00:00:01, set by /etc/init.d/a3-addr (boot runlevel). Without an address hci0 stays
  "unconfigured" (btmgmt shows no index).
- Boot: /etc/modules loads ath10k_sdio + hci_uart; wpa_supplicant, dbus, bluetooth in default; local.d/wlan.start
  runs udhcpc -b on wlan0 and ntpd (no RTC time otherwise: TLS fails at 1970).
- Wi-Fi credentials: /etc/wpa_supplicant/wpa_supplicant.conf on the phone only (the user enters them; never
  copy them into the repo or the rootfs build). The home LAN is 172.16.42.0/24, so wlan0 gets 172.16.42.x
  from the router: fine for wlan0, but USB stays on 10.78.70.0/24.

## Battery / charger (S2MU004, working 2026-11-08)

- Fuel gauge: I2C 0x3b on &i2c0 (13830000, i2c-0 on the phone), IRQ gpa0-3. Driven by mainline
  s2mu005-battery via `"samsung,s2mu004-fuel-gauge", "samsung,s2mu005-fuel-gauge"` (same registers);
  sysfs name `s2mu005-fuel-gauge`. Capacity is the gauge's raw RSOC (no stock capacity_max scaling).
- Charger: S2MU004 main block, 0x3d on &i2c4 (13870000, i2c-1), via mfd sec-core (`samsung,s2mu004-pmic`)
  + s2m-charger S2MU004 variant; sysfs `s2mu004-charger` (online/status/health, read from the chip on
  each access). IRQ gpa2-7 level-low: regmap-irq masks all 40 sources at probe (the bootloader leaves
  some unmasked; before every bit was declared, plug events did not reach the CHGIN child). The charger
  uses chgin + MUIC vbus-on/vbus-off (cable), done, fault -> power_supply uevent; one IRQ per plug change.
- **Charger watchdog:** the bootloader leaves it enabled (CTRL13 bit1, 80 s). Without a driver clearing
  it, charging stops ~80 s after boot (STATUS1 fault = WD suspend, battery ~0 mA on the cable). The
  driver clears it every 30 s (CTRL14 bit0) and cycles buck->charge mode if it already expired.
  User-approved write (2026-11-08); other charger settings (input 500 mA, charge 450 mA, float 4.35 V)
  are left as the bootloader set them. Changing those needs the user's yes.
- USB-C/PD controller: 0x3c on &i2c0, no driver (later phase). Raw register survey and test log:
  work/notes/s2mu004-regs.md.
- Safety: no userspace I2C writes to these chips and no I2C bus scans (quick-write probing); the guard
  hook blocks the i2c-tools write/scan commands (even inside heredocs: edit docs with the editor).
  With the drivers bound, reads need `i2cget -f`.
- Read status: `cat /sys/class/power_supply/*/uevent`. Measured: ~-150 mA idle on Wi-Fi, ~+350 mA
  charging from the VMware USB port; unplug/replug switches state by itself.
- The phone's dropbear listens only on the USB address, so cable-out tests log to a file on the phone.
- Left (2026-11-08): wall-charger speed (MUIC charger-type detection + higher input limit, needs the
  user's yes) together with battery-temperature limits (thermistor on the SoC ADC); stock-style %
  scaling (cosmetic); USB-C/PD at 0x3c (own phase). Camera flash is an RT8547, not on the S2MU004.

## CPU DVFS facts (for cpufreq work)

- CMU_CPUCL0 0x10900000, CMU_CPUCL1 0x10800000: PLL_CON0 +0x100 (pll_1417x), MUX_CPUCLx_PLL +0x200,
  SWITCH_USER +0x204, MUX_CLK_CPUCLx +0x208, DIV_CLK_CPUCLx_1 +0x400 ... _PLL +0x424. S-Boot leaves
  both clusters at 902.2 MHz (P5 M347 S1) and BUCK1 (shared by both clusters) at 700 mV.
- ECT (S-Boot's DVFS/ASV tables) lives at 0x90000000, 0x14000 bytes. Already dumped: decode with
  `tools/ect-parse`. To dump again, reserve it no-map in the DTS temporarily, but that reservation can
  overlap U-Boot's fastboot buffer: U-Boot then prints "reserving fdt memory region failed" and hangs
  (fastboot still lists the device but no command works; the user must hold Power+VolDown).
  Saved: out/ect-a3y17lte.bin, out/ect-a3y17lte-cpucl.txt, out/cpufreq-opp-a3y17lte.txt.
- ASV fuses (0x100d9000) are secure-only (peek gets SIGBUS). Read them with the
  tools/probes/asv-fuse module (vendor SMC_CMD_REG SFR read; prints, then refuses to load).
  This unit: CPU group 9, table version 5. Vendor volt = ECT ASV table[5][level][9], no SSA offsets.
- Enabled CPU levels: 1586 MHz down to 343 MHz (1794+ are disabled in every ASV table).
- cpufreq works (2026-11-08): CMU_CPUCL0/1 + CPUCLK_LAYOUT_E7870 + cpufreq-dt, DT OPPs at worst-case
  voltages (lowered per chip by exynos-asv, below), both clusters cooling devices of soc-thermal. The alternate parent during PLL relock is
  oscclk (SWITCH_USER mux left on OSC). Local config default governor is `userspace`, so frequency
  only changes by hand: `echo KHZ > .../cpufreq/policy{0,4}/scaling_setspeed`.
- Per-chip voltages (2026-11-08): exynos-asv (in the exynos-chipid driver) reads the ASV fuses by
  SMC and lowers the DT worst-case OPPs to the vendor table (drivers/soc/samsung/exynos7870-asv.c,
  tables generated from the ECT: steps of 6.25 mV above 600 mV; SSA tables are in ECT GEN, see
  `tools/ect-parse FILE SSA`). dmesg: "CPU cluster N: ASV group 9, table version 5". Check with
  debugfs opp/cpuN/opp:*/supply-0/u_volt_target and regulator/regulator_summary.
- BUCK1 voting gap fixed in cpufreq-dt: each policy sets its boot OPP at init, so both clusters
  hold a request from the start (875 mV until the first change after ASV).
- CPU idle works (2026-11-08): PSCI core power-down (param 0x10000, PSCI v0.1 IDs from DT), MCT as
  broadcast timer. The 7870 firmware only powers a core off if the kernel clears PMU
  CPUn_CONFIGURATION[3:0] (0x10482000 + ((cluster<<2)|cpu)*0x80) first: exynos-pmu does it from a
  CPU PM notifier. Without it the board resets within seconds of boot. Check with
  .../cpuN/cpuidle/state1/usage; PMU STATUS (+4) low nibble 0 = core powered off.

## GPU and postmarketOS (as of 2026-11-11)

- The phone runs postmarketOS (Phosh) from eMMC. ssh: `work/notes/gpu-gsk-spill/pm 'cmd'` (user
  `user` @10.78.70.1, key ~/.ssh/a3y17lte_ed25519, known_hosts_a3pmos). Root: `sudo -S` with
  `out/pmos-password` on stdin (the user is fine with that).
- Build pmOS kernel/device packages ONLY with `tools/build-pmos --packages [mesa gtk4.0 ...]` (full
  images: no argument). It skips a kernel commit that is already built and never rebuilds
  dependencies; Mesa/GTK forks build only when named (a Mesa build takes very long). Its header lists the traps: `--src` drops kernel/time/timeconst.bc via .gitignore,
  dated pkgver, GCC `CC` leaking into resolve_btfids (the APKBUILD unsets CC). Install on the phone:
  copy the .apk over ssh, `sudo apk add --allow-untrusted ./x.apk` (kernel install rewrites /boot via
  boot-deploy). The pmaports kernel APKBUILD `_commit` must be a commit pushed to the gh fork.
- GPU DVFS: kernel groups S (G3D PLL clk) and T (OPP table, cooling, GPU supply), installed. BUCK2
  feeds G3D + MIF/INT/DISP/CAM; the bootloader leaves it at 750 mV and nothing votes for the
  buses, so no GPU voltage may go below 750 mV. Check: /sys/class/devfreq/11400000.gpu/trans_stat.
- Userspace: pmaports forks `temp/mesa` (999926.2.6: Midgard compiler fixes + renderonly leak fix;
  aarch64 only, rusticl/NVK off) and `temp/gtk4.0` (99994.24.1: adaptive gradient supersampling),
  9999 version prefix so Alpine cannot replace them: rebase on every Alpine mesa/gtk4.0 upgrade.
  The user wants stable releases on the phone; Mesa/GTK main only for preparing MRs.
- Clean Mesa series: branch `a3-26.2.6` in work/ref/mesa-26.2.6-series; GTK: work/ref/gtk-4.24.1
  (commit a8decab). Mesa main checkout: work/ref/mesa-main. Upstream groups U, V, W in the tracker.
- After a Mesa change, check for buffer leaks: `sudo cat /sys/kernel/debug/dma_buf/bufinfo | tail -1`
  before/after a short client run. Phosh/phoc only pick up new Mesa after logout/login.
- Notes: work/notes/gpu-perf/README.md (state, results, open items), work/notes/gpu-gsk-spill/ (compiler,
  shader-db helpers `sdb`/`sdb-each`, phone timing `time-run`/`tour-run`).

## Phone-side helpers

- `tools/phone-sh 'cmd'` runs a command over ssh (exit status passed through), falling back to the serial
  shell (/dev/ttyACM0) when port 22 is closed; PHONE_SH_SERIAL=1 forces serial. Serial can wedge (python
  stuck in tty close): if it hangs, use ssh or ask the user for Power+VolDown.
- pinctrl@139d0000 (TOUCH, gpc3) and pinctrl@139e0000 (ESE, gpc7) are secure-world only: any access
  aborts (peek gets SIGBUS, kernel oopses). Disabled in the board DTS; keep them disabled.
- `tools/peek/peek` (+ `poke`, `dump` symlinks): 32-bit MMIO read/write via /dev/mem; `dump ADDR LEN`
  writes raw bytes to stdout (the range must not touch System RAM: STRICT_DEVMEM). Alpine busybox has no
  devmem. Push it over nc (see tools/peek/build); host nc is ncat, so use `ncat --send-only`.

## Environment notes

- Fedora 45 VMware guest on a Windows host. USB passthrough needs a good USB 2.0 cable
  (USB-C cables gave "device descriptor request failed").
- pmOS 7870 devices boot via S-Boot → U-Boot → systemd-boot from FAT (device-samsung-exynos7),
  not a Samsung boot.img. linux-postmarketos-exynos7870 = mainline 6.15 + exynos7870-mainline patches.
