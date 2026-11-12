# GPU performance work (Mali-T830, Panfrost)

Plan: `~/.claude/plans/we-have-built-a-eager-metcalfe.md`. Targets and this unit's fuse
data: `targets.md`. Shader/compiler side: `../gpu-gsk-spill/README.md`.

## Results

glmark2-es2-wayland `--off-screen` (the FBO is 720x1186: phoc maximises the window and
glmark2 takes its size, so scores are not comparable with 800x600 runs elsewhere), Mesa 26.2.6 + both Midgard patches
(`/home/user/mesa-test`), glmark2 unpacked in `~/gpu-bench` on the phone (not installed).
`time-run` values are ms per frame at scale 2.

| State | glmark2 score | terrain | refract | blur | gradient 360x593 | same, 1 sample (`GSK_GPU_DISABLE=gradients`) |
|---|---|---|---|---|---|---|
| 343 MHz fixed (installed kernel a89accf66396) | 72 | 4 | 10 | 21 | 193 | 55 |
| GPU DVFS 343-1001 MHz (RAM-booted 77382b95b917) | 162 | 8 | 23 | 48 | 70 | 20 |

Files: `glmark2-343mhz-mesa-r1.txt`, `glmark2-dvfs-mesa-r1.txt`.
The three `buffer` scenes stay at 33-36 fps at any GPU clock: they are CPU-bound.

## Phase 1: clocks (2026-11-10)

- Buses need no work: `clk-summary-boot.txt` shows the memory PLL at 1804.4 MHz (MIF 902
  level) and the INT clocks at the 400 MHz level, as the ECT boot indexes say.
- GPU DVFS, kernel branch `a3y17lte`:
  - `631a68dbe3cc` clk: G3D PLL rate table (PMS from the ECT), `CLK_SET_RATE_PARENT` on
    `mout_g3d` and `dout_g3d_bus`. The PLL is type 14180 in the ECT; the vendor spec has a
    10-bit M divider for it, so mainline's `pll_1417x` (10 bits) is the right type and
    `pll_1418x` (9 bits) would not fit M = 545.
  - `77382b95b917` dts: OPP table 343-1001 MHz, GPU as cooling device.
  - Config: `DEVFREQ_THERMAL=y` (in `tools/mkconfig-a3y17lte`; not yet in the pmaports
    kernel config).
- Test: `tools/build-pmos-ram-kernel -e DEVFREQ_THERMAL`, `fastboot boot
  out/images/pmos-ram-test.img`. panfrost uses `simple_ondemand`; BUCK2 reads 750 mV at
  343 MHz and 1006 mV at 1001 MHz; the PLL relocks under the running GPU without the
  vendor's mux switch (as on Exynos5433). 179 transitions over a glmark2 run, no faults,
  44-46 C.

## Phase 2: Midgard compiler (2026-11-10)

Patches (all in `../gpu-gsk-spill/`, applied in this order in pmaports
`temp/mesa-midgard-test`, r6 on the phone in `/home/user/mesa-test-r6`):

1. `midgard-ra-aliasing-spill.diff`: spill across the r26/r27 aliasing (compile fix).
2. `midgard-keep-expensive-branches.diff`: no if-flattening of pow/log/exp/div.
3. `midgard-test-knobs.diff`: test only, `MDG_PEEPHOLE_LIMIT` / `MDG_EXPENSIVE_OK`.
4. `midgard-remat-varyings.diff`: a spilled value that is a varying load without
   register sources is loaded again before each use instead of going through TLS.
   Shader-db: spill reloads -42 % on T830 (693 -> 399), bundles unchanged.
5. `midgard-fewer-registers-retry.diff`: fragment shaders that use more than 8 work
   registers are compiled a second time limited to 8 (2x the threads per core); the
   result with the lower estimated cost (bundles x 1.0 at 1 thread, x 0.55 at 2) is
   kept. The attempt gives up after 100 RA rounds and never replaces a working
   shader. `MIDGARD_MESA_DEBUG=noregretry` turns it off. Shader-db: 349 shaders switch
   on T830/T820, 335 on T860, 68 on T720; no failures.

Phone, GPU DVFS kernel, full-window gradient (ms per frame):
| Mesa | ms |
|---|---|
| patches 1-2 | 66.5-67.7 |
| 1-4 | 66.7 |
| 1-4, 8 registers forced | 54.7-55.4 |
| 1-5 (r6) | 54.7-55.1 |
| 1 sample per pixel (`GSK_GPU_DISABLE=gradients`, not Cairo) | 20.3-20.6 |

glmark2 (`glmark2-dvfs-mesa-r6.txt`): 161 vs 162, its shaders already fit in 8
registers. The `build` and `ideas` scenes swing by up to 40 % between runs (CPU-bound,
schedutil), so single-run differences there are noise.

What the shaders GTK compiles on the phone look like (`MIDGARD_MESA_DEBUG=shaderdb`):
the gradient pipeline is a 304-bundle shader at 16 registers (529 bundles at 8) and a
360-bundle one at 11 (429 at 8). They differ from the captured `shcap/7`.

### Midgard hardware features: used or not

| Feature | State in Panfrost/Midgard | Effect here |
|---|---|---|
| Thread count by register use (4/8/16) | Compiler always used up to 16 | Used now via patch 5: -18 % on the gradient |
| Transaction elimination (CRC) | Implemented | `nocrc` changes nothing in glmark2 off-screen |
| AFBC | Implemented | `noafbc` changes nothing in glmark2 off-screen |
| Hierarchical tiling | Not on T830 (Panfrost knows) | - |
| Out-of-order texture execution | Used | - |
| Interpipe register aliasing | Handled (patch 1 fixed the spill case) | - |
| Bundle packing | 2.2 ALU ops per bundle on the gradient, 117 of 388 ALU bundles single-op | Open: scheduler work |
| Load/store pairing | 62 single-op LS bundles, 12 adjacent pairs | ~2 %, not done |
| "T7xx pairing rules" (vendor feature list) | Set for every Mali from T720 to Valhall (not T60x/T62x); the kernel never reads it, only passes it to the blob | Generation marker, nothing to gain |
| 4-register tier (4x threads) | GSK shaders cannot be allocated in 4; 346 of 3576 shader-db FS would switch (Skia, games), none glmark2/GTK | Deferred (Firefox) |
| No tier below 4 registers | `pan_props.c`: 4/8/16 only | - |

Packing estimate (temporary instrumentation in `schedule_block`, longest dependency chain
per block): gradient 938 ALU ops in 474 ALU bundles, floor 372 (-21 %, unreachable); about
5 % of gradient time realistically. glmark2 shaders 1277 ALU bundles, floor 730.

## Phase 3: GTK gradient shader (2026-11-10)

- `GSK_GPU_DISABLE=gradients` is "don't supersample gradients", not a Cairo fallback.
  So the whole remaining gap was the 4 samples per pixel.
- GTK change (`gtk-linear-gradient-adaptive-supersampling.patch`, against 4.24.1): take one
  sample where all four fall between the same two stops with no hint and no repeat
  boundary in between (their average is then the centre colour). Tested without rebuilding
  GTK: shaders from `gsk/gpu/shaders` processed with GTK's own scripts, loaded through
  `G_RESOURCE_OVERLAYS=/org/gtk/libgsk/shaders/gl=/home/user/gsk-shaders/new`.
- Output check (`gradient-test/`, `gtk4-rendernode-tool render --renderer=gl`): 7 cases
  (stops, hard stops, repeating stripes, reverse/diagonal, pad, fractional bounds); old vs
  new shader differ in 3 pixels by 1/255.
- Mesa fix found on the way (patch 6, `midgard-remat-remove-dead-load.diff`): the
  rematerialisation left the original varying load in place with no users. Removing it:
  T830 shader-db 155 shaders changed, -534 instructions, -262 bundles, 2 Skia shaders +1 spill.
- Full-window gradient, ms per frame:

| Mesa | GTK 4.24.1 shader | adaptive shader |
|---|---|---|
| r6 | 55.1 | 31.8 |
| r7 (patch 6) | 48.5 | 27.2 |
| r7, 1 sample (`GSK_GPU_DISABLE=gradients`) | 20.7 | - |

- A variant that always takes the fast path is as slow as the real check (31.3 vs 31.8 ms
  on r6): the check costs nothing; the rest of the gap to 1 sample is the two extra
  derivative texture ops and the larger shader.
- r7: phosh-tour 2x clean, glmark2 164 (r6 161), no faults (clean rerun; the first r7 run had GNOME Maps open and is discarded).
- Radial and conic gradients not done: offset is not linear in position there.

## State 2026-11-11 (phone runs all of this, untethered)

| Piece | Where | Installed on the phone |
|---|---|---|
| Kernel GPU DVFS (S: clk, T: OPP/cooling/supply) | `src/linux` a3y17lte `15bd435cebaa`..`edef9dd36714` (pushed; before the 2026-11-12 reorder `4f2936bf77f6`..`e04846a59a11`, tag `a3y17lte-pre-dsim-reorder`) | `linux-samsung-a3y17lte-7.4_p20261111003613-r1` (same tree as the old tip 77382b95b917), `DEVFREQ_THERMAL=y`; previous boot files in `/home/user/boot-backup-20261111/` |
| Mesa Midgard series + renderonly leak fix | branch `a3-26.2.6` in `work/ref/mesa-26.2.6-series` (worktree of `work/ref/mesa-26.2.6`), 6 commits | pmaports `temp/mesa` 999926.2.6-r1 (still the old patch files incl. test knobs; same code) |
| GTK adaptive gradient supersampling | `work/ref/gtk-4.24.1` commit a8decab, `gtk-linear-gradient-adaptive-supersampling.patch` | pmaports `temp/gtk4.0` 99994.24.1-r0 |
| Gradient quirk removed | pmaports `device-samsung-a3y17lte` r2 | yes |

Full-window gradient with the installed system: 27.4 ms (was a timeout). glmark2 164.

Memory leak (fixed): Mesa's renderonly (kmsro) imported every client buffer into the
display device on phoc's render-node fd and released it with DESTROY_DUMB, which render
nodes refuse. 789 MB of exited clients' buffers were held after a few hours; the GPU
shrinker purged every few seconds, Firefox/Maps starved. Diagnosis: `/sys/kernel/debug/dma_buf/bufinfo`
(panfrost-exported buffers still attached to 14830000.display-controller) and
`/sys/kernel/debug/dri/11400000.gpu/gems` (owners already exited). Fix: GEM_CLOSE
(`mesa-renderonly-close-imported-handles.diff`). Check after any Mesa change: run a short
client and compare the `Total ... objects` line of `bufinfo` before and after.

Tracker: groups S, T (kernel), U, V (Mesa), W (GTK) in the upstream tracker.

## Per-chip GPU voltages (2026-11-12: implemented, tested, installed in pmOS kernel r3)

Kernel 4195aef43c59, abda411c1106, 2ce321ca6342 (tracker group X). RAM test: ASV group 7, table 5;
GPU 1001 MHz at 900 mV (was 1006.25), 343 MHz asks 600 mV and BUCK2 stays at the 750 mV floor;
glmark2 `--off-screen --swap-mode immediate` 224, no faults. (With the phone locked, glmark2 needs
`--swap-mode immediate`: phoc sends no frame callbacks and the default mode scores 0.)

### Design

Data (ECT dump, `tools/ect-parse out/ect-a3y17lte.bin g3d`): ASV dvfs_g3d has 7 table
versions x 10 levels (1300..343 MHz; 1146 MHz and up disabled) x 16 groups, the same
6.25 mV/600 mV encoding as the CPU tables. GEN SSA_G3D: split at level 7 (levels 0..6 =
1300..676 MHz take SSA10, the rest SSA11), SSA0 floor 575 mV + 25 mV steps, offsets
0/12.5/25/50 mV: the same rules as the CPU, only the split differs (CPU: 6). Fuse word 1
bits 15:0 = G3D group/mod-group/SSA10/SSA11/SSA0 in the CPU layout. This unit at table 5:
1001 MHz 900 mV if group 7 (DT worst case 1006.25 mV).

1. BUCK2 floor: BUCK2 also feeds MIF/INT/DISP/CAM, which have no consumer in mainline,
   and the bootloader leaves it at 750 mV. Put `regulator-min-microvolt = <750000>` on BUCK2
   in the board DTS (a board constraint: J5/A3 share the PMIC wiring, so exynos7870-j5lte
   too if its DTS describes BUCK2 the same way). The regulator core then clamps every GPU
   request to >= 750 mV, so ASV values below that (most of the 343..545 MHz rows) are
   harmless. Keep each OPP's DT maximum (1006.25 mV) when lowering, as for the CPUs, so the
   requested range always contains 750 mV.
2. ASV for a device whose OPP table appears only when panfrost probes: exynos-asv (chipid,
   early) cannot touch the GPU table at its own probe. Options:
   a. Create the table first from the ASV side (dev_pm_opp_of_add_table on the GPU device;
      panfrost's later add_table just takes a reference). Rejected as fragile: panfrost
      configures the table (supported_hw, regulators) expecting to do it before the OPPs
      are parsed, and ASV would need the GPU device long before panfrost binds.
   b. (chosen) Bus notifier: exynos-asv registers a platform bus notifier and, on
      BUS_NOTIFY_BOUND_DRIVER for the device of the `gpu` node (found from the chipid
      node, e.g. a `samsung,asv-gpu` phandle or by the SoC compatible of the GPU node),
      calls dev_pm_opp_adjust_voltage() per level; also handles a GPU that was bound
      before the notifier was registered. The adjust sends OPP_EVENT_ADJUST_VOLTAGE; the
      new voltage applies at the next devfreq transition (until then the GPU runs at the
      DT worst case, which is safe). Unbind needs nothing (the table goes away with it).
   Generalise exynos7870_asv_init_subsys() to take the frequency list, the level count and
   the SSA split, add a third subsys for G3D (exynos_asv.subsys[] has 2 entries today).
3. Test (RAM only, fastboot boot of build-pmos-ram-kernel): dmesg "GPU: ASV group N",
   debugfs opp/.../11400000.gpu/*/supply-0/u_volt_target, regulator_summary for BUCK2
   (never below 750 mV), glmark2 + the GTK gradient at 1001 MHz pinned, then a long run.

## Open

- Mesa series (2026-11-12): shader-db re-run per commit on T720/T820/T830/T860, three messages
  corrected (branch `a3-26.2.6` rewritten). Rebased on main in `work/ref/mesa-main`: branches
  `renderonly-gem-close` (Cc: mesa-stable; send first) and `midgard-t830`, both apply cleanly,
  not built on main. pmaports `temp/mesa` 999926.2.6-r2 uses the format-patch output (no test
  knobs); installed on the phone 2026-11-12, bufinfo unchanged after a client run.
  The phone stays on stable releases (26.2.x, GTK 4.24.x); main is only for sending.
- GTK patch: rebased on GTK main (`work/ref/gtk-main`, branch `gpu-gradient-adaptive-ss`,
  applies unchanged: the shader file is identical on main); radial/conic not covered.
- Per-chip GPU voltages (this unit needs 900 mV at 1001 MHz, the DT worst case is
  1006.25 mV): extend exynos-asv to G3D (fuse word 1 bits 15:0, same layout as the CPU;
  SSA_G3D split at vendor level 7 = 676 MHz and above use SSA10). Needs (a) the 750 mV
  BUCK2 floor as a board regulator constraint (shared with MIF/INT/DISP/CAM, which have
  no consumer), and (b) ASV for a non-CPU device whose OPP table only exists once panfrost
  probes (create the table from the ASV side, or defer).
- Thermal behaviour over a long run, `emul_temp` check of the GPU cooling device.
- Phase 2 leftovers (not worth it now): ALU bundle packing (~5 % on the gradient),
  4-register retry (no GTK/glmark2 shader benefits; maybe Firefox/Skia).
- Phase 3 rest: phoc (direct scanout, damage, compositor cost) and display path checks.
- pmbootstrap notes: it does not evaluate `case "$CARCH"` blocks for makedepends (put an
  aarch64-only fork's deps in the main list); its cross build has no rustc (rusticl/NVK off
  in the fork). Build the kernel only through `tools/build-pmos --packages` (see the script
  header for the .gitignore/timeconst.bc, version and CC pitfalls).
