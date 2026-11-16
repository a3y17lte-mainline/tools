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

`ImsSetChannel` (0x1601, 0x34 bytes). Field names are inferred from the argument order:

| Offset | Field |
|---|---|
| 7 | channel id |
| 8 | operation (1 and 3 carry the address block; other values send 0xff fill) |
| 9, 0xb, 0xd, 0xf | four u16: local and remote RTP/RTCP ports |
| 0x11 | address family (2 = IPv6, else IPv4) |
| 0x12 | first address (4 or 16 bytes) |
| 0x22 | address family again |
| 0x23 | second address (4 or 16 bytes) |
| 0x33 | one byte (last argument) |

`ImsSetPdnIp` (0x1606, 0x2d bytes): @7 address family, @8 address (4 or 16 bytes), @0x18
interface name length, @0x1c interface name (up to 16), @0x2c one byte (PDN/cid).

`ImsSetCodec` (0x1602, 0x41 bytes): @7 channel, @8 u32, @0xc codec id
(`convertCodecNameToIpc`), @0xd payload type, @0xe u16, @0x10 bit rate
(`convertBitRateToIpc`), then one byte per codec option from `_stSAE_CodecInfo` (octet
align, mode set, DTX, EVS fields).

## Open

- The exact meaning of each byte: needs the callers (`CpAudioEngineClient::SAE_*`,
  `AudioSession`, `CallSession`).
- The order Samsung uses for one call (SIM info, PDN, engine, channel, codec, RTP media).
- Whether the modem needs the session-refresh and signalling hints at all for a basic call.
- How this maps onto `android.hardware.radio.ims.media.IImsMedia` (`RtpConfig`, `AmrParams`,
  `DtmfParams`, `RtcpConfig`, media quality): the fields look like a direct match.
