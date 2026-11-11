# GTK4 glitches on Mali-T830: Midgard "Failed to choose spill node"

## What happens
GSK's supersampled linear-gradient fragment shader (`shcap/7.shader_test`) makes
Mesa's Midgard register allocator give up (`ERROR: Failed to choose spill node`,
`src/panfrost/compiler/midgard/midgard_ra.c`). `mir_ra()` returns without
assigning registers, the shader is emitted anyway, and the GPU rejects it
(`js fault ... INSTR_INVALID_ENC`), so part of the frame is never redrawn.

## Cause (confirmed on the PC, Mesa 26.2.6)
T720/T820/T830 have `MIDGARD_INTERPIPE_REG_ALIASING`: texture sources (class
TEXR) share r26/r27 with load/store sources (class LDST). At the failure:
failing class = LDST, and both registers are held by two long-lived TEXR
values (spill cost 0, so spillable). The allocator only looks for a spill
candidate in the failing class, where every node is already a one-bundle copy.

## Fix
`midgard-ra-aliasing-spill.diff`: on aliasing GPUs, when the failing class has
no spill candidate, spill a node of the other class that shares the registers.

## Results (`results/`, one process per shader, captured shaders + shader-db, 7391 shaders; T720: 817, it is ES2-only)
| GPU  | before                              | after       | other shaders |
|------|-------------------------------------|-------------|---------------|
| T830 | 3 fail (one aborts on an assertion) | all compile | identical     |
| T820 | same 3                              | all compile | identical     |
| T720 | 2 fail (the two shader-db ones)     | all compile | identical     |
| T860 | none                                | none        | identical     |

Failing before: `shcap/7` (GSK gradient), shader-db `glmark/10-2` (vertex),
`unity/64-DeferredPointShadows` (fragment). All three take the LDST -> TEXR
direction; no shader exercises TEXR -> LDST.

## Reproduce
- Mesa: `work/ref/mesa-26.2.6/build-host` (panfrost + drm-shim; the shim needs
  the two extra GET_PARAMs added in `panfrost_noop.c`, a local change).
- `./sdb 830 shcap/7.shader_test` (one run), `./sdb-each 830 <outdir>` (all shaders).
- Phone: `pm` (ssh), `time-run <w> <h> <css|-> [ENV=val ...]` (frame times), `tour-run [ENV=val ...]` (phosh-tour for 12 s),
  `grad-run <w> <h> [ENV=val ...]` (`grad.py`: one CSS gradient box of that
  size). Both print spill errors and new js faults / sched timeouts.
  `gcc-run` (Settings) no longer triggers the shader.

## Phone test of the fix (2026-11-10)
Patched Panfrost-only build of 26.2.6 (pmaports `temp/mesa-midgard-test`),
library in `/home/user/mesa-test`, used through `LD_LIBRARY_PATH`, shader cache off.

| run | spill errors | js faults | sched timeouts |
|---|---|---|---|
| phosh-tour, system Mesa, gradients on | 1 | 1 | 0 |
| phosh-tour, patched, gradients on (3 runs) | 0 | 0 | 1 each |
| phosh-tour, system Mesa, `GSK_GPU_DISABLE=gradients` (3 runs) | 0 | 0 | 0 |
| grad 100x100, system Mesa | 1 | 1 | 0 |
| grad 100x100, 300x300, 360x400, patched | 0 | 0 | 0 |
| grad 500x500, 700x800, 700x1200, patched | 0 | 0 | 1 |

Sizes are logical pixels at scale 2 and the window is clipped to 360x593, so
the last good size is 720x800 device pixels (576k) and the first bad one is
720x1000 (720k).

So the fix removes the compile error and the INSTR_INVALID_ENC fault, and the
shader runs. But it is about 560 bundles per fragment (on T860 too, where it
always compiled), and one job covering most of the screen takes longer than
panfrost's 500 ms job timeout on this single-core T830. The kernel then kills
the job (`gpu sched timeout, js=0, status=0x8`), which leaves the same stale
area on screen. The timeout depends only on the area, which points at speed
and not at wrong code. Not checked: the GPU clock rate (there is no devfreq
node, so the GPU runs at whatever rate the bootloader left).

## Workaround shipped
`device-samsung-a3y17lte` installs `/etc/profile.d/mali-t830-quirks.sh`
(`GSK_GPU_DISABLE=gradients`). Keep it even with a fixed Mesa, unless the
supersampled gradient turns out fast enough at a higher GPU clock.

## Second fix: keep expensive branches (2026-11-10)
The gradient shader was slow because Midgard flattened every `if`
(`nir_opt_peephole_select`, `limit = 64`, `expensive_alu_ok = true` in
`midgard_compile.c`): all 6 colour-stop segments, each with a `pow` and a
`log`, ran for each of the 4 samples of every pixel.
`midgard-keep-expensive-branches.diff` sets `expensive_alu_ok = false`.

PC (`results/keep-branches-summary.txt`, made with `results/cmp.py`): no new
failures on T830/T820/T720/T860; 294 files change, code size +1.4 %, spill
reloads -42 %. With it `shcap/7` compiles even without the spill fix, but
`glmark/10-2` and `unity/64-DeferredPointShadows` still need that one.

Phone (pmaports `temp/mesa-midgard-test` r1 in `/home/user/mesa-test`; r0 = spill
fix only, in `/home/user/mesa-test-r0`). `time-run` (`gradtime.py`) redraws the
box every frame; ms per frame, scale 2, GPU at 343 MHz:

| box (logical) | plain red | gradient r0 | gradient r1 | `GSK_GPU_DISABLE=gradients` |
|---|---|---|---|---|
| 360x300 | 17 | 258 | 102 | 32 |
| 360x593 (whole window) | - | timeout | 193 | 55 |

phosh-tour with r1 and gradients on, 3 runs: 0 spill errors, 0 faults, 0 timeouts.
A window larger than the screen (box 700x1200, about 3.4M pixels) still times out.

Notes:
- After one job timeout the kernel cancels every later job of that GL context,
  so frame times measured after a timeout are meaningless (they read 16.7 ms).
- A plain `queue_draw()` does not make GSK render the node again; `gradtime.py`
  alternates the opacity to force it.
- The GPU gradient is now inside the timeout but still about 3.5x slower than
  the non-supersampled GPU gradient, so the profile.d workaround remains the faster setting.
  (`GSK_GPU_DISABLE=gradients` is GSK's "don't supersample gradients": still the GPU, one
  sample per pixel instead of four. Earlier notes called it the Cairo fallback; it is not.)
