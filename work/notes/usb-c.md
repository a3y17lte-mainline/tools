# S2MU004 USB-C / PD controller (i2c0 0x3c): notes for a clean TCPM driver (research 2026-11-12)

Research only: nothing was written to the phone. Sources:
- vendor `drivers/ccic/s2mu004-usbpd.c` (PHY layer; Samsung's own PD stack is `usbpd*.c`),
  `include/linux/ccic/usbpd-s2mu004.h`, `usbpd_manager.c` (capabilities);
- the stock DT (`usbpd-s2mu004@3C`);
- our read-only survey `s2mu004-regs.md` (PD registers, read once, cable in, bootloader state).

Samsung's PD stack (policy engine, notifier chains, "ccic" sysfs, water detection, RID factory
cables, dual_role_usb) is NOT to be ported. Mainline TCPM does the policy; we only need a
`struct tcpc_dev` low-level driver, like `drivers/usb/typec/tcpm/fusb302.c`.

## 1. What the A3 needs

- USB 2.0 only (dwc3 "samsung,exynos7870-dwusb3", `usb-role-switch` already in exynos7870.dtsi).
  No alt modes, no DisplayPort, no SuperSpeed mux, no orientation switch.
- **No VCONN.** The stock DT has neither `vconn-en` nor `regulator-en`, so the vendor driver never
  sources VCONN on this phone. Report VCONN as unsupported (`set_vconn` returns -EOPNOTSUPP, or
  no-op for off).
- Roles: dual-role power and data (DRP), try.SNK preferred (phone = sink normally, source with an
  OTG adapter/device).
- PD capabilities the stock firmware used (usbpd_manager.c:56-107, stock `battery,max_input_voltage`
  = 5000 mV, `max_input_current` = 1500 mA):
  - Source: one fixed PDO 5 V 500 mA, DRP + DR swap + USB comm capable.
  - Sink: fixed 5 V (op 500 mA) plus a variable 5 V..`sink_cap_max_volt`; the battery driver then
    capped the request to 5 V. So on the A3, **PD only ever negotiated 5 V**. Fast charging is
    Samsung AFC over D+/D- (charging.md), not PD.
  - Mainline DT: `sink-pdos = <PDO_FIXED(5000, 1500, PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP |
    PDO_FIXED_USB_COMM)>`, `source-pdos = <PDO_FIXED(5000, 500, ...)>`, `op-sink-microwatt =
    <2500000>`. Raise the sink current only after charger tests (input limit is a charger write).

## 2. Wiring (stock DT)

- I2C: &i2c0 (13830000, also has the fuel gauge 0x3b), address 0x3c.
- IRQ: `usbpd,usbpd_int = <&gpa0 5 0>` with pinctrl `usbpd-irq` = gpa0-5, function 0xf (EINT),
  no pull. Vendor requests it **falling edge** (IRQF_TRIGGER_FALLING, s2mu004-usbpd.c:2283).
  Use `interrupts-extended = <&gpa0 5 IRQ_TYPE_EDGE_FALLING>`, but check the level at idle first
  (section 7): if the line stays low while status bits are pending, level-low + IRQF_ONESHOT is the
  safer choice (the chip has 7 status registers; an edge can be lost between reads).
- No supplies, no reset GPIO in the stock DT. The chip is part of the S2MU004 package, powered
  from the PMIC side.
- VBUS sensing and VBUS output are NOT in this I2C device: they are in the S2MU004 main block
  (0x3d on &i2c4): charger CHGIN status / MUIC VBUS IRQs, and the charger's OTG boost mode.
  See section 5.

## 3. Register map (from usbpd-s2mu004.h, verified against the vendor code)

| reg | name | meaning |
|---|---|---|
| 0x00 | RESET_REG_00 | value constant "0" used for resets; not a register used directly |
| 0x01 | PD_CTRL | bit0 LP_LDO_D, bit1 LPM_EN (low-power mode) |
| 0x02 | PD_CTRL_2 | [7:4] CC_OCP threshold (vendor sets 15 = 575 mV) |
| 0x18 | PLUG_CTRL_PORT | [1:0] mode 1 DFP / 2 UFP / 3 DRP, [5:4] Rp select 0 = off(Rp0) / 1 = 80 µA (default USB) / 2 = 180 µA (1.5 A), bit6 DETECT_BAT_DISABLE, bit7 DETECT_OCP_DISABLE |
| 0x19 | PLUG_CTRL_MSG | bit5 data role (1 = DFP), bit6 power role (1 = source): the role bits put into GoodCRC/headers by hardware |
| 0x1E | PLUG_CTRL_SET_RD | Rd-side comparator threshold (enum CCIC_THRESHOLD_SEL) |
| 0x1F | PLUG_CTRL_SET_RP | Rp-side comparator threshold |
| 0x26 | PLUG_CTRL_CC_HOLD | bit0 hold CC state (used during OTG VBUS re-boost) |
| 0x27 | PLUG_CTRL_RpRd | bit2 FSM_MANUAL_EN, bit3 Rp/Rd plug select (1 = Rp), bit4 VCONN manual en, bit5 VCONN on CC1, bit6 VCONN on CC2, bit7 Rp/Rd manual en. Rd sink = 0x80, Rp source = 0x88 |
| 0x28 | PLUG_CTRL_CC12 | [3:0] FSM manual input (2 = ATTACHED_SNK, 6 = ATTACHED_SRC), bit4 CC manual en, bit5 CC1 on, bit6 CC2 on: selects the CC used for BMC (polarity). 0x10 alone = PD comms off |
| 0x2E | PLUG_CTRL | (not decoded by vendor code; survey 0x20) |
| 0x2F | CTRL | (survey 0x22) |
| 0x3E-0x44 | INT_MASK0..6 | 1 = masked, written as one 7-byte block |
| 0x90 | MSG_SEND_CON | bit0 SEND_MSG_EN, bit1 OP_MODE (always set), [4:2] SOP type 0 SOP, 1 SOP', 2 SOP'', 3/4 debug, 5 Hard Reset, 6 Cable Reset |
| 0x91/0x92 | MSG_HEADER_L/H | TX and RX message header (same registers) |
| 0x93-0xAE | MSG_OBJECT0..6 | 7 data objects x 4 bytes, little endian, TX and RX |
| 0xB2 | ADC_STATUS | [7:5] RID (factory cable resistor); ignore |
| 0xB3 | PLUG_MON1 | [2:0] CC1 monitor, [5:3] CC2 monitor |
| 0xB4 | PLUG_MON2 | bit1 attach done, bit2 sink selected, bit3 source selected (0x06 = attached as sink, 0x0A = as source) |
| 0xE0-0xE6 | INT_STATUS0..6 | clear-on-read; read all 7 in one block |
| 0xF7 | ETC | bit1 soft reset (write 1 then 0) |
| 0xF8 | ID_MONITOR | [2:0] hardware MessageID counter |

Interrupt bits (status = mask layout):
- STATUS0 (0xE0): b0 MSG_ACCEPT, b1 MSG_GOODCRC, b2-b7 VDM attention/exit/enter/disc-mode/svid/id.
- STATUS1 (0xE1): b0 PR_SWAP, b1 DR_SWAP, b2 GET_SNK_CAP, b3 GET_SRC_CAP, b4 PS_RDY, b5 REJECT,
  b6 GOTOMIN, b7 PING.
- STATUS2 (0xE2): b0 WAKEUP, b2 SOFT_RESET, b3 REQUEST, b4 SNK_CAP, b5 SRC_CAP, b6 WAIT, b7 VCONN_SWAP.
- STATUS3 (0xE3): b5 UNS_CMD_DATA.
- STATUS4 (0xE4): b1 MSG_ERROR, b2 MSG_SENT, b3 MSG_PASS, b4 USB_DETACH, b5 PLUG_IRQ (attach),
  b6 CC12_DET_IRQ.
- STATUS5 (0xE5): b2 HARD_RESET received.

Comparator threshold codes (CCIC_THRESHOLD_SEL): 2..20 = 128..685 mV in ~21-43 mV steps, 35..57 =
1328..2271 mV in ~43 mV steps (full table in the header, lines 270-315). Vendor normal setting:
SET_RD = 9 (428 mV), SET_RP = 52 (2057 mV) with Rp 180 µA; with Rp 80 µA SET_RD = 4 (214 mV),
SET_RP = 42 (1628 mV).

## 4. How the chip works (what the vendor driver shows)

- **The CC state machine and DRP toggling run in hardware.** Writing mode DRP (0x18[1:0] = 3, Rp
  180 µA) and clearing LPM_EN makes the chip toggle by itself; on attach it raises PLUG_IRQ and
  PLUG_MON2 says sink (0x06) or source (0x0A). Detach raises USB_DETACH. Attach and detach can
  arrive in the same read ("atomic" re-plug): handle detach first, then attach.
  This maps onto TCPM's `start_toggling` exactly like fusb302's hardware toggling (TOGDONE).
- **PD PHY: hardware does GoodCRC, CRC and the MessageID counter.** The chip classifies received
  messages into the STATUS0-2 bits and also sets STATUS4 MSG_PASS for any received message. TX:
  write header (0x91-92) + objects, then MSG_SEND_CON = 0x03, then 0x02 (pulse SEND_MSG_EN).
  Completion: STATUS4 MSG_SENT or STATUS0 MSG_GOODCRC = success; STATUS4 MSG_ERROR = failure.
- **MessageID:** vendor overwrites the header's MessageID with ID_MONITOR[2:0] before sending
  (s2mu004-usbpd.c:766). TCPM keeps its own counter. Open question: does the chip check or replace
  the ID? Safest first version: do what vendor does (patch the ID from 0xF8 into the header) and
  log a mismatch with TCPM's ID; revisit if TCPM's retry logic gets confused.
- **Hard reset:** TX = MSG_SEND_CON 0x16 (SOP HardRST | OP_MODE), 5 µs, 0x17, 1 µs, 0x02.
  RX = STATUS5 HARD_RESET: vendor turns CC comms off, soft-resets the PD block (0xF7 = 2 then 0),
  then lets the policy engine handle it. Also: after a received hard reset the chip **enters LPM
  by itself**; vendor detects LPM_EN set on CC12_DET_IRQ and forces normal mode again after 300 ms.
- **CC comms on/off** (`set_cc_control`): 0x28 bits[6:4] = 0 -> automatic CC selection, PD comms
  on; 0x28 bit4 alone -> manual with no CC -> comms off. Used around detach and hard reset.
- **Polarity:** `assert_rd` (s2mu004-usbpd.c:428) picks the CC whose PLUG_MON1 field reads 2 and
  sets 0x28 = CC1 manual on (0x30) or CC2 manual on (0x50).
- **Rp current level as sink** (`rp_check`, :1740): force the FSM to ATTACHED_SNK (0x28[3:0] = 2,
  0x27 bit2), move SET_RP to 1328 mV, wait 20 ms, read MON1; if both CC read "Ra" the source is
  Rp 3.0 A; else move SET_RP to 685 mV, wait, if either CC reads "Rd" -> default USB current, else
  1.5 A. Restore SET_RP 2057 mV and release the manual FSM. TCPM `get_cc` needs RP_DEF / RP_1_5 /
  RP_3_0, so a clean driver runs this sequence once after attach as sink and caches the result
  (and on a CC12_DET_IRQ while attached, since a source may change Rp to signal SinkTxOk).
- **CC monitor encoding** (PLUG_MON1, 3 bits per CC): the vendor compares to its USBPD_Rp = 1,
  USBPD_Rd = 2, USBPD_Ra = 4 (usbpd_msg.h:206). The names describe the *partner side seen by the
  comparators* inconsistently (as sink, the connected CC reads 2 "Rd"). Survey with the PC cable in
  (we are sink): MON1 0xB3 = 0xE2 -> CC1 = 2 (connected), CC2 = 4 (open), MON2 = 0x26 (attached as
  sink). Confirm the encoding with the checks in section 7 before trusting it.
- **LPM (low-power mode):** 0x01 bit1 + mode DFP with Rp off. All PD IRQs masked. The vendor
  leaves LPM only when the **MUIC** (0x3d) reports a Type-C attach (its ID-pin ADC reads 0, or VBUS
  appears) through a notifier (`type3_handle_notification`, s2mu004-usbpd.c:1598), and re-enters it
  on MUIC detach. That cross-chip dance is the vendor's power optimisation. At boot the bootloader
  leaves the chip in LPM (survey: 0x01 = 0x03, 0x18 = 0xC1).
  **Clean driver v1: no LPM.** Probe takes the chip out of LPM and stays in normal DRP. Measure the
  idle current difference (fuel-gauge current, cable out) before deciding whether LPM is worth an
  extcon/notifier link from the MUIC driver later.
- **OTG over-current workaround** (`CHECK_OCP`, :1663): when the MUIC reports OCP while sourcing
  VBUS, vendor holds CC (0x26 bit0), drops Rp to 80 µA, cycles the boost off/on. Skip in v1; an
  OTG fault from the charger should just turn VBUS off and report it.
- Water detection (MUIC + PD), RID (factory jigs), the "619k" factory checks: skip.

## 5. Integration on the A3 (mainline pieces)

- **VBUS presence** (`tcpc_dev.get_vbus`): the PD chip has no VBUS status. Sources on 0x3d:
  charger STATUS0 CHGIN[7:5] (valid = 5, or 3 = valid alt, see mainline s2mu004.h) and the MUIC
  VBUSON/VBUSOFF IRQs (already used by s2m-charger's S2MU004 variant). Proposal: the tcpc driver
  gets the charger's power_supply by phandle (`samsung,charger = <&...>` or the generic
  `power-supplies`), registers a power_supply notifier, calls `tcpm_vbus_change()` on change and
  answers `get_vbus` from the supply's ONLINE (or PRESENT) property. The charger driver already
  tracks VBUS this way (vbus-on/vbus-off IRQs), so this needs no new hardware access. An extcon
  from the MUIC driver would also work, but it mixes the BC1.2 type into VBUS sensing.
- **VBUS output** (`set_vbus`): S2MU004 charger OTG boost (charging.md section 4) exposed by
  s2m-charger as a regulator ("otg-vbus", 5 V fixed). The connector node gets
  `vbus-supply = <&otg_vbus>`; the tcpc driver enables/disables it in `set_vbus(source=true)`.
  When `set_vbus(charge=true)` (sink) nothing to do: the charger runs in CHG mode anyway.
- **Data role**: TCPM calls `usb_role_switch_set_role()` through the connector's port graph:
  connector port@0 (HS) -> `&usbdrd` dwc3 `usb@0` port. The dwc3 node already has
  `usb-role-switch`; remove the board's `role-switch-default-mode = "peripheral"` override once
  the tcpc driver probes (keep it until then: USB networking depends on it).
  Also tell the PD chip: `set_roles` writes 0x19 bit5 (data) and bit6 (power), used by hardware in
  GoodCRC/headers.
- **Input current from the partner**: TCPM registers a `tcpm-source-psy-*` power supply with
  CURRENT_MAX (Rp level or PD contract) and USB_TYPE C/PD. The S2MU004 charger driver should list
  it (via `power-supplies`/`supplied-from` on the charger node, or `power_supply_get_by_reference`)
  and take max(BC1.2 type limit, Type-C/PD limit), see charging.md.
- **USB D+/D- switch**: the MUIC (0x3d SW_CTRL 0xCA) routes D+/D- to the AP USB PHY or open; it
  must say USB for both gadget and host. Today it works because the bootloader/auto mode left it on
  USB; read it in section 7 before any MUIC driver writes it.

### Proposed DT (sketch, not checked against dtbs_check yet)

```dts
&i2c0 {
	typec@3c {
		compatible = "samsung,s2mu004-usbpd";
		reg = <0x3c>;
		interrupts-extended = <&gpa0 5 IRQ_TYPE_EDGE_FALLING>;
		pinctrl-0 = <&usbpd_irq>;
		pinctrl-names = "default";

		connector {
			compatible = "usb-c-connector";
			label = "USB-C";
			data-role = "dual";
			power-role = "dual";
			try-power-role = "sink";
			vbus-supply = <&otg_vbus>;   /* from s2m-charger */
			sink-pdos = <PDO_FIXED(5000, 1500, PDO_FIXED_DUAL_ROLE |
					       PDO_FIXED_DATA_SWAP | PDO_FIXED_USB_COMM)>;
			source-pdos = <PDO_FIXED(5000, 500, PDO_FIXED_DUAL_ROLE |
						 PDO_FIXED_DATA_SWAP | PDO_FIXED_USB_COMM)>;
			op-sink-microwatt = <2500000>;

			port {
				typec_hs: endpoint {
					remote-endpoint = <&usb_hs>;
				};
			};
		};
	};
};

&usbdrd { usb@0 { port { usb_hs: endpoint { remote-endpoint = <&typec_hs>; }; }; }; };
```
Binding: new `Documentation/devicetree/bindings/usb/samsung,s2mu004-usbpd.yaml` (I2C device,
`connector` child via usb-c-connector.yaml, interrupts, a phandle to the charger for VBUS sensing).
Check against `fcs,fusb302.yaml` and `richtek,rt1711h.yaml` for the expected shape.

## 6. tcpc_dev op mapping

| op | implementation |
|---|---|
| init | clear pending status (read 0xE0-0xE6), exit LPM (0x01 bit1 = 0), soft reset 0xF7, CC_OCP 575 mV (0x02[7:4] = 15), SET_RD 428 mV, SET_RP 2057 mV, 0x27 = 0, 0x28 = 0 (vendor reg_init :2232), mask all PD IRQs except STATUS4 attach/detach |
| get_vbus | from the charger power_supply (section 5) |
| get_current_limit | optional; leave to the charger (BC1.2) |
| set_cc | Rd: 0x27 = 0x80 (manual Rd), mode UFP. Rp(level): 0x27 = 0x88, 0x18 Rp select (DEF -> 80 µA with its thresholds, 1.5 A -> 180 µA; 3.0 A unsupported), mode DFP. Open: 0x27 = 0, 0x18 = DFP + Rp0 (vendor `set_detach`) |
| get_cc | MON1 fields + cached Rp level from the rp_check sequence (as sink); as source, MON1 "Rd" -> TYPEC_CC_RD, "Ra" -> TYPEC_CC_RA |
| set_polarity | 0x28 = 0x30 (CC1) or 0x50 (CC2) |
| set_vconn | not supported (no VCONN on the A3); return 0 for off, -EOPNOTSUPP for on |
| set_vbus | regulator enable/disable on the OTG boost |
| set_pd_rx | on: unmask STATUS0-2/4/5 message bits, CC comms auto (0x28[6:4] per polarity); off: mask them, 0x28 = 0x10 |
| set_roles | 0x19 bit5 data role, bit6 power role |
| start_toggling | 0x18 = DRP + Rp 180 µA, LPM off, unmask PLUG_IRQ/USB_DETACH; on PLUG_IRQ read MON2 and report via tcpm_cc_change |
| pd_transmit | header (MessageID from 0xF8, see section 4) + objects, MSG_SEND_CON pulse; hard reset via SOP 5; completion from MSG_SENT/GOODCRC (success) or MSG_ERROR (failed) -> tcpm_pd_transmit_complete |
| RX path | on MSG_PASS (or any STATUS0-2 message bit): read header 0x91-92 + n objects -> tcpm_pd_receive(SOP); STATUS5 HARD_RESET -> tcpm_pd_hard_reset |
| set_bist_data, enable_frs, auto discharge, contaminant | not needed |

Unknowns to settle on the phone (read-only first, then with the driver under test):
1. Exact PLUG_MON1 encoding per partner type (sink device, PC, charger, nothing).
2. Whether MSG_PASS fires for every received message or only "unsolicited" ones, and whether
   GoodCRC-only completions arrive as STATUS0 b1 or STATUS4 b2 (vendor accepts both).
3. Whether hardware retries on missing GoodCRC (TCPM must not retry on top if it does: check how
   many times a message goes out with a PD analyser-less test: count MSG_ERROR latency).
4. MessageID handling (section 4).
5. IRQ trigger (edge vs level) and whether the chip deasserts after the 7-byte status read.

## 7. Read-only checks (when the phone is free; no writes, no status reads)

The INT_STATUS registers 0xE0-0xE6 are clear-on-read: DO NOT read them (they would eat events of a
future driver only, but keep the rule consistent with s2mu004-regs.md). With nothing bound to 0x3c
no `-f` is needed. On pmOS: `sudo i2cget -y 0 0x3c 0xb3` etc. (i2c-0 = 13830000; check the bus
number in /sys/bus/i2c/devices/*/name first).

Registers to read in each state: 0x01 0x02 0x18 0x19 0x1e 0x1f 0x26 0x27 0x28 0x2e 0x2f
0x3e-0x44 0xb2 0xb3 0xb4 0xf7 0xf8, plus the gpa0-5 level (`peek 0x139f0044`, bit 5; gpa0 DAT).
States the user can provide: (a) nothing plugged, (b) PC USB (C-to-A cable), (c) wall charger,
(d) C-to-C cable to a PD charger or laptop if available, (e) OTG adapter with a USB stick/mouse.
Expected: in LPM (0x01 = 0x03) MON2 still shows attach as sink for (b)-(d); for (e) probably
nothing, because the chip is in DFP/Rp0 LPM, which is why vendor needs the MUIC wakeup.

## 8. Order

1. s2m-charger: OTG boost regulator for S2MU004 (needs the user's OK: it is a charger write,
   although it only switches mode 3 -> 6 and back).
2. MUIC/extcon S2MU004 variant (charging.md) so D+/D- switching and BC1.2 are owned by a driver.
3. tcpc driver `drivers/usb/typec/tcpm/s2mu004-tcpc.c` (name TBD, e.g. `tcpci`-less like fusb302),
   bring-up as sink only first (try-power-role sink, source-pdos absent), then DRP + OTG.
4. Drop `role-switch-default-mode = "peripheral"` once host/device switching works.
