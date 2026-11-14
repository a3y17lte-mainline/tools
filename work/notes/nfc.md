# NFC (Samsung S3NRN80) (2026-11-14)

Status (2026-11-14): working in reader mode on Alpine. Tags are read (MIFARE Classic 1K tested,
UID/SENS_RES/SEL_RES), repeated power cycles and deep sleep (`mem`) pass. Kernel commits:
dt-bindings exynos-pmu NFC clock, clk exynos-clkout 7870 NFC clock, dt-bindings s3fwrn5 S3NRN80,
s3fwrn5 clock in core, s3fwrn5 S3NRN80 support, dts 7870 PMU clocks, dts a3y17lte NFC.
Userspace: tools/probes/nfc-poll (no neard in Alpine). Card emulation / eSE: not done (secure world).

## Hardware (stock rev02 DT, vendor kernel sec_nfc.c, stock SYSTEM)

- Chip: S3NRN80 (stock files sec_s3nrn80_firmware.bin / _rfreg.bin; vendor config SEC_NFC
  PRODUCT_N5, I2C). Vendor kernel = sec-nfc char device; all NCI/firmware logic is in the
  userspace HAL /vendor/lib/hw/nfc_nci.default.so (config /etc/libnfc-sec-hal.conf).
- I2C: hsi2c@138f0000 (mainline `&hsi2c4`, pins gpf2-0/1), address 0x27, 400 kHz.
- IRQ: gpa2-6, rising edge, pull-down (`nfc_int` in exynos7870-pinctrl.dtsi).
- FIRM / wake: gpd4-1 output, high = bootloader (firmware) mode, also the NCI wake line
  (`nfc_n5_firm`). = s3fwrn5 `wake-gpios`, active high.
- PD: gpc2-2. Stock puts it in function 2 = driven by the PMU (PMU_NFC_CLK_CTRL bit 0). It is also
  a plain GPIO (`nfc_en`, stock state "gpio-nfc"). Polarity: the vendor ven-gpio path writes 0 for
  ON, and the PMU path with PD_POLA = 0 clears PD for ON, so PD is active high = s3fwrn5
  `en-gpios` GPIO_ACTIVE_HIGH (s3fwrn5's "en" = 1 holds the chip off).
- CLK_REQ: gpc2-3 function 2 (`nfc_n5_clk_req`): the chip requests the reference clock from the PMU.
- PVDD: gpd2-1 output high at probe (low only in LPM charging) (`nfc_pvdd_en`).
- eSE: gpf4-0 pvdd + SPI on the secure gpc6/gpc7 banks: secure world, not touched.

## PMU_NFC_CLK_CTRL (PMU + 0x600c = 0x1048600c)

- Name from vendor pwrcal S5E7870-pmusfr.h. Bits (sec_nfc.h): 0 PD, 4 REQ_POLA, 5 PD_POLA.
  Only sec_nfc touches it (not pwrcal/sleep code).
- Vendor probe writes 0x10 (REQ_POLA, PD = 0 = on with PD_POLA = 0); power on/off then flips bit 0.
- S-Boot leaves it 0 (read 2026-11-14), and leaves every NFC pin an input: gpc2 CON 0x22 (only the
  i2c6 pins set), gpd2-1 and gpd4-1 inputs, PVDD therefore off. The kernel has to set up all of it.
- With PD as a GPIO, the PMU only needs REQ_POLA (0x10) once.

## Stock HAL open sequence (RE of nfc_nci.default.so, Ghidra via REA)

- `nfc_hal_open_sm`: power on in firmware mode (FIRM high), GET_BOOTINFO, map the bootloader
  version to a product code (table: 0x30 S3FRNR3, 0x50 S3FWRN5, 0x60 S3FWRN5P, 0x70 S3FWRN7,
  0x80 S3FWRN8, 0x81 S3FWRN81, 0x82 S3NRN82). FW_UPDATE_MODE=1 (< 2): no forced update.
- Product >= 0x70: switch to NCI mode and send prop FW_CFG first: `2F 28 01 <clk_speed>` with
  FW_CFG_CLK_SPEED = 0x12 from the conf (1 byte for 0x70/0x80 class; FWRN5-class sends 3 bytes
  type/speed/req). Product < 0x70: plain CORE_RESET.
- FW_CFG_RSP status (`nfc_hal_fw_sm` case 0x21): 0 ok -> CORE_RESET -> CORE_INIT; 1 length error;
  2 clock mismatch -> power cycle, send again; 3 "check the clock value" -> firmware update (we never
  do this); 5 bootloader without FW_CFG.
- So FW_CFG is sent on EVERY open, before CORE_RESET. Mainline s3fwrn5 sends a 3-byte FW_CFG
  (01 ff ff) only after a firmware download, i.e. never for us.

- FW_CFG_CLK_SPEED 0x12 = "26MHz PLL", clock source "AP direct 26MHz" (comments in the stock
  libnfc-sec-hal.conf; the conf is for N7/N80/N81 firmware, "SEN81DGX (Include eSE), 26MHz PLL").

## Stock NCI stack: proprietary RF protocols (REA, libnfc-nci.so + libnfc_nci_jni.so)

- `nfa_proprietary_cfg` (libnfc-nci .rodata 0xb5b7f) = 00 81 82 80 8a 81 90 70 74 f4: AOSP layout
  {18092_active, b_prime, dual, 15693, kovio, mfc, ...}.
- JNI `NfcTag::discoverTechnologies(tNFA_ACTIVATED&)`: protocol == cfg[5] (0x81) -> "Mifare Classic"
  (EXTNS_MfcInit, NXP's MFC extension); == cfg[3] (0x80) -> tech 5 = ISO 15693; == cfg[4] (0x8a)
  -> Kovio. libnfc-nci `RW_SetActivatedTagType` uses cfg[3] for rw_i93 (tech mode 6).
- So the S3NRN80 reports MIFARE Classic as RF protocol 0x81 and ISO 15693 as 0x80, never the NCI
  standard T5T (0x06). Linux NCI needs ops->get_rfprotocol for both.
- Prop NTF OID 0x3F (`nci: unsupported ntf opcode 0xf3f` at poll start) = "CE Error Notification"
  (JNI nfaVSNtfCallback -> RoutingManager::CeErrorEventHandler): card-emulation only, ignore.

## Hardware test (2026-11-14, Alpine)

- GET_BOOTINFO: hw_version 80 00 01 00, sector 4096, page 512, frame 262 -> product class 0x80.
- NCI up (CORE_RESET + CORE_INIT) works from en-gpios/wake-gpios alone.
- With PMU_NFC_CLK_CTRL = 0x10 and FW_CFG `2F 28 01 12` (status 0) before CORE_RESET, as stock:
  a MIFARE Classic 1K card was read (RF protocol 0x81, SENS_RES 0x0004, SEL_RES 0x08, 4-byte UID).
  Without them the same card was not found. No A/B split of the two: we mirror stock and do both.
- Final series: nfc_clk 26 MHz enabled by 5-0027 in clk_summary, register 0x10 after probe and
  after a deep-sleep cycle; 3/3 down/up/poll rounds read the card; no firmware-load attempt.

## Kernel design (final)

- PMU: exynos-clkout gets an Exynos7870 variant with clock 1 = "nfc_clk", a gate on REQ_POLA
  (bit 4) with oscclk as parent; no CLKOUT clock 0 (7870 mux inputs unknown). The register is
  saved/restored over system sleep like PMU_DEBUG. dtsi: PMU #clock-cells = <1>, clocks = oscclk.
- s3fwrn5: compatible "samsung,s3nrn80" (clocks required). The core takes the optional clock
  (moved from i2c.c); for the S3NRN80 the rate must be 26 MHz -> FW_CFG code 0x12, sent from
  ops->init (after power-up in NCI mode, before CORE_RESET). get_rfprotocol: 0x80 -> ISO15693,
  0x81 -> MIFARE (like pn533 for Classic). Prop NTF 0x3f consumed. Firmware download skipped.
- Board: hsi2c4 nfc@27, en-gpios gpc2-2 (PD as GPIO instead of the PMU PD function), wake gpd4-1,
  IRQ gpa2-6 rising, PVDD gpd2-1 as an always-on fixed regulator (binding has no supply).
- Open for upstream review: clkout refactor might have to be split; PVDD supply property.

## Mainline driver (drivers/nfc/s3fwrn5)

- Firmware safety: if sec_s3fwrn5_firmware.bin exists, post_setup flashes the chip when the
  version differs. Never install that file (work/firmware/README says so); without it the driver
  skips firmware mode.
- NCI core open order: ops->open -> ops->init -> CORE_RESET -> ops->setup -> CORE_INIT ->
  ops->post_setup. Commands can't be sent from ops->open (cmd_cnt is only set after it), so
  FW_CFG goes in ops->init to match the HAL.

## Card emulation (HCE) and MIFARE Classic writes (2026-11-14, from code + REA)

- HCE: not possible through the kernel NFC stack. The NCI core only listens for NFC-DEP (P2P)
  (`nci_start_poll` tm_protocols), no ISO-DEP listen mode and no listen-mode routing. The chip
  can do it (stock JNI RoutingManager, CE error NTF 0x3f), so on Android it needs the AOSP
  userspace stack (libnfc-nci) talking raw NCI through a HAL: s3fwrn5 unbound, chip reached via
  i2c-dev + GPIO chardev (IRQ as line events), plus something kernel-side keeping nfc_clk on.
- MIFARE Classic read/write: the stock JNI uses NXP's extension framing on the normal data channel
  (phNciNfc_SendMfReq): authenticate = `40 <block> <flags: key B 0x80 | 0x10 embedded key> [6-byte key]`,
  exchange = `10` + raw MIFARE command (`30 <blk>` read, `A0 <blk>` + 16 bytes write).
  nci_transceive passes raw socket payloads through unchanged, so a userspace tool on a raw NFC
  socket (connect with NFC_PROTO_MIFARE) can use the same frames. Response layout: see stock
  Mfc_RecvPacket (not yet RE'd). Not tested; neard's MIFARE plugin expects pn533-style raw
  commands, not this framing.
