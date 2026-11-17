# The modem's voice engine (CPVE), IPC group 0x16

Status 2026-11-16: static reading of the stock `libsec-ims.so`. Nothing sent to the modem.
Generated field skeletons: `voice-engine-layouts.txt`.

On Shannon modems the VoLTE voice media (RTP, RTCP, codec, jitter buffer) runs in the modem.
The IMS stack on the AP only does SIP and tells the modem what to do. Audio then takes the
modem's PCM path, the same as a circuit-switched call.

## Who sends what

- Java picks the engine: `RegistrationManagerBase.getAudioEngineType()` = 1 when the modem
  board name starts with `SHANNON`.
- `VoIpEngineProxy` -> `CPVEInterface` -> `CpAudioEngineClient` (`SAE_*` calls) ->
  `OemIPCHandler` builds the IPC frames.
- `ImsSignalingIpcManager` sends the call-status and signalling hints.
- Transport: the RIL's client socket ("Multiclient"), request `InvokeOemRequestHookRaw` with
  the bytes `0f 01 <length, u16 big endian> <IPC frame>`; the length is the frame length + 4.
  The RIL forwards the frame unchanged (`MiscManager::DoOemRawIpc` ->
  `IpcProtocol41::IpcTxRawIpc`) and returns answers and notifications the same way
  (`OnRawIpcReceived`). An open daemon sends these frames itself.

## Messages (AP -> modem)

| Command | Type | Function | Purpose |
|---|---|---|---|
| 0x1601 | set | `ImsSetChannel` | open/modify/close an RTP channel (0x34 bytes) |
| 0x1601 | event | `ImsStartEvent` | start event reporting |
| 0x1602 | get/set | `ImsGetCodecList`, `ImsSetCodec` | codec for a channel (0x41 bytes) |
| 0x1603 | set | `ImsSetRtpMedia` | media direction and state, 4 bytes |
| 0x1604 | set/exec | `ImsSetDtmf`, `ImsHandleDtmf` | DTMF payload type setup; send a DTMF event |
| 0x1605 | get/set | `ImsGetOption`, `ImsSetOption` | RTP/RTCP options and statistics (0x4e bytes) |
| 0x1606 | set | `ImsSetPdnIp` | the IMS PDN: address and interface name |
| 0x1607 | exec/get/set | session refresh: init (3), start (1), stop (2), CSeq | the modem sends SIP session refreshes itself while the AP sleeps |
| 0x1609 | set | `ImsEngineSet` | engine on/off, 2 bytes |
| 0x160a | get/set | `ImsFrameTimeGet/Set` | RTP/NTP time reference |
| 0x160c | event | `ImsNotiInfo` | free-form info block (100 bytes) |
| 0x160f | get | `ImsSendIPC` | generic block |
| 0x1611 | event | `ImsSIPInfoForACB` | access barring hint |
| 0x1612 | set | `ImsSetCSTuneAway` | avoid tune-away (dual SIM) |
| 0x1618 | get | `ImsGetNetworkInfo` | network info report |
| 0x1619 | set | `ImsSignalingInfo` | SIP signalling in progress (message type, state) |
| 0x161b | get | `GetVersion` | engine version string |
| 0x161c | set | `ImsDeleteCpClientSocket` | close a modem-side TCP client socket |
| 0x1620 | set | `ImsSimInfo` | SIM/slot info |
| 0x1621 | get | `ImsGetRadioEnvironmentInfo` | radio environment report |

Sent in the call group by the IMS stack: 0x027c IMS call status, 0x027d refresh PANI,
0x027e CS tune-away, 0x0280 signalling info.

## Answers and notifications (modem -> AP)

Read from `onUnsol()` (a plain C function; dump in
`work/system/extract/decomp-secims-global`).

Answers:
- Data answers (type 2) come back with the request's own command for 0x160f, 0x1618, 0x161b
  and 0x1621.
- Every other request is acknowledged by the generic response frame: @7 main and @8 sub of
  the request, @10 u16 result; 0x8000 = success, anything else goes through `MapIPCError`.
  The sender blocks until it arrives (`OemIPCHandler::Wait`).

Notifications (type 3), main 0x16:

| Sub | Content | Layout |
|---|---|---|
| 0x05 | media event | @7 event (1-9: RTP/RTCP timeout, statistics, codec bit-rate change, DTMF received, ...), @8 channel, @9 data (statistics block for event 0x66 after conversion) |
| 0x07 | session refresh event | @7 u32, @0x0b event |
| 0x0a | frame time | @7 event (2, 6), @8, @0x0c, @0x10 three u32 (RTP/NTP times), @0x14 channel |
| 0x0b | dedicated bearer | @7 bearer id, @8 QCI, @9 event (2, 3), @0x0a and @0x12 two u64 (bit rates; the log prints "qci, gbr_dl"); frames of 0x1b bytes or more add @0x1a, @0x1b, @0x1c packet filter count and up to 16 filters of 26 bytes from @0x1d (the TFT) |
| 0x0e | RRC connection event | @7 event |
| 0x10 | IMS mode change | parsed by the RIL (ims-ipc.md) |

So the modem does report the dedicated bearer with its QoS and packet filters, in this group.

## Order of operations (partly read)

`VoIpEngineProxy` operations: `AllocateChannel`, `SetChannel`, `SetCodec`, `StartRtpMedia`,
`SetDtmfMode`, `HandleDtmf`, `SetPdnIp`, `SetEngine`, `ImsStartEvent`, `SimInfo`,
`SignalingInfo`, `SetImsCallStatus`, session refresh, frame time, statistics options.
`Channel::isReady()` needs the channel state to be 1 or 2 and both a codec and an option set
recorded for it, so codec and options are set before the channel is started. The full
sequence for one call is still to be read from `CpAudioEngineClient::SAE_*` and `AudioSession`.

## Layouts read by hand

Names come from Samsung's own setters and log strings (`SAE_SetSendIP` "SendAddr"/"LocalAddr",
`SetCodecInfo` "type=%d, freq=%d, ..."), so they are theirs, not guesses, unless marked.

`ImsSetChannel` (0x1601, set, 0x34 bytes):

| Offset | Field |
|---|---|
| 7 | channel id |
| 8 | operation: 1 create, 2 delete, 3 change |
| 9 | u16 local RTCP port |
| 0x0b | u16 remote RTCP port |
| 0x0d | u16 local RTP port |
| 0x0f | u16 remote RTP port |
| 0x11 | address family: 1 IPv4, 2 IPv6 |
| 0x12 | remote ("send") address, 4 or 16 bytes |
| 0x22 | address family again |
| 0x23 | local address, 4 or 16 bytes |
| 0x33 | stream type |

For other operations the port and family fields are sent as 0xff. The argument after the
channel in the caller is the operator id (`mno`); it is only logged.

`ImsSetRtpMedia` (0x1603, set, 0x0b bytes): @7 operation (1 = start/stop request), @8
channel, @9 status (the direction/hold state from `StartChannel`/`ModifyChannel`), @0x0a one
more byte from the channel object.

`ImsSetCodec` (0x1602, set, 0x41 bytes):

| Offset | Field |
|---|---|
| 7 | channel |
| 8 | u32 flag mask of changed codec settings |
| 0x0c | codec id (`convertCodecNameToIpc`: AMR, AMR-WB, EVS, ...) |
| 0x0d | payload type |
| 0x0e | u16 sampling frequency |
| 0x10 | u32 bit rate (`convertBitRateToIpc`) |
| 0x14 | ptime |
| 0x15 | max receive ptime |
| 0x16 | channels |
| 0x17 | fixed-rate flag |
| 0x18 | packetization mode (octet-aligned or bandwidth-efficient) |
| 0x19-0x1c | four bytes not named (one is the receive payload type) |
| 0x1d | DTX |
| 0x1e, 0x1f | redundancy level, redundancy payload type |
| 0x21 | u16 mode set |
| 0x24-0x29 | EVS: DTX receive, channel-aware receive, mode switch, channel send, channel receive, CMR |
| 0x2a, 0x2e, 0x32, 0x36 | EVS bit rate: send min, send max, receive min, receive max |
| 0x3a, 0x3b | EVS bandwidth range: send, receive |
| 0x3c, 0x40 | EVS default bit rate, default bandwidth |

`ImsSetOption` (0x1605, set, 0x4e bytes): the channel's option block with a flag mask saying
which settings are valid. Flag bits: 0x1 RTCP on (with RS/RR intervals), 0x2 RTCP during the
call, 0x4 RTP timeout, 0x8 RTCP timeout, 0x20 SRTP, 0x40 AV sync, 0x80 CIQ metrics, 0x100
codec bit-rate notification, 0x800 RTCP-XR, 0x2000 echolocate. Field order inside the block
was not mapped byte by byte.

`ImsSetDtmf` (0x1604, set, 0x0d bytes): @7 channel, @8 and @9 payload types, @0x0a u16,
@0x0c one byte. `ImsHandleDtmf` (0x1604, exec): @7 channel, @8 digit, @9, @0x0a volume/duration
(inferred).

`ImsSetPdnIp` (0x1606, set, 0x2d bytes): @7 address family, @8 address (4 or 16 bytes), @0x18
interface name length, @0x1c interface name (up to 16), @0x2c one byte (PDN/cid).

`ImsEngineSet` (0x1609, set): @7, @8 two bytes (engine and state). `ImsSimInfo` (0x1620,
set): @7 one byte.

## Read 2026-11-17 (stage 0 of the IMS plan)

Engine and PDN:
- 0x1609 `ImsEngineSet`: @7 on (1) / off (0), @8 a mode byte that the IMS service passes
  in at `CPVEInterface::Initialize` and keeps per SIM (`InitEngine(phone, on, -1)` reuses
  it). Its value comes from the Java side (not read). Sent once per SIM at start.
- 0x1606 `ImsSetPdnIp`: @7 family (1 IPv4, 2 IPv6), @8 address (4 or 16 bytes), @0x18 u32
  name length, @0x1c name (up to 16), @0x2c one byte (a flag from the caller). The name is
  `"lte_" + <pdn name>`, e.g. `lte_ims`: the modem's data profile name (as in 0x0D1B),
  not the network interface.

Codec (0x1602) values:
- Codec ids (`convertCodecNameToIpc`): 1 PCMU, 2 PCMA, 3 G729, 4 G729A, 5 G729AB,
  6 AMR-NB, 7 AMR-WB, 8 EVRC, 9 EVRC0, 10 EVRC1, 11 EVRCB, 12 T140, 13 EVS.
- Bit rate (`convertBitRateToIpc`): one bit per rate. AMR 4750 0x4, 5150 0x8, 5900 0x10,
  6700 0x20, 7400 0x40, 7950 0x80, 10200 0x100, 12200 0x200; AMR-WB 6600 0x400, 8850
  0x800, 12650 0x1000, 14250 0x2000, 15850 0x4000, 18250 0x8000, 19850 0x10000, 23050
  0x20000, 23850 0x40000; others: 8000 0x1, 64000 0x2, 800 0x80000, 4000 0x100000, 8550
  0x200000, 1200 0x400000, 2400 0x800000, 4800 0x1000000, 9600 0x2000000.

Option block (0x1605, set, 0x4e bytes), from `VoIpEngineProxy::SetCIQMetric` (it copies
the channel's option fields, 0x70 bytes from channel +0x13c, plus the flag word at +0x1ac)
and the `SAE_*` setters that fill them:

| Offset | Field | Set by (flag) |
|---|---|---|
| 7 | channel | |
| 8 | u16 flags: which fields are valid | |
| 0x0a | RTCP on | `SAE_EnableRTCP` (0x1) |
| 0x0b | RTCP during the call | `SAE_EnableRTCPOnCall` (0x2) |
| 0x0c | u32 RTP timeout | `SAE_SetRTPTimeout` (0x4) |
| 0x10 | u32 RTCP timeout | `SAE_SetRTCPTimeout` (0x8) |
| 0x14 | one byte, no setter found | |
| 0x15 | SRTP profile | `SAE_EnableSRTP` (0x20) |
| 0x16 | SRTP direction | `SAE_EnableSRTP` |
| 0x17 | SRTP key length (max 30) | `SAE_EnableSRTP` |
| 0x18 | SRTP key [30] | `SAE_EnableSRTP` |
| 0x36 | AV sync | `SAE_EnableAVSyncOnCall` (0x40) |
| 0x37 | CIQ metrics | `SAE_SetCIQ` (0x80) |
| 0x3a, 0x3e | u32 RTCP RS and RR bandwidth | `SAE_EnableRTCP`/`OnCall` |
| 0x42, 0x43, 0x44 | RTCP-XR blocks, stat flags, RTT mode | `SAE_EnableXR` (0x800) |
| 0x45..0x4b | four u16 RTCP-XR parameters | `SAE_EnableXR` |
| 0x4d | echolocate | `SAE_EnableEcholocate` (0x2000) |

Flag 0x100 (codec bit-rate notification) has no field. Every setter only records; the
block goes out once (`SAE_SetInCallOptions`, `SetCIQMetric`), then the flags are cleared.

Call status and signalling on Shannon:
- `SetImsCallStatus` (0x027c) and the call-group signalling info (0x0280) are sent only when
  `getModemType()` is 1, i.e. `ril.modem.board` starts with one of three Qualcomm prefixes
  ("MSM", ...). On Shannon neither goes out. 0x1619 `ImsSignalingInfo` (@7 message type,
  @8 state, @9 one byte) does, waiting up to 1 s for the answer.
- 0x1620 `ImsSimInfo`: @7 one byte.

Errors (`MapIPCError`, result codes in the generic answer): 0x8305 engine state invalid,
0x8314 no free session, 0x8328 AP/CP local IP mismatch, 0x8352 codec already active,
0x8354 MVS acquire failed, 0x8355 MVS enable failed, 0x8358 TTY start failed, 0x8359 CVD
start failed, 0x838d PDN profile number invalid, 0x838f netlib open failed. 0x8000 = ok.

## Checked 2026-11-17 (stage 2, against libsec-ims)

- Notification path: `onUnsol` (decomp-secims-global) decodes group 0x16 notifications and
  maps the event byte with `OemIPCHandler::ConvertIpcEventToVE` / `MAPBearerEventtoIPCEvent`
  / `MAPRRCConnectionEventtoIPCEvent`, then `VoIpEngineProxy::IpcCallback` calls the handler
  `CPVEInterface::HandleMediaEvent` (or the bearer one).
- 0x1605 notification: @7 event, @8 channel, @9 data. Events (stock VE number, consumer):
  1 RTP timeout (0x1c, `CallSession::OnRtpRtcpTimeout`: BYE "Call End By RTP Timeout",
  ignored when not receiving and during early media), 2 RTCP timeout (0x3d, same handler),
  3 codec bit-rate change (0x65), 4 (0x48) and 5 (0x20) not named, 6 first RTP received
  (0x1d, `OnFirstRtpRxReceived`), 7 RTP back (0x1f) and 8 RTCP back (0x3e)
  (`OnRtpRtcpTimeoutClear`), 9 statistics (0x66, only printed). Subcommand 0x0a = frame time
  (100), 0x07 = session refresh events 1/2/6.
- 0x160B: @7 bearer id, @8 QCI, @9 event, @0x0a and @0x12 u64 bit rates; frames of 0x1b bytes
  or more: @0x1a, @0x1b, @0x1c filter count, filters from @0x1d. Events 1, 2, 3 -> 0x6e, 0x6f,
  0x70; `CallSession::OnDedicatedBearer` takes 1 and 2 alike ("allocated bearer id", cancels
  the bearer timer, UpdateQos), 3 = released.
- 0x1602 (`ImsSetCodec` from `_stSAE_CodecInfo`, filled by `CPVEInterface::SetCodecInfo`,
  called from `AudioSession::Create`): @0x0d send payload type, @0x0e clock rate, @0x10
  bit rate, @0x14 ptime, @0x15 max receive ptime, @0x16 channels (1), @0x17 fixed rate (0),
  @0x18 packetization = `SdpCreator::GetOctetAlign` (the fmtp's octet-align digit, 0 if
  absent; EVS uses 1/2 there instead), @0x19-0x1c zero for AMR, @0x1d DTX, @0x1e/@0x1f
  redundancy level / payload type (0, 0), @0x21 mode set = `GetModeSetCombination` (OR of
  1 << mode over mode-set; 0 if none, except for MNO 0x37 Vodacom ZA: 0xff / 0x1ff), @0x23
  receive payload type ("rx_type"). Bit rate: the mode table at the highest mode of mode-set
  (`GetModeSet`, default 8 AMR-WB / 7 AMR), capped by an ImsUserProfile value (+0x70, not
  named). Log: "sndpayload rcvpayload bitrate name octet-align ptime maxptime sampleRate
  modeSet".
- 0x1604 set (`SAE_SetDTMFOptions`, from `AudioSession::Create`, log "DTMF sndPayload=%d
  rcvPayload=%d bitrate=%d"): @7 channel, @8 send payload type, @9 0, @0x0a the
  telephone-event codec's `getRate()` (clock rate), @0x0c receive payload type.
  `Channel::isDtmfReady` needs @8 and @9 >= 0.
- 0x1604 execute (`ImsHandleDtmf`, 0x0b bytes): @7 channel, @8 code, @9 mode, @0x0a
  operation, passed through from Java: imsservice `ImsCallSession.sendDtmf` (mode 0, op 1),
  `startDtmf` (mode 1, op 1), `stopDtmf` (mode 1, op 2); the code from the dialled char in
  ImsTelephonyService (`com.google.ims.ImsCallSessionImpl.convertDtmfToCode`: '0'-'9' ->
  0-9, '*' -> 10, '#' -> 11, else -1). The framework parts are odex/vdex only: dex carved
  from the vdex (checksum and SHA-1 recomputed) decompiles with out/tools/jadx; quickened
  code (ims-common) does not, its bytecode reads with a small dex parser.
- Channel numbers: `VoIpEngineProxy::AllocateChannel` takes the first free of 8 per phone,
  channel id = phone * 8 + index.
- MNO enum (SecImsIF.java): TELEFONICA_CZ = 119, VODACOM_SOUTHAFRICA = 55 (0x37).

## Modem firmware side (read 2026-11-17 night, RADIO backup, MAIN image)

MAIN loads at 0x40010000 (TOC: file offset 0x1920, size 0x2479508). Code is Thumb-2;
log calls take a DBT trace entry ('DBT:', fmt pointer at +0x10, line at +0x14), addressed
as a literal-pool base plus an immediate. `llvm-objdump --triple=thumbv7r-none-eabi` on
an objcopy'd slice reads it.

- IPC_IMS_PDN SET handler (0x408bd544): flag byte (frame 0x2c) 0 = answer 0x8000 and do
  nothing; IP type must be 1/2; then the context lookup by profile name (0x408bc0b8):
  `lte_emergency`, `lte_ims`, `lte_internet` against 11 PDP entries, matching IP type
  (2 IPv4 / 5 IPv6, or 6), state byte +0x383 == 1 and the kind mask +0x382 (2 IMS, 4
  emergency, 0x10 internet); else by address (0x408bc2b4); else 0x8001.
- The kind mask is set in one place, "Set PDP property" (0x4089c9f4), called from the
  IPC_GPRS_PDP_CONTEXT SET handler (0x408a0d48) by its first byte: 1 -> internet,
  3 -> IMS, 4 -> emergency. So the IMS PDN must be activated with mode 3 (stock does).
- The modem's 0x1609 log names its second byte "DTMF profile num" (we send stock's value).

## Open

- The byte at 0x14 of the option block; the ImsUserProfile bit-rate cap (+0x70).
- The value set of the status byte in 0x1603 and the mode byte of 0x1609.
- Whether the modem needs the session-refresh and signalling hints at all for a basic call.
- How this maps onto `android.hardware.radio.ims.media.IImsMedia` (`RtpConfig`, `AmrParams`,
  `DtmfParams`, `RtcpConfig`, media quality): the fields look like a direct match.
