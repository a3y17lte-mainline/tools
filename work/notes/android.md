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

- vendor_samsung_a3y17lte is **public by the user's decision** (2026-11-14): it carries the 4 firmware
  files of device/.../proprietary-files.txt (Wi-Fi/BT; the rest of work/firmware stays local).
- The kernel repo is a full single-branch clone of ACK (the GitHub repo started empty, so a
  blob:none clone could not be pushed). Remotes: origin = ACK, torvalds = kernel.org (for
  prerequisites), gh = GitHub. It never shares a worktree, .config or out dir with src/linux.
- Android tooling lives in tools/android/ and touches nothing the mainline work uses.

## Kernel: tools/android/backport-kernel

The branch is generated, never edited by hand: ACK base -> prerequisites
(tools/android/kernel-prereqs.txt, "UPSTREAM:"/"BACKPORT:" prefixes) -> `git am -3` of the
mainline series (format-patch of src/linux master..a3y17lte) -> tools/android/kernel-patches/
("ANDROID:" commits: defconfig, S-Boot DT properties). Conflict resolutions are kept by rerere in
tools/android/rr-cache, so a regeneration after the series changes replays them.

Expected manual rework: samsung-dsim (we do not take the 17 upstream bridge-API commits),
pinctrl-exynos-arm64 context, exynos-pmu (gs101 split), cpuidle-psci, tcpm, ssp_sensors, s3fwrn5,
and the sec-* mfd files (S2MU005 sits on the S2MPG10/11 rework).
Optional later: 65c132fa8c93 (dw_mmc DMA threshold, needs adapting to the pre-refactor struct).

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

## Graphics

Mesa Panfrost (GLES 3.1; no Vulkan on Midgard) + minigbm + drm_hwcomposer. The Mali blob HALs of
the community trees are not usable.
