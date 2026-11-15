# Modem: Shannon 315 (SS315AP) on the Exynos 7870

Status 2026-11-15 night: **the modem registers on LTE** with the user's SIM, from a RAM boot
(section 0): hsi2c0 in batcher mode, samsungipcd with the NV service, AT port by hand. Not tried
yet: SMS, data, calls, suspend. The SIM is the user's own: no PIN tries, nothing sent without
asking. Earlier the same day:
Part A done. Kernel: 10 commits on branch
`modem` (worktree `src/linux-modem`, based on the a3y17lte tip of that morning, 929b94890e8d;
not pushed, not installed, not in the tracker yet: section 8). U-Boot needs one commit first
(branch `modem`, worktree `src/u-boot-modem`): it loads files into the modem's memory. Userspace: libsamsung-ipc with an
a3y17lte profile and samsungipcd with an NV service, cross-built into `out/modem-userspace`
(section 6). Part B (probes, first CP start, SIM, data) waits for Alpine: section 10.
Plan: `~/.claude/plans/start-researching-and-planning-luminous-eclipse.md` (this file wins).

Sources, all local unless a URL is given: vendor kernel `src/vendor-kernel` (`drivers/misc/modem_v1`,
`drivers/misc/mcu_ipc`, `drivers/soc/samsung/pmu-cp.c`), stock DT `work/stock-dt/stock-02.dts`,
stock `/sbin/cbd` (ramdisk of `backups/*/BOOT.img`), stock `libsec-ril.so`
(`work/system/SYSTEM.img`), the RADIO/EFS/CPEFS backup images (read-only; never copied into git).
The vendor code is a register/protocol reference only: the driver is written clean.

## 0. Phone results (2026-11-15, RAM boot of the `modem` kernel through U-Boot on RECOVERY)

Setup: U-Boot from the `modem` U-Boot branch on RECOVERY (`out/images/u-boot-a3y17lte-modem.img`,
a3-flash, sha256 d7a25b69...; BOOT keeps Android). Recovery key combo -> U-Boot -> no extlinux
on SYSTEM -> fastboot -> `fastboot boot out/images/linux-a3y17lte-modem.img`
(`tools/modem/build-ram-img`: modem kernel + debug initramfs + modules in /lib/modem + the
userspace). Undo: `a3-flash RECOVERY out/android/new-20261115-1548/recovery.img`.

- U-Boot built for BOOT runs from RECOVERY, and with the modem reservations it still boots.
- Linux reserves the three regions (`/proc/iomem` affff000-b77fffff reserved).
- **The modem control registers are secure-only.** `peek` of PMU 0x30 (CP_CTRL_NS), 0x34
  (CP_CTRL_S), 0x50 (CP2AP_MEM_CONFIG), 0x70 (MODAPIF_CONFIG) gives a bus error, and the monitor's
  generic SFR read (SMC_CMD_REG) refuses them (-1). Readable: CP_STAT 0x38 = 0x110, CP_DEBUG
  0x3c = 0, CENTRAL_SEQ_CP 0x280 = 0x10000, the five CP SYS_PWR registers = 0xffffffff.
  The lineage defconfig says "CP_SECURE_BOOT is not set", but `SEC_MODEM_SS310AP` selects it:
  the stock kernel binary (BOOT backup) contains the secure-path messages ("read Fail",
  "CP_CTRL Write Fail", "cp_ctrl[0x%08x] -> [0x%08x]") and none of the direct-write ones.
  So CP_CTRL goes through `SMC 0x82000700`: READ_CTRL = (3, 0, reg), value in bits 31-16, error
  in bits 15-0; WRITE_CTRL = (4, value, reg), error if > 0; reg 0 = CP_CTRL_S, 1 = CP_CTRL_NS.
  The driver was changed accordingly (it would have taken a synchronous abort otherwise).
- READ_CTRL returns error 11 for both registers, on CPU 0 and CPU 4, before any image was
  verified (`tools/probes/cp-ctrl`). On stock the first request to this service is the
  unlock/verify pair from cbd, and the kernel only reads CP_CTRL after that (a failed read
  counts as "not powered" there too), so this is expected to change after the verify request.
  The same 11 is what the Galaxy S7 write-up got from its first unlock request.
- Mailbox clock gate CMU_MIF 0x828 = 0x00ffffff (bit 22 on), 0x830 = 1.
- Bootinfo page 0xaffff000: 280 non-zero bytes. The first ~0x90 bytes look like DRAM
  initialisation data (LPDDR mode register writes 01 83 00 84 / 02 1c 00 84 / 03 01 00 84 /
  0b 00 00 84, timing words); after that stale Android heap pointers (0xb4... tagged), because
  the Android kernel does not reserve the page. The firmware region starts with stale Android
  memory too. Not yet compared across two cold boots.
- Both modules load: `exynos-mailbox` (IRQ 401 = SPI 369), `exynos_modem` (all `/dev/umts_*`,
  `multipdp`, `rmnet0-7`, the fail and watchdog IRQs).
- `modem-nv-setup`: EFS mounted "ro without journal" (norecovery), NV working copy in
  /var/lib/modem/nv (RAM in this boot).

### First modem starts (2026-11-15 evening, `ipc-modem boot`, boot only, no SIM/network)

- Unlock request 2 returns 11 on the first start after a cold boot and 0 on later ones; the
  verify request 0 returns 0 every time. After verify, the AP gets a bus error on the firmware
  region (the monitor locks it): never touch `fw_base` after `SEC_VERIFY`.
- The loader runs: both handshake stages pass, the loader's answers arrive in the RAW ring.
- Then nothing: no PHONE_START. Mailbox: CP->AP message word stays 0x83 (from the loader),
  status (ISSR5) 1, active (ISSR9) 1, no performance requests, INTMR0 = 0x73e20000 (the CP
  masked its side), CP_STAT stays 0x110, the IPC magic stays "BOOT". BOOT_OFF times out after
  15 s. The CP fail and watchdog interrupts never fire.
- Not the cause (each tried, same result): stale IPC header (now cleared at probe), clock
  tables written only at INIT_END (now also before power-on), MIF/INT at their lowest level
  (held at 902/200 MHz for a run), MIF frequency word 0 (written by hand).
- A second start without a reboot does not get past BOOT_ON (status never set): the restart
  path is not right yet. Stock resets the CP through the PMU sequencer path every time; ours
  only resets a powered CP. Use a fresh boot per attempt for now.
- **Leading suspect: the PMIC bus.** The S2MPU05 on hsi2c0 is shared with the CP, which sets
  its own RF regulators through the PMIC. The stock DT runs hsi2c0 in "batcher" mode
  (`samsung,hsi2c-batcher`, second reg range 0x10500180 inside the CP mailbox block, second
  IRQ SPI 372): every transfer is handed to a batcher that takes a hardware semaphore. Mainline
  drives the controller directly, which collides with the CP: in every run our side logged
  "exynos5-hsi2c 10510000.i2c: tx timeout" some seconds after the CP started (never otherwise),
  and our IRQ handler would also acknowledge the CP's controller interrupts.
- Batcher mode for i2c-exynos5 is written (kernel branch `modem`: binding, driver, DT commits,
  register sequence identical to the vendor's for a 2.5 MHz HS bus, checked on the host against
  a transcription of the vendor code). Hardware check so far, read path only:
  `tools/probes/batcher-test` (adapter lock, controller IRQ masked, the driver's own functions,
  controller restored afterwards) read PMIC 0x00-0x0f through the batcher, 1- and 7-byte reads:
  identical to the normal path (0x01/0x02 are clear-on-read), 0 failures, normal driver fine
  afterwards.
- NOT done: the write path. The probe's `wreg=5` mode (INT2M 0xfb -> 0xff -> 0xfb through the
  batcher, original restored through the normal path) was refused by the session's safety
  check (a PMIC write), and booting the batcher kernel (`out/images/linux-a3y17lte-modem.img`
  as built at 18:01) writes the PMIC through the same path, so it was not booted either. Both
  need the user's yes. Worst case of a wrong write: a crash (watchdog -> Android, `adb reboot
  recovery` -> U-Boot) or, unlikely, a PMIC power-off that needs the power key.

### Batcher mode: the fix (2026-11-15 night, user's go)

- Write path checked with `tools/probes/batcher-test wreg=5`: INT2M 0xfb -> 0xff -> 0xfb
  through the batcher, batcher and normal reads agree at every step, original restored.
- First boot of the batcher kernel: `sec-pmic-i2c 5-0066: Failed to set masks in 0x4: -6`, so no
  PMIC driver, no regulators, everything deferred, no UDC: the system ran but was unreachable,
  and `poweroff` failed (no PMIC). The user had to reset it. pstore log:
  `out/pstore-batcher-crash-20261115.txt`.
- Cause: in batcher mode the probe skipped what the vendor probe does in every mode: clear the
  controller's pending interrupt status, reset, init. The bootloader's last batcher transfer
  leaves a controller interrupt pending, and the batcher reports it as an unexpected interrupt
  (state bit 1 -> -ENXIO). In the probe tests the normal driver had already cleared it. (A
  bare software reset is enough for the batcher: checked with `batcher-test rawrst=1`.)
  Fixed (fixup on "i2c: exynos5: Add batcher mode").
- Safety net: `tools/initramfs/init` reboots into recovery (our U-Boot -> fastboot) when no UDC
  appears within ~70 s; `build-ram-img` puts the current init into the image.
- Then: PMIC probes, 32 regulators, cpufreq steps BUCK1 600-975 mV through the batcher with no
  error, charger fine. `ipc-modem boot`: security 2 -> 11 (first after cold boot), 0 -> 0,
  state 3 at 31.46 s, **state 4 (online) at 32.56 s**, CP->AP word 0xc8 (PHONE_START), IPC
  magic 0xaa / access 1. No hsi2c timeouts, no CP fail/watchdog interrupts. The CP repeats
  PHONE_START (~300 mailbox interrupts in a minute) until FMT and RFS are open.
- So the PMIC bus is shared with the CP and must be used through the batcher on every board
  with a running modem. The CP mailbox block's 0x10500180 range (stock second reg of hsi2c0)
  was never needed.

### Protocol stack, SIM, network (2026-11-15 night, user's go)

- Clean RAM boot, `samsungipcd /dev/ptyp0` (legacy pty; AT on `/dev/ttyp0`, `stty raw -echo`):
  it boots the modem itself, opens FMT and RFS, INIT_END goes out, the stack starts.
- NV service: the modem wrote 0x10b60 bytes at 0x80000 and 0x32d6 at 0x0 right after start:
  exactly the regions that differ between stock's nv_data.bin and the 2019 backup. Served from
  the RAM working copy; EFS untouched (mounted ro,noload).
- Without SIM: `AT+CGSN` returns the IMEI (from NV), `+CSQ: 18`, `+CREG: 1,3` (denied, no SIM),
  SIM status 0x80 (IPC_SEC_PIN_STATUS_CARD_NOT_PRESENT).
- SIM inserted while running (user's own SIM, slot 1, PIN off): hot-detected, status
  0x00 -> 0x82 -> 0x83 (init complete), `+CPIN: READY`; registered: `+CEREG: 1,1` (LTE),
  `+CGREG: 1,1`, `+CREG: 1,6` (SMS only on the CS side, as on LTE without VoLTE). `+CSQ` reads
  0,99 on LTE (the proxy's CSQ only covers 2G/3G).
- The AT port queues unsolicited +CREG/+CGREG/+CEREG reports: the first command after a while
  returns a long backlog before its own answer.

## 1. Decisions (user, 2026-11-15)

- Kernel interface: clean core; a Samsung-compatible front end (`/dev/umts_boot0`, `umts_ipc0`,
  `umts_rfs0`, `rmnetN`, `multipdp`) first, so libsamsung-ipc and the stock Android RIL run
  unmodified. An upstream-shaped WWAN front end comes after the hardware works; the compat front
  end then stays a local/Android-only commit.
- Test system: Alpine with the open stack (libsamsung-ipc fork + samsungipcd + ModemManager).
  Android (stock cbd + libsec-ril through the LineageOS Samsung RIL) follows.
- NV data: the modem writes NV (section 7), so the end state is read-write. First stage: EFS
  mounted read-only, NV files copied to a working directory, every write logged. Real EFS
  read-write only after reviewing that log, with a fresh EFS backup and the user's explicit yes.
- Scope: SIM, registration, SMS, mobile data. Voice calls (mixer CP port) and GNSS later.

## 2. Hardware overview

- Modem name in the stock DT: `ss315ap`, modem type 0x0a (SEC_SS310AP), SIPC version 50, link
  type shmem (0x200), `mif,link_attrs` 0x7c8 = DPRAM magic check, memory-type boot and dump,
  boot/dump frames padded, bootloader upload. No SBD, no IOSM messages.
- This unit: SM-A320FL, single SIM, `androidboot.hw_rev=4`.
- CP memory (not reserved by our DTS today; RAM is 0x40000000-0x7e3fffff + 0x80000000-0xbfffffff):

  | Region | Address | Size | Use |
  |---|---|---|---|
  | deliver_cp | 0xaffff000 | 0x1000 | S-Boot leaves data here; first 0x800 bytes copied to IPC+0x1000 before each CP start |
  | CP firmware | 0xb0000000 | 0x7400000 (116 MB) | BOOT/MAIN/NV images, CP private RAM |
  | IPC | 0xb7400000 | 0x400000 (4 MB) | shared map, section 5 |
  | GNSS (unused here) | 0xb7c00000 | 0x400000 | KEPLER GNSS, own mailbox 0x10570000 |

  The CP sees its firmware area at 0x40000000 (TOC load addresses).
- Interrupts: mailbox SPI 369, CP fail SPI 18, CP watchdog SPI 479. CP fail is enabled only
  during boot, CP watchdog only after boot.
- PMU (0x10480000) registers, from `include/soc/samsung/pmu-cp.h`:

  | Offset | Name | Bits used |
  |---|---|---|
  | 0x0030 | CP_CTRL_NS | PWRON bit 1, RESET_SET bit 2, START bit 3, ACTIVE_REQ_EN 5, ACTIVE_REQ_CLR 6, RESET_REQ_EN 7, RESET_REQ_CLR 8 |
  | 0x0034 | CP_CTRL_S | START bit 3 |
  | 0x0038 | CP_STAT | low byte 1 = CP in sleep (vendor free-memory path); 0x110 seen on mainline with the CP never started |
  | 0x003c | CP_DEBUG | |
  | 0x0050 | CP2AP_MEM_CONFIG | base[13:0] in 4 MB units, size[24:16] in 4 MB units; set by S-Boot (the vendor setter is never called) |
  | 0x0070 | MODAPIF_CONFIG | |
  | 0x0280 | CENTRAL_SEQ_CP_CONFIGURATION | written 0 before a CP reset |
  | 0x1170, 0x11cc, 0x11d0, 0x11d4, 0x11d8 | RESET_AHEAD_CP, CLEANY_BUS, LOGIC_RESET_CP, TCXO_GATE, RESET_ASB_CP SYS_PWR | written 0 before a CP reset |

  CP_CTRL_NS/S are secure-only and go through the secure monitor (section 0); the sequencer and
  SYS_PWR registers are plain PMU writes.
- Vendor power sequences (`pmu-cp.c`, `modem_ctrl_ss310ap.c`):
  - on: clear all 64 mailbox registers; write `hw_rev | (dual_sim << 8)` to mailbox register
    0x21; copy 0x800 bytes deliver_cp -> IPC+0x1000; pda_active (mbox 6) = 1; if CP_CTRL_NS.PWRON
    is already set only CP_CTRL_S.START is set, else PWRON then START; sleep 300 ms.
  - reset: skipped if the u32 at IPC+0xf80 is 0xDEB; else the 6 SYS_PWR/sequencer writes above,
    then CP_CTRL_NS.RESET_SET, wait 80-100 us.
  - off: clear CP_CTRL_NS.PWRON.
  - CP watchdog IRQ: set RESET_REQ_CLR, state "crash watchdog". CP fail IRQ: set ACTIVE_REQ_CLR,
    state "crash reset".
- Secure monitor: `SMC 0x82000700 (mode, arg2, arg3)`, wrapped in `SMC 0x82001011 (0)` before
  and `(1)` after (SSS clock on/off).
  - mode 2 "re-init / non-secure": args 0, 0. Unlocks the firmware area for the upload.
  - mode 0 "secure": arg2 = BOOT size, arg3 = MAIN size. Verifies the image.
  - mode 1 (dump): arg2 = BOOT size, arg3 = address of the IPC region.
  - Stock cbd takes CPU0 out of its affinity mask around the ioctl that issues this SMC and
    restores it afterwards. Mirror it: issue the SMC from a CPU other than 0.

## 3. Mailbox (MCU_IPC, 0x10500000)

Registers (`regs-mcu_ipc.h`): MCUCTLR 0x00 (bit 0 software reset), INTGR0 0x08, INTCR0 0x0c,
INTMR0 0x10, INTSR0 0x14, INTMSR0 0x18, INTGR1 0x1c, INTCR1 0x20, INTMR1 0x24, INTSR1 0x28,
INTMSR1 0x2c, ISSR0.. at 0x80 + 4*n (64 shared registers).

- CP -> AP: INTSR0 bits 16-31 = interrupts 0-15, acknowledged by writing the bits to INTCR0.
- AP -> CP: write `1 << n` to INTGR1. Probe clears INTCR1 = 0xffff.
- Mainline `drivers/mailbox/exynos-mailbox.c` drives the same block for ACPM (exynos850
  variant: INTGR0 0x08, INTMR1 0x24), doorbell only.

Register and interrupt assignment (stock DT `mif_pdata`):

| Shared register | Direction | Meaning |
|---|---|---|
| 0 | AP->CP | message word (interrupt mask, section 5) |
| 1 | CP->AP | message word |
| 2 / 3 | AP->CP / CP->AP | wakeup |
| 4 / 5 | AP->CP / CP->AP | status (AP: 1 = boot confirmed; CP: non-zero = bootloader alive; bit 4 = TX flow control) |
| 6 | AP->CP | pda_active (1 = AP awake) |
| 7 | CP->AP | DVFS request (legacy) |
| 9 | CP->AP | CP active (phone_active) |
| 10, 11, 12 | CP->AP | minimum CPU, MIF, INT frequency request |
| 13 | AP->CP | current MIF frequency in kHz, written after every MIF change |
| 0x1e | AP->CP | UART owner (only with a JIG UART cable; stays 0) |
| 0x21 | AP->CP | `hw_rev | (dual_sim << 8)` |
| 0x27 | AP->CP | lock value |

| Interrupt | AP->CP (INTGR1 bit) | CP->AP (INTSR0 bit 16+n) |
|---|---|---|
| 0 | message | message |
| 1 | wakeup | wakeup |
| 2 | status | status (TX flow control) |
| 3 | pda_active changed | perf request (legacy) |
| 4 | | CP active changed |
| 5, 6, 7 | | perf request CPU, MIF, INT |
| 15 | UART owner | |

Frequency requests are level numbers, not frequencies: bits 7-0 = 1-based index into the table
the AP published (below), 0 or out of range = release. In the CPU request bit 31 set selects
cluster 0, clear selects cluster 1. The vendor turns them into minimum-frequency PM QoS requests
(cluster, BUS throughput = MIF, DEVICE throughput = INT). The combined request on interrupt 3 is
compiled out in the vendor driver.

Clock tables: when the FMT and RFS nodes are both open the vendor writes the ECT DVFS level lists
into the IPC map at offset 40 (`struct clock_table`: `char version[4]` = "CT0", `u32 count`,
then `count` entries `{char name[4]; u32 levels}` with names "CL0", "CL1", "MIF", "INT", then the
u32 frequencies of each table in kHz, lowest level first), and only then sends INIT_END. The
lists are every ECT level, enabled or not (`tools/ect-parse out/ect-a3y17lte.bin cpucl mif int`):
- CL0 = CL1: 343000 449000 546000 676000 757000 839000 902000 1014000 1144000 1248000 1352000
  1482000 1586000 1690000 1794000 1924000 2002000
- MIF: 275000 451000 546000 676000 728000 836000 902000
- INT: 134000 200000 334000 400000

Suspend: pda_active = 0 + interrupt 3 on suspend, 1 + interrupt 3 on resume.

## 4. Boot sequence

Device nodes and ioctls ('o' = 0x6f): MODEM_ON 0x19, OFF 0x20, RESET 0x21, BOOT_ON 0x22,
BOOT_OFF 0x23, BOOT_DONE 0x24, STATUS 0x27, DL_START 0x28, XMIT_BOOT 0x40, SECURITY_REQ 0x53,
DUMP_START 0x32, FORCE_CRASH_EXIT 0x34, SHMEM_FULL_DUMP 0x54, CRASH_REASON 0x55,
GET/SET_SHMEM_SRINFO 0x45/0x46 (0x800 bytes at IPC+0x800, "silent reset info").

Stock `cbd` (`-d -tss310 -bm -mm -P platform/13540000.dwmmc0/by-name/RADIO`), function
`start_shannon310_boot`, in order:

1. Wait up to 5 s for property `ril.cbd.rfs_check_done` = 1 (rild validates the NV files first).
2. Boot wake lock; read the TOC from RADIO; check NV; save the SRINFO block (ioctl 0x45).
3. `ioctl STATUS`; if the state is not offline: `ioctl RESET`.
4. `SECURITY_REQ {mode 2, 0, 0}`.
5. Upload with `XMIT_BOOT` (`struct modem_firmware {u64 binary; u32 size; u32 m_offset;
   u32 b_offset; u32 mode; u32 len;}` packed, 28 bytes; cbd is a 32-bit program and fills the
   pointer as 64 bits): BOOT, MAIN from RADIO, NV from `/efs/nv_data.bin`. `m_offset` = TOC load
   address - 0x40000000, chunks of 0xf800 bytes (62 KiB), `size` = size of the whole image.
   The kernel checks the memory sizes embedded in the image against its own: u32 words 16-18 at
   firmware offset 0x10000 are "0MEM" (0x4d454d30), CP size 0x7400000, IPC size 0x400000
   (verified in the RADIO backup). After a CP crash cbd skips images it marked as "already
   transmitted once" (property `ril.cbd.first_xmit_done`).
6. `SECURITY_REQ {mode 0, size_boot, size_main}`.
7. `MODEM_ON` (power sequence of section 2).
8. `BOOT_ON`: link offline, state "booting", wait up to ~1-2 s for mailbox register 5 != 0,
   disable the watchdog IRQ, enable the CP fail IRQ.
9. `DL_START`: reset the ring pointers, write magic 0x424F4F54 ("BOOT"), then AP status
   (mbox 4) = 1.
10. Handshake over `umts_boot0` (SIPC5 frames on channel 215 through the RAW rings; cbd:
    `std_udl_stage_done(0)` then `std_udl_stage_start(0xf)`, each a 4-byte write, poll, 4-byte
    read): write 0x0000900d, read 0x0000a00d; write 0x00009f00, read 0x0000af00. The AP does not
    raise an interrupt for these frames (the loader polls the ring); the loader does for its
    answers.
11. `BOOT_OFF`: wait up to 15 s for the CP's PHONE_START command, enable the watchdog IRQ, state
    "online", disable the CP fail IRQ.
12. cbd then calls ioctl 0x6f48 (0x6f47 if BOOT_OFF failed); this kernel does not know either
    and returns -EINVAL. Restore SRINFO (ioctl 0x46), release the wake lock. cbd then stays in a
    status loop on `umts_boot0` (poll returns POLLHUP on a crash) and handles dumps: ioctl 0x54
    (whole IPC region), 0x57-0x59 (newer kernels), then reset, loader upload, security mode 1,
    DUMP_START, dump data on `umts_ramdump0`.

The link goes to IPC mode in the PHONE_START handler: reset ring pointers, magic 0xAA, access 1.
INIT_END is sent once both the FMT (235) and RFS (245) nodes are open ("rild ready"); until then
the CP repeats PHONE_START.

RADIO image (A320FLXXS9CTK1), TOC of 32-byte entries `{char name[12]; u32 file_off, load_addr,
size, crc, index}`:

| Name | File offset | Load address | Size |
|---|---|---|---|
| BOOT | 0x200 | 0x40000000 | 0x170c |
| MAIN | 0x1920 | 0x40010000 | 0x2479508 |
| NV | (EFS file) | 0x44900000 | 0x100000 |
| OFFSET | 0x7aa00 | 0 | 0x85600 |

## 5. Link layer (SIPC 5.0 over the legacy 4 MB map)

IPC region layout (`struct shmem_4mb_phys_map`, CONFIG_MODEM_IF_QOS off), all little endian:

| Offset | Size | Field |
|---|---|---|
| 0x0 | 4 | magic: 0xAA ipc, 0x424F4F54 boot, 0x44554D50 dump, 0xDEADDEAD crash |
| 0x4 | 4 | access (1 = AP attached) |
| 0x8, 0xc | 4+4 | FMT TX head, tail |
| 0x10, 0x14 | 4+4 | FMT RX head, tail |
| 0x18, 0x1c | 4+4 | RAW TX head, tail |
| 0x20, 0x24 | 4+4 | RAW RX head, tail |
| 0x28 | 4056 | reserved: clock table at 0x28, SRINFO at 0x800 (0x800 bytes), 0xDEB flag at 0xf80 |
| 0x1000 | 4096 | FMT TX buffer (also where the deliver_cp data goes before boot) |
| 0x2000 | 4096 | FMT RX buffer |
| 0x3000 | 2084864 | RAW TX buffer |
| 0x200000 | 2097152 | RAW RX buffer |

TX = AP to CP. Rings: head = write index, tail = read index, one byte kept free
(space = out - in - 1 mod size). The writer advances head, the reader advances tail. An index
>= size is a fatal error.

Message word (mailbox register 0 or 1) with interrupt 0:

| Bits | Meaning |
|---|---|
| 0x80 | valid |
| 0x40 | command present; command = low 6 bits |
| 0x20 / 0x10 | request ack FMT / RAW |
| 0x08 / 0x04 | response ack FMT / RAW |
| 0x02 / 0x01 | data sent FMT / RAW |

Commands: INIT_START 1, INIT_END 2, REQ_ACTIVE 3, RES_ACTIVE 4, REQ_TIME_SYNC 5, CRASH_RESET 7,
PHONE_START 8, CRASH_EXIT 9, CP_DEEP_SLEEP 0xa, NV_REBUILDING 0xb, EMER_DOWN 0xc,
PIF_INIT_DONE 0xd, SILENT_NV_REBUILD 0xe, NORMAL_POWER_OFF 0xf.

Receive: on interrupt 0 snapshot the message word and ring pointers; handle a command; for FMT
then RAW: note a request-ack, read all frames, answer a request-ack with response-ack, a
response-ack restarts a stopped TX queue. A frame with a bad start byte, or longer than the data
in the ring, is fatal: tail = head, forced CP crash.

Transmit: write the frame, advance head, send the "data sent" bit. No space: keep the frame
queued, stop the net queues (RAW), send request-ack, retry every 1 ms from a timer; the CP's
response-ack (or an empty ring) restarts. The CP can also suspend/resume AP TX through status
register 5 bit 4 with interrupt 2.

Forced crash (AP side): magic = crash, access = 0, send CRASH_EXIT, wait for the CP's CRASH_EXIT
(timer), then state "crash exit": userspace sees POLLHUP on FMT/boot nodes and collects a dump.

SIPC5 frame header:

| Byte | Field |
|---|---|
| 0 | config: 0xf8 start pattern, bit 2 padding present, bit 1 extension field, bit 0 control field |
| 1 | channel |
| 2-3 | length of header + payload (without padding), u16 |
| 4 | control (config low bits 11 = multi-frame): bit 7 more, bits 6-0 frame id |
| 4-5 | extended length (config low bits 10): length is u32 at offset 2 |

Padding aligns the frame to `sizeof(size_t)` of the kernel: 8 bytes on the arm64 stock kernel.
The AP pads its frames while the link's `aligned` flag is set. It starts clear, DL_START sets it
(boot frames are padded), and in this link type nothing clears it again: every frame the AP
sends after the first modem boot is padded, IPC included. Received frames say for themselves
whether they are padded (config bit 2).

Channels: PDP 10-24 (`rmnet0` = 10 ...), BT DUN 25 (`umts_router`), CS VT 1 (`umts_csd`), CP log
28 (`umts_dm0`), loopback 30/31, BOOT 215 (`umts_boot0`), DUMP 225 (`umts_ramdump0`), FMT 235/236
(`umts_ipc0/1`), RFS 245 (`umts_rfs0`), 244 `ipc_loopback0`. FMT channels use the FMT rings,
everything else (RFS, boot, PDP) the RAW rings.

Character device behaviour (the compat ABI):
- write: one write = one payload; the driver adds the SIPC5 header (splits into multi-frames only
  if the node has a max TX size, e.g. `umts_router` 0x800). IPC channels return -EPERM unless
  the CP is online.
- read: one frame's payload per read (header stripped), remainder kept if the buffer is short;
  waits at most 100 ms and returns 0 when nothing arrived. Multi-frame messages are delivered
  after the last fragment.
- poll: POLLIN when data is queued; POLLHUP on FMT and boot nodes once the CP crashed.
- open of FMT/RFS triggers INIT_END when both are open; RX for a closed node is dropped.
- Net devices: ARPHRD_PPP-like raw IP (no L2 header), MTU 1500, protocol from the IP version
  nibble. `multipdp` is a dummy node (waketime/loopback/txlink attributes, protocol
  suspend/resume ioctls 0x25/0x26).

## 6. Userspace

- Stock: `cbd` (boot, dump), `rild` + `libsec-ril.so` (IPC on `umts_ipc0`, RFS on `umts_rfs0`).
- Open: libsamsung-ipc, `herolte` profile (same kernel ABI); fork
  github.com/manoedinata/libsamsung-ipc branch `shannon-lte-data` (LTE attach/APN messages);
  github.com/manoedinata/samsungipcd (AT proxy for ModemManager/oFono, data over `rmnet0`,
  tested on a Galaxy J4, October 2026; no SIM PIN entry, no USSD).
- With herolte's NV size (2 MiB) the library's check of our 1 MiB file fails and it overwrites
  `nv_data.bin` from `.nv_data.bak` (2019): never run the herolte profile against the real EFS.
- samsungipcd as published opens the RFS channel (the kernel wants FMT and RFS open before it
  ends the initialisation) but never reads it: the modem's NV requests go unanswered. Data works
  that way on the Galaxy J4, so a Shannon modem of this family runs without its NV writes being
  stored.
- The fork's `ipc_rfs_nv_data_item_size_setup()` returns the header size only (heap overflow in
  the NV read response). Fixed in our patches.

Our userspace (2026-11-15), built by `tools/modem/build-userspace` into `out/modem-userspace`
(a tree for the rootfs, not installed; needs libcrypto3 and musl on the phone):
- Patches in `tools/modem/patches/` on the pinned upstream commits (clones in `work/ref/`, branch
  `a3y17lte`): libsamsung-ipc gets the NV read fix, the NV path taken from the client, a log
  line per NV write, and the `a3y17lte` device (herolte code, NV size 1 MiB, NV directory from
  `A3Y17LTE_NV_DATA_DIR`); samsungipcd gets an RFS thread that answers NV item reads and writes
  and logs every other RFS command.
- Built for `IPC_DEVICE_NAME` a3y17lte with the NV files in `/var/lib/modem/nv` (the working
  copy). `usr/bin/ipc-modem` is the library's test tool (boots the modem, prints IPC).
- `tools/modem/test-nv` (host, qemu-user): the profile accepts the NV files of the EFS backup as
  they are, and an NV write leaves all four files byte-identical when the data is the same.
  Passed.
- `tools/modem/modem-nv-setup` (phone): by-partlabel links, EFS mounted read-only with `noload`
  (no journal replay, so no write at all), first-run copy of the four NV files.
- Not built: the ModemManager plugin (github.com/manoedinata/debian_j4lte,
  `mm-plugin-samsungipcd`). It patches itself into a ModemManager 1.14 autotools tree; Alpine
  3.24 ships a meson-built ModemManager with plugins in a different place, so it needs a port.
  The first phone tests do not need it: samsungipcd's AT port can be driven by hand.
- No .apk packages either (no abuild on the host): the tree is copied instead.

## 7. NV data and RFS

- Files on EFS: `nv_data.bin` (1 MiB) + `nv_data.bin.md5`, `.nv_data.bak` + `.nv_data.bak.md5`,
  `nv.log`, `.nv_state`; CPEFS holds `.nv_core.bak` + md5. m9kefs1-3 and CP_DEBUG are blank.
- Checksum: md5(file contents followed by the ASCII string `Samsung_Android_RIL`), stored as 32
  hex characters. Verified on both files of the backup.
- The modem writes NV: stock rewrote `nv_data.bin` and its md5 at 2026-11-07 23:57:42 (its last
  session). Against the 2019 backup file 18 of 256 4 KiB blocks differ (0x0-0x3fff and around
  0x80000-0x8dfff).
- libsamsung-ipc implements RFS NV_READ_ITEM 0x4201 and NV_WRITE_ITEM 0x4202 (offset, length);
  a write rewrites the md5 file. Stock libsec-ril additionally handles generic file requests
  (open/read/write/close/lseek/unlink/rename/mkdir/opendir/..., `RFS_NV_RESTORE`,
  `RFS_NV_WRITE_ALL_ITEM`) under `/efs` and `/cpefs`.

## 8. Kernel code (branch `modem`, 2026-11-15)

Worktree `src/linux-modem`, build directory `out/linux-modem` (config = out/linux/.config +
`MAILBOX EXYNOS_MBOX=m WWAN=m EXYNOS_MODEM=m EXYNOS_MODEM_LEGACY_ABI`; the options are in
`tools/mkconfig-a3y17lte` too). A separate worktree because `src/linux` was mid-rebase in another
session. To join the series: `git rebase --onto a3y17lte 929b94890e8d modem`, then add the
tracker groups (pending entries in `out/tracker/ledger.md`).

| Commit | What |
|---|---|
| dt-bindings: clock: samsung,exynos7870-cmu: Add the modem mailbox clock | `CLK_GOUT_MIF_MAILBOX_CP_PCLK` (145) |
| clk: samsung: exynos7870: Add the modem mailbox clock | CMU_MIF 0x828 bit 22, parent dout_mif_apb, not critical |
| dt-bindings: mailbox: google,gs101-mbox: Add the Exynos7870 modem mailbox | `samsung,exynos7870-mbox`, `#mbox-cells = <1>` (interrupt number) |
| mailbox: exynos: Support the Exynos7870 modem mailbox | one channel per interrupt in both directions, RX in hard IRQ context, shared registers as the mailbox device's regmap |
| dt-bindings: net: Add the Samsung Exynos integrated modem | `samsung,exynos7870-modem` |
| net: wwan: Add the Samsung Exynos integrated modem interface | `drivers/net/wwan/exynos_modem/`: `ctrl.c` (boot steps, PMU, SMC, IRQs, performance requests, MIF notify, suspend), `link.c` (rings, SIPC5, commands, flow control, crash), `sipc5.h`, `modem.h` |
| net: wwan: exynos_modem: Add KUnit tests for the link helpers | `sipc5_test.c`, 11 cases |
| arm64: dts: exynos7870: Add the modem and its mailbox | `mbox_cp`, `modem` (disabled) |
| arm64: dts: exynos7870-a3y17lte: Enable the modem | the three no-map regions, board revision 4 |
| net: wwan: exynos_modem: Add the vendor kernel's userspace interface | `legacy.c`: `umts_boot0/ramdump0/ipc0/ipc1/rfs0/csd/router/dm0`, `multipdp`, `rmnet0-7`; local only |

Design points:
- The boot steps are separate functions (`exmodem_reset/security/fw_write/power_on/boot_on/
  dl_start/boot_off/dump_start`), driven by the front end; the legacy front end maps the vendor
  ioctls onto them one to one. A WWAN front end would run them in sequence itself.
- Ports: a front end registers a receive callback per SIPC channel; frames for channels nobody
  listens on are dropped (the vendor forces a modem crash for unknown channels).
- The verify/unlock SMC runs through `work_on_cpu()` on a CPU other than 0, like cbd does.
- The modem gets the ECT level lists as tables (hardcoded per SoC); its level requests become
  `freq_qos` minimums on the cluster's cpufreq policy and `dev_pm_qos` minimum frequencies on
  the DMC (`samsung,mif-devfreq`) and INT bus (`samsung,int-devfreq`) devfreq devices. Each MIF
  change is reported as the nearest table value in kHz.
- Suspend/resume toggle pda_active with interrupt 3. No wake-up source is set up yet.
- Not implemented: sending multi-frame messages (only `umts_router` would need them), ioctl 0x54
  (IPC region dump for cbd), the CP_UPLOAD ioctl (it panics the vendor kernel on purpose).
- The CP mailbox clock is no longer left on by accident: it is on while the mailbox driver is
  bound.

Checks run: `W=1` build of the driver, full `Image dtbs modules` build of the branch (Image
29.5 MB, `exynos_modem.ko`, `exynos-mailbox.ko`), `checkpatch --strict` (only the missing Signed-off-by and the
MAINTAINERS question, as for the rest of the series), `dt_binding_check` for both schemas,
`dtbs_check` of the A3 DTB with zero warnings, `tools/series-lint` 0 errors 0 warnings, KUnit
under UML: 11 of 11 pass (`./tools/testing/kunit/kunit.py run
--kunitconfig=drivers/net/wwan/exynos_modem`).

U-Boot must change first (found 2026-11-15, by reading, not on the phone). Our U-Boot allocates
its kernel (128 MiB), ramdisk (128 MiB), fdt, script and fastboot buffers with `lmb_alloc()`
from the top of the RAM (`board/samsung/exynos-mobile/exynos-mobile.c`), which is where the
modem regions are, and an earlier local commit dropped the firmware reservations from its
device tree. Two consequences:
- whatever U-Boot loads can overwrite the bootinfo page at 0xaffff000;
- a kernel DTB with the three no-map regions asks U-Boot to reserve memory that holds the kernel
  and ramdisk it just loaded ("ERROR: reserving fdt memory region failed"), and the relocated
  initramfs can end up inside a region Linux will not map. The same kind of overlap hung
  fastboot when the ECT was reserved (CLAUDE.md, CPU DVFS facts).
Fix: U-Boot branch `modem` (worktree `src/u-boot-modem`, one commit on a3y17lte 2c2bc7ddcb,
"LOCAL: a3y17lte: keep the modem's memory free") reserves the same three regions in
`arch/arm/dts/exynos7870-a3y17lte-u-boot.dtsi`. It builds (`out/u-boot-modem`); no BOOT image
was made and nothing was flashed. Order on the phone: new U-Boot on BOOT (a3-flash, user's
approval) before the first kernel with the modem DT. If the kernel is installed first and does
not boot: Volume Down menu, "Boot Previous Kernel".

None of it has met the hardware. First-boot risks, in the order they would show:
1. The bootinfo page at 0xaffff000 may be garbage by the time Linux runs (U-Boot as above, or
   S-Boot not writing it on this boot path). The driver copies it blindly.
2. PMU state after S-Boot (CP_CTRL, CP2AP_MEM_CONFIG) is assumed to be what the vendor kernel
   finds.
3. The SMC arguments follow the vendor code path for CP_SECURE_BOOT off, but that path was only
   read, not run.
4. Mailbox RX in the mailbox framework adds a little latency to sampling the message word.

## 9. Open questions (answered on the phone, Part B)

- Current values of CP_CTRL_NS/S, CP_STAT, CP2AP_MEM_CONFIG, MODAPIF_CONFIG.
- Does the deliver_cp page survive U-Boot and early Linux? What is in it?
- Mailbox clock/reset state when Linux starts.
- Does the verify SMC lock the firmware area against AP writes (then never touch it afterwards)?
- Which RFS requests the CP really sends on this firmware (NV items only, or files too).

## 10. Part B checklist (needs Alpine)

1. Read-only: `peek` 0x10480030/34/38/3c/50/70 on the current kernel.
2. U-Boot from the `modem` branch on BOOT (build an image with `tools/build-uboot` from that
   tree; `tools/a3-flash BOOT`, user's approval). Then the `modem` kernel, and
   `dump 0xaffff000 0x1000` (now outside System RAM): it should look like a table, not like
   file data, and be the same after two cold boots. Compare with an S-Boot direct boot
   (Android) if in doubt.
3. Copy `out/modem-userspace` to the rootfs, run `modem-nv-setup`, `modprobe exynos_modem`.
4. First CP start, with the user's go: `ipc-modem` boots the modem; watch dmesg for the security
   request results (0 expected), "state 3" (booting), "state 4" (online).
5. samsungipcd on a pty, AT commands by hand: SIM (`AT+CPIN?`, PIN disabled on the SIM),
   registration (`AT+CREG?`), SMS, then data on `rmnet0`.
6. Read the NV write log (samsungipcd's output, "Writing nv_data"), then decide on real EFS
   read-write with the user.
7. MIF/INT `trans_stat` with the modem running, s2idle, `mem`, incoming SMS as wake-up.
8. Android: stock cbd + libsec-ril on the legacy nodes. S-Boot loads Android's boot.img
   directly, so the U-Boot problem does not exist there.
