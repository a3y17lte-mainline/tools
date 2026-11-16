# Stock RIL and IMS stack: reference notes

Status 2026-11-16: static analysis of the stock image (A320FLXXS9CTK2, Android 8.0). Nothing
here was run or sent to the modem. Purpose: document Samsung's telephony and IMS userspace as
the reference for the open modem daemon (work/notes/modem.md).

Files in this folder:
- `README.md` (this file): architecture, findings, how to regenerate.
- `ipc-layouts.txt`: generated. Every modem message the RIL builds or parses, with command
  number, type and field offsets (760 lines).
- `ims-ipc.md`: hand-decoded layouts of the messages IMS needs.
- `voice-engine.md`, `voice-engine-layouts.txt`: the modem's voice engine (group 0x16).
- `ims-stack.md`: Samsung's IMS app and native SIP stack, class by class.
- `ims-sequences.md`: the voice-engine message order for a call, registration and call
  setup as Samsung does them.

Inputs, all proprietary and untracked, under `work/system/extract/` (pulled from
`work/system/SYSTEM.img` with `debugfs -R "dump ..."`):

| File | From | What |
|---|---|---|
| `libsec-ril.so` | `/vendor/lib` | the RIL (2.4 MB, C++ symbols exported) |
| `ims/imsservice.apk`, `.vdex` | `/priv-app/imsservice` | IMS app; `res/raw/*.json` operator tables |
| `ims/libsec-ims.so` | `/lib` | native SIP stack (7.3 MB, 24,543 exported symbols) |
| `imsd`, `ims/vcd`, `apve.so` | `/bin`, `/lib` | IPsec helper, AT bridge, AP voice engine |

## Tools (tools/re/)

- `DumpDecomp.java`: Ghidra headless script, decompiles every function into one C file per
  class. `analyzeHeadless PROJ NAME -import LIB -scriptPath tools/re -postScript
  DumpDecomp.java OUTDIR [skip-regex]`. The RIL takes about 25 minutes.
- `fn DIR REGEX [-l]`: print functions from such a dump.
- `vtslot LIB CLASS OFFSET...|--all`: resolve virtual calls (`*(int *)this + 0x...`) to method
  names. Decodes Android's packed relocations (APS2).
- `rxmap DIR VTABLE.txt`: receive command map (main -> sub -> parser) from the dispatchers.
- `txmap DIR [CLASS]`: main/sub/type of every `IpcTx*` builder (heuristic; 388 of 477 parsed,
  the rest are CDMA and factory messages).
- `layouts DIR RXMAP TXMAP [CLASS]`: field offsets per message; produces `ipc-layouts.txt`.
- Java: `out/tools/jadx` (1.5.1). The dex files are carved from the vdex (magic `dex\n037`);
  jadx needs `-Pdex-input.verify-checksum=no`. Output: `work/system/extract/ims/java`.
- REA trap: after a cold boot the first Java start can exceed REA's probe timeout and the
  server keeps the failure. Kill this session's `rea mcp` process; the harness restarts it.

## RIL architecture (libsec-ril.so)

- `SecRil` owns one manager per domain: `SimManager`, `NetworkManager`, `DataCallManager`,
  `CallManager`, `SmsManager`, `SsManager`, `StkManager`, `PowerManager`, `MiscManager`,
  `ImsManager`, `SoundManager`, `PhonebookManager`, `ImeiManager`, plus region variants.
- `ReqRouter`/`ReqDispatcher` queue RIL requests; each manager's `HandleEvent()` switches on an
  internal event number and calls `Do*()`. Results come back through `On*Done()`.
- `ModemProxy` -> `IpcModem` -> `IpcProtocol41`: one `IpcTx*` builder and one `IpcRx*` parser per
  modem message. `IpcProtocol41::GetRxData` dispatches on the main command to a group function
  (`IpcRxSec`, `IpcRxNet`, ...), which dispatches on the sub command.
- `Nv` serves the RFS channel (NV items and files under /efs).
- Frame: u16 length, u8 mseq, u8 aseq, u8 main, u8 sub, u8 type; payload from offset 7.
  Request types: 1 exec, 2 get, 3 set, 4 confirm, 5 event. In notifications type 3 = NOTI.

Main command groups (receive side):

| Main | Group | Main | Group |
|---|---|---|---|
| 0x01 | power | 0x0d | data (GPRS/LTE) |
| 0x02 | call | 0x0e | SIM toolkit |
| 0x03 | CDMA data | 0x0f | configuration |
| 0x04 | SMS | 0x10 | IMEI |
| 0x05 | SIM (security) | 0x12 | SIM access profile |
| 0x06 | phonebook | 0x13 | factory |
| 0x07 | display (signal) | 0x16 | IMS / modem voice engine |
| 0x08 | network | 0x17 | eMBMS |
| 0x09 | sound | 0x20 | domestic (Korea) |
| 0x0a | misc | 0x22, 0x40 | QMI bridges (other modems) |
| 0x0b | service mode | 0x70 | IIL (not a modem group, see below) |
| 0x0c | supplementary services | | |

Protocol variant: the cellular modem is created with protocol version 2, the plain
`IpcProtocol41` (`ModemFactory::LoadModemConfiguration`). `IpcProtocol41Iil` (version 8, main
group 0x70) belongs to two extra built-in configuration entries named `imsd` and `epdgd` that
are only enabled with `CscFeature_IMS_EnableIMS`: a socket peer, not the modem. Which process
is that peer was not found: neither `imsd` (IPsec only), `libsec-ims.so` nor the Java app
contains IIL code. The IIL message set is still worth having as a list of what an IMS stack
asks a RIL for (registration state, ISIM authentication, preferred network, barring info).

## IMS architecture

```
imsservice.apk (Java, com.sec.internal.ims)        policy, operators, sessions
   | protobuf requests/notifications (SecImsIF, StackIF) over JNI
libsec-ims.so (C++)                                 SIP stack: reSIProcate + Samsung classes
   |-- SIP over the IMS PDN netdev (sockets)
   |-- /dev/socket/imsd  -> imsd: sets up the IPsec SAs (PF_KEY)
   |-- OemIPCHandler -> RIL "Multiclient" socket, raw IPC pass-through
   |        -> modem, main group 0x16: the modem's voice engine (CPVE)
   `-- SAEInterface -> libSamsungAPVoiceEngine.so: RTP and codecs on the AP
```

- **SIP is reSIProcate.** `libsec-ims.so` carries the open reSIProcate stack (4,262 `resip::`
  symbols, source paths `vendor/samsung/imscore/.../opensrc/resiprocate`) and Samsung's layer
  on top: `RegistrationManager`, `RegistrationSession`, `CallManager`, `CallSession`,
  `CallUacSession`, `CallUasSession`, `SdpCreator`, `SipMessageBuilder`, `SubscribeSession`,
  `UserAgentManager`.
- **Voice media runs in the modem on this phone.** `RegistrationManagerBase.getAudioEngineType()`
  returns 1 when the modem board name starts with `SHANNON`. Type 1 is the CP voice engine:
  `CPVEInterface`/`CpAudioEngineClient`/`OemIPCHandler` send `IPC_IMS_*` messages (CHANNEL,
  CODEC, RTP_MEDIA, PDN, ENGINE, DTMF, CALL_STATUS, SIGNALLING, SIM_INFO, OPTION, Sess_Refresh,
  SIP_INFO_ACB, GET_NET_INFO, GET_RADIO_ENVIRONMENT_INFO, Frame_TIME) to the modem as raw IPC
  through the RIL (`MiscManager::DoOemRawIpc` -> `IpcProtocol41::IpcTxRawIpc`). The modem then
  does RTP, the codec and the jitter buffer, and reports dedicated-bearer, RRC, DTMF, RTP
  timeout and statistics events back. The AP engine (`SAEInterface`) is the Wi-Fi calling
  path (`changeEPDGAudioPathForSlot`: SAE or CPVE).
- **The Java app uses ordinary Android services for the rest**: `ConnectivityManager.
  requestNetwork` for the IMS PDN (`PdnController`), the telephony ISIM authentication call
  for AKA (`SimManager.requestIsimAuthentication`, answered into the stack with
  `RequestUpdateAkaResp`), logical channels for ISIM access.
- **Per-operator behaviour is Java**: `imsservice/governor/RegistrationGovernor*.java` (23
  variants; `Eur` covers O2 CZ) and `GlobalSettingsRepo*.java`, on top of the JSON tables
  (`mnomap.json`, `imsprofile.json`, `globalsettings.json`).
- `vcd` is unrelated to IMS: it bridges AT commands between USB and the modem.

Java to native interface (`StackIF`, protobuf `SecImsIF`), the operations that exist:
registration (`RequestUACreation`, `RequestRegistration`, `RequestUADeletion`,
`RequestUpdateAkaResp`, `RequestUpdateSimInfo`, `RequestUpdatePani`, `RequestUpdateRat`,
`RequestNetworkSuspended`, `RequestSetPreferredImpu`), calls (`RequestMakeCall`,
`RequestAcceptCall`, `RequestRejectCall`, `RequestEndCall`, `RequestHoldCall`,
`RequestResumeCall`, `RequestUpdateCall`, `RequestModifyCallType`, `RequestTransferCall`,
`RequestMakeConfCall`, `RequestHandleDtmf`, `RequestProgressIncomingCall`,
`RequestStartLocalRingBackTone`), SMS (`RequestSendMsg`, `RequestReceiveSmsResp`,
`RequestSendRpAckResp`), media (`RequestStartMedia`, `RequestUpdateAudioInterface`,
`RequestRtpStatsToStack`, `RequestDeleteTcpClientSocket`), plus RCS, presence, XDM and video.

## Findings that matter for the open stack

1. Dedicated bearers: the modem manages them. The data call status notification 0x0D10 has a
   flag byte at offset 11 that the RIL only logs ("Dedicated bearer changed"); the detailed
   bearer events go to the IMS stack through the 0x16 group.
2. No PCO message exists in this protocol; P-CSCF addresses arrive in 0x0D09.
3. Logical channels are raw APDUs in 0x0506 (fixed 0x10e-byte frame); ISIM file reads use
   0x0505 with an AID.
4. `IpcRxCallVoiceRadioBearerHandover` (0x0233) is T-Mobile US only.
5. AOSP's ImsMedia module has an offload path (`AudioOffloadService`) onto the standard HAL
   `android.hardware.radio.ims.media.IImsMedia` (RtpConfig, AmrParams, EvsParams, DtmfParams,
   RtcpConfig, media quality). That HAL is the natural front end for the modem voice engine.

## Decisions for the open stack (user, 2026-11-16)

- No Samsung userspace blobs in the product; they are the reference only.
- VoLTE is the goal. Wi-Fi calling is not built now but must stay possible: keep the design
  extensible instead of hardcoding the LTE-only case.
- What that means in practice:
  - the daemon core has no Android types; front ends (radio HALs, `IImsMedia`, later a Linux
    one) are separate layers;
  - the IMS app treats the transport (IMS PDN on LTE now, an ePDG tunnel later) and the media
    engine (modem engine now, an AP engine later) as interfaces with one implementation each;
  - data calls carry the access network type and handover hooks from the start (0x0D15,
    0x0D16 exist in the protocol), even if only LTE is wired up;
  - the operator table keeps Samsung's per-network service lists (`network[].type`), so
    adding `wifi` is data, not code.

## Open

- Byte meanings in the voice-engine channel, codec and option messages, and line-by-line
  reads of Samsung's REGISTER and INVITE handling (ims-sequences.md, "Open").
- RIL request numbers (Samsung's OEM requests) to manager functions: the table was not
  extracted; managers use internal event numbers.
- The IIL peer process.
- RFS file commands beyond NV items (`Nv::ProcessRfsPacket`).
- Hand annotation of `ipc-layouts.txt`: offsets and widths are generated, meanings are not.
