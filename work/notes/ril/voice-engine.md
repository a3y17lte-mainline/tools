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

## Open

- The byte order inside the option block (0x1605) and the four unnamed codec bytes.
- The value sets of the status byte in 0x1603 and of the engine bytes in 0x1609.
- Whether the modem needs the session-refresh and signalling hints at all for a basic call.
- How this maps onto `android.hardware.radio.ims.media.IImsMedia` (`RtpConfig`, `AmrParams`,
  `DtmfParams`, `RtcpConfig`, media quality): the fields look like a direct match.
