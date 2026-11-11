# GPU frequency scaling (Mali-T830): design notes

Superseded 2026-11-10: implemented and tested, see `gpu-perf/README.md`. The text below is
the earlier design note; its open questions are answered there.

Status 2026-11-10: design only. No kernel, DTS or config change was ever made for this;
`src/linux` branch `a3y17lte` has only "Add the GPU supply" (BUCK2) for the GPU.

Shelved because the GTK gradient timeouts are not a clock problem: the shader ran about
24x more work per pixel than needed (see `gpu-gsk-spill/README.md`). Even at 1001 MHz a
full-screen gradient would still have taken about 250 ms.

## Known state

- The GPU runs at the rate the bootloader leaves: `fout_g3d_pll` = 343 MHz
  (`sudo grep g3d /sys/kernel/debug/clk/clk_summary`), BUCK2 at 750 mV.
- No `/sys/class/devfreq` node: the GPU node has no OPP table, so panfrost never sets up
  devfreq.
- Vendor levels (`gpu_dvfs_table_default`, vendor kernel
  `drivers/gpu/arm/t83x/r29p0/platform/exynos`): 343, 450, 545, 676, 728, 845, 1001 MHz
  enabled (`GPU_MAX_CLOCK` 1001); 1146, 1246, 1300 are in the table but above the maximum.
- BUCK2 is shared with the MIF and INT domains in the vendor DT (`devfreq_mif`,
  `devfreq_int` use `reg_name = "BUCK2"`).

## What an implementation needs

1. A rate table for `fout_g3d_pll` (`pll_1417x`, `g3d_pll_clks` in
   `drivers/clk/samsung/clk-exynos7870.c`; today it is registered with `NULL`, so the rate
   is read-only). The G3D gates already carry `CLK_SET_RATE_PARENT`.
2. An OPP table on `gpu@11400000` (`arch/arm64/boot/dts/exynos/exynos7870.dtsi`) with those
   levels. Panfrost then scales the clock and `mali-supply` by itself.
3. Voltages: start from the vendor worst-case column in the ECT (`tools/ect-parse`,
   `out/ect-a3y17lte.bin`); about 1.006 V at 1001 MHz. The per-chip GPU ASV group has not
   been read (the CPU one was, with `tools/probes/asv-fuse`).
4. Config: `DEVFREQ_GOV_SIMPLE_ONDEMAND` is already on; `DEVFREQ_THERMAL` is off.

## Open questions

- Whether MIF/INT need a minimum BUCK2 voltage that a GPU-only OPP table could undercut.
- Whether the PLL may be relocked while the GPU is clocked from it, or needs the
  `mout_g3d` switch to the alternate parent first (as the CPU clusters do).
