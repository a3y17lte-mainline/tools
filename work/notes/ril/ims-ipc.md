# Modem: IMS-related Samsung IPC messages (from the stock RIL)

Status 2026-11-16: static reading only, nothing sent to the modem. Source: stock
`/vendor/lib/libsec-ril.so` (A320FLXXS9CTK2, sha256 3ca3b57a...838ea2), extracted from
`work/system/SYSTEM.img` to `work/system/extract/` (proprietary, untracked), decompiled with
Ghidra through REA. The library exports its C++ symbols: `IpcProtocol41` has 483 `IpcTx*` and
357 `IpcRx*` methods, one per message, so any message can be decoded the same way
(`nm -DC libsec-ril.so | grep IpcProtocol41::`).

## How stock does IMS

On the AP: `imsservice` (SIP), `/system/bin/imsd` (IPsec through PF_KEY),
`libSamsungAPVoiceEngine.so` (RTP + AMR/EVS over sockets). The modem supplies the services
below. There is no IMS channel in the link layer; SIP and RTP run over an ordinary PDN netdev.

## Frame header (7 bytes, as in libsamsung-ipc)

u16 length (with header), u8 mseq, u8 aseq, u8 main, u8 sub, u8 type. Offsets below count
from the start of the frame, so the payload starts at 7.

## Messages

| What | Command | Dir, type | Payload |
|---|---|---|---|
| AKA authentication | 0x0509 (`IPC_SEC_IMS_AUTH`) | AP->CP, 2 (GET) | @7 auth type, @8 RAND length, RAND, AUTN length, AUTN |
| AKA result | 0x0509 | CP->AP | @7 auth type (0-2), @8 status, @9 length-prefixed fields: type 0 three, type 2 four, type 1 two |
| Data profile | 0x0D1B (`IPC_GPRS_SET_APN_INFO`) | AP->CP, 3 (SET), 0xcb bytes | @7 profile id (`ConvertProfileId`: IMS 1, FOTA 2, default 3, CBS 4), @8 name[20] (`lte_ims`, `lte_internet`, `lte_emergency`, ...), @0x1c protocol (2 IPv4, 5 IPv6, 6 IPv4v6, 4 PPP), @0x1d APN[101], @0x82 user[32], @0xa2 password[32], @0xc2 auth (0 none, 1 PAP, 2 CHAP), @0xc3 P-CSCF request (1 IPv4, 2 IPv6, 3 both; only when the profile needs one), @0xc4 disabled (inverted "enabled"), three u16 from @0xc5 |
| PDP activation | 0x0D04 (`IPC_GPRS_PDP_CONTEXT`) | AP->CP, 3 (SET) | @7 mode (`ActivateContext`: 0 off, 1 activate, 3 IMS, 4 emergency, 5 eMBMS), @8 cid, credentials only with mode 1. Stock cids (`GetCidFromApnType`): IMS 1, internet 2, FOTA 3, CBS 4, emergency 5 |
| IP configuration with P-CSCF | 0x0D09 (`IPC_GPRS_IP_CONFIGURATION`) | CP->AP, 3 (NOTI) | @7 cid, @8 u16 field flags; bit 0: IPv4 @0x0a, DNS @0x0e, @0x12; bit 6: IPv6 @0x2a, DNS @0x6a, @0x7a; P-CSCF: 3 IPv4 @0x1e, 3 IPv6 @0x3a; if the frame is >= 0xa5 bytes, 5 more IPv4 @0xa4 and IPv6 @0xb8; flag 0x2000: u32 @0x9f; flag 0x4000: u8 @0xa3 |
| IMS registration state | 0x1608 | AP->CP, 3 (SET), 13 bytes | @7 status (0 not registered, 1 registered, 2 a third state, 3/4 emergency), @8 feature mask (1 voice, 2 SMS, 8 video), @9 network type (0/1, names not in the RIL), @10 ECMP state, @11 RAT (stock table from RIL radio technology: LTE 6, IWLAN/ePDG 5, UMTS family 1-4), @12 low byte of a reason field (VZW-only use). Sent by the RIL on every IMS registration change (`ImsManager::OnImsRegistrationChanged`) |
| SRVCC state | 0x0214 | both, 3 | @7 state (0-2) |
| IMS call list | 0x0206 (`IPC_CALL_LIST`) | AP->CP, 3 (SET) | @7 count; per call: u16 call type, id, direction (1 MO, 2 MT), state (1-6), multiparty, number length, 0x81, number |

SRVCC on Shannon, single SIM (read 2026-11-18, `CallManager::OnCallSrvccNotification`,
`IpcModem::SetCallList`, `GetTidAfterSrvcc`, `IpcTxCallSetCallList`):
- The modem's 0x0214 notification is ignored without a VoLTE call ("No VoLTE call").
- Started (0): the RIL sends 0x0206 SET with the IMS calls, each id replaced by the
  transaction identifier it gets on CS: one call 0; two calls: the active one 0 (without an
  active call the held one), the other 1; three: active 0, held 1, the third 2 (conference
  rules not copied). Call type u16 0x0100 voice. `SetCallList` sends for states 0 and 4
  only (4 = a resend when the radio comes back, EUR builds); completed (1) sends nothing.
- Completed (1): the calls are CS calls; the RIL fetches the current calls again.
  Failed (2): the handover marks are dropped, a call state change is raised.
- Each state goes on to the IMS service (the IIL "modem", `SetSrvccState`) and to Android
  as RIL_UNSOL_SRVCC_STATE_NOTIFY. Nothing shows 0x0214 SET going to the CP in this flow.
- Not sent on this phone: 0x1619 signalling info (`VoIpEngineProxy::SignalingInfo` skips it
  unless `persist.radio.multisim.config` is dsds/dsda; stock has `ss`), 0x1620 IMS SIM info
  (Java `SimManagerFactory`: dual IMS / DSDS only), an IMS call state to the modem
  (`IpcModem::SetImsCallStateNoti` is a stub returning -1).

The status field of the AKA result: 0 = success, 4 and 5 also carry data (likely the
synchronisation failure with AUTS: an inference), 1-3 are errors mapped through tables.

## Compared with libsamsung-ipc

- 0x0509 is only a name there (no structure, no code).
- Its `ipc_gprs_ip_configuration_data` is 22 bytes with an 8-bit flag field: the P-CSCF and IPv6
  parts are missing.
- 0x0D1B and the activation modes are in the fork's `shannon.h` since 2026-11-17.
- The IMS group 0x16 and 0x0214 do not exist there.

## SIM, data status and smaller messages (decoded 2026-11-16)

| What | Command | Dir, type | Payload |
|---|---|---|---|
| SIM application list | 0x050F | CP->AP | @7 count; 24-byte records from @8: app type, AID length, AID[16], PIN1 state @+0x12, PIN2 state @+0x13, four retry counters |
| File access with AID (ISIM files) | 0x0505 | AP->CP, 2 (GET) | @7 command, @8 app type, @9 AID length, @0x0a AID[16], @0x1a u16 file id, @0x1c path length in words, @0x1d path, @0x29 P1, @0x2a P2, @0x2b P3, @0x2c data |
| Raw APDU (logical channels) | 0x0506 | AP->CP, 2 (GET), fixed 0x10e bytes | @7 u16 APDU length (big endian), @9 APDU. Open = `00 70 00 00 01`, close = `00 70 80 <ch> 00`, transmit = CLA with the channel number in its low bits |
| Data call status | 0x0D10 | CP->AP, 3 (NOTI) | @7 cid, @8 state (1 active, 0x0a, 0x0b = two inactive variants), @9 fail cause, @10 flag, @11 dedicated-bearer-changed flag, @12 u32 retry time, @16 one more byte in long frames |
| Network registration | 0x0805 | CP->AP | @7 access technology, @8 domain (1-3), @9 state, @10 one byte, @0x0b u16 LAC, @0x0d u32 cell id, @0x11 reject cause, @0x12 u16 TAC, @0x14 VoLTE (PS voice) support, @0x15 IMS emergency support (both: 1 yes, 2 no, else unknown; stock log "VolteSupport %d ImsEmergencySupport %d TAC %d PhysicalCellId %d"), @0x17 u16 physical cell id. `NetRegState::IsVolteAllowed` = @0x14 == 1 |
| Access barring for IMS (SSAC) | 0x0819 | CP->AP | voice factor/time @7/@8 and video @0x0f/@0x10, repeated at @0x17.. (not checked field by field) |
| Serving network, with SSAC | 0x0803 | CP->AP | @0x19 u8 + @0x1a u16 and @0x1c u8 + @0x1d u16 barring factor/time pairs |
| IMS mode change | 0x1610 | CP->AP, 3 (NOTI) | @7 switch type |
| Avoid tune-away | 0x1612 | AP->CP, 3 (SET) | one byte |
| VoLTE test mode | 0x0828 | both | one byte |

Not present in this protocol: a PCO indication (the RIL's PCO code has no modem parser) and
any dedicated-bearer or TFT message. The modem owns dedicated bearers; the AP only sees the
flag in 0x0D10 and the bearer events of the voice engine (group 0x16).

`IpcRxCallVoiceRadioBearerHandover` (0x0233) is only used for T-Mobile US.

## The modem voice engine (group 0x16)

The IMS stack does not send these through the RIL's own builders: `libsec-ims.so` builds them
(`OemIPCHandler`) and the RIL passes them through as raw IPC. Message names: CHANNEL, CODEC,
RTP_MEDIA, PDN, ENGINE, DTMF, CALL_STATUS, SIGNALLING, SIM_INFO, OPTION, INFO, Sess_Refresh,
SIP_INFO_ACB, GET_NET_INFO, GET_RADIO_ENVIRONMENT_INFO, GET_VERSION, Frame_TIME. Layouts: to
be decoded from the `libsec-ims.so` dump. See README.md, "IMS architecture".

## IIL is not the modem

`IpcProtocol41Iil` (protocol version 8, main group 0x70) is used for two extra configuration
entries named `imsd` and `epdgd`, not for the cellular modem, which runs protocol version 2
(`ModemFactory::LoadModemConfiguration`). Its messages, for reference:

| Command | Dir | Meaning |
|---|---|---|
| 0x7001 | peer->RIL | IMS registration state: @7 status, @8 feature mask, @9..@0x10 flags and two u16, @0x11 IMPU length, @0x12 IMPU, @0x112 RAT |
| 0x7002 | peer->RIL, RIL->peer | ISIM authentication: RAND[16] @8, AUTN[16] @0x18; answer @7 = 1, @8 length, @9 data |
| 0x7004, 0x700b | RIL->peer | deregistration requests |
| 0x7006 | peer->RIL | preference (14 bytes from @7) |
| 0x7008, 0x7009, 0x700a | RIL->peer | emergency registration, deregistration, PDN info |
| 0x700e | RIL->peer | SSAC info |
| 0x700f | peer->RIL | retry network |
| 0x7012, 0x7013 | both | connected, phonebook init completed |
| 0x7014, 0x7015 | both | change/set preferred network type; IMS call state (@7, @8) |
| 0x7016 | RIL->peer | SIP suspend notification type |
