# GPU performance targets (Mali-T830 MP1, Exynos 7870)

## Published vendor-driver numbers (Android, Samsung's Mali driver)

| Benchmark | Score | Source |
|---|---|---|
| GFXBench 2.7 T-Rex HD offscreen (1080p) | 12 fps | Notebookcheck, Mali-T830 MP1 page |
| GFXBench 3.0 Manhattan offscreen (1080p) | 5.1 fps (4.9-5.4) | Notebookcheck |
| GFXBench 3.1 Manhattan ES 3.1 offscreen | 3 fps | Notebookcheck |

- https://www.notebookcheck.net/ARM-Mali-T830-MP1-GPU.215360.0.html
- https://www.notebookcheck.net/Mali-T830-MP1-vs-Mali-T830-MP3_7718_7686.247598.0.html

These are chip-level figures, not runs on an A3 2017. No published glmark2-es2 score for a
Mali-T830 was found (searched 2026-11-10).

## Limits of the comparison

GFXBench has no build for Linux/Wayland on arm64, so the published numbers cannot be
reproduced on postmarketOS, and glmark2 has no vendor reference. What can be made equal
to the vendor setup and checked directly:

1. Hardware operating points: the same clock levels and bus rates as the vendor table
   (GPU 343-1001 MHz, MIF 902 MHz, INT 400 MHz).
2. Per-shader cost: bundle counts and frame times for the same shaders.

glmark2-es2 off-screen (720x1186 here) and the frame-time cases in `../gpu-gsk-spill/` are the
before/after measure for every change.

## This unit

- ASV fuses (0x100d9000..): 0x00090009 0x00090007 0x00070009 0x00091e85.
  CPU clusters group 9, G3D group 7, MIF group 9, INT group 9, DISP group 7, table version 5.
- G3D voltages for this unit (table 5, group 7): 343 MHz 600 mV, 450 612.5, 545 662.5,
  676 718.75, 728 737.5, 845 806.25, 1001 900 mV.
- Worst case over all table versions (group 0), used in the DT: 343/450/545 750 mV,
  676 812.5, 728 875, 845 925, 1001 1006.25 mV.
- BUCK2 feeds G3D, MIF, INT, DISP and CAM (the vendor GPU driver and the stock DT devfreq
  nodes all name "BUCK2"). The bootloader leaves it at 750 mV. Nothing in mainline votes
  for the buses, so the GPU must never request less than 750 mV here.
- ECT boot levels: MIF index 0 (902 MHz), INT index 0 (400 MHz), i.e. the buses are
  expected to be at their top rates after boot (to be confirmed from clk_summary).
- Full decode of the G3D/MIF/INT/DISP/CAM tables: `ect-g3d-mif-int.txt`.
