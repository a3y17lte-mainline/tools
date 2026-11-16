# Samsung's IMS sequences

Status 2026-11-16: read from the decompile dumps (`work/system/extract/decomp-secims`, the
Java tree in `work/system/extract/ims/java`). Static only. Each section says what is read and
what is inferred.

## 1. Voice engine: one call, as modem messages

Read: the call chain `CPVEInterface` -> `CpAudioEngineClient::SAE_*` -> `VoIpEngineProxy` ->
`OemIPCHandler::Ims*` was extracted function by function. Inferred: the order in which the
call logic invokes the `CPVEInterface` operations (they are virtual calls from `AudioSession`
and `CallSession`), taken from the natural order and from `Channel::isReady()`.

| Step | `CPVEInterface` | Modem messages, in order |
|---|---|---|
| once, at start | `Initialize` | RIL client connect, event handlers registered, 0x1609 engine set |
| once per SIM | `SendSimInfo` | 0x1620 SIM info |
| at registration | `SetPdnInfo` | 0x1606 PDN address and interface |
| call setup | `CreateChannel` | 0x1606 PDN, 0x1601 channel (allocate), 0x1605 options, then send IP/ports and receive ports are stored and 0x1601 channel is sent again with the addresses (`SAE_ReconfigIPPort`) |
| after SDP answer | `SetCodecInfo` | 0x1602 codec |
| | `SetDtmfCodecInfo` | 0x1604 DTMF setup |
| | `SetRtpTimeout`, `SetRtcpTimeout`, `SetRtcpOnCall`, `SetRtcpXr`, `EnableSRTP`, `SetCallOptions` | each sets a flag; 0x1605 options carries them |
| media start | `StartChannel` | 0x1603 RTP media, start |
| hold / resume | `ModifyChannel` | 0x1603 RTP media with the new status |
| remote change | `UpdateChannel` | 0x1601 channel with new addresses |
| DTMF | `HandleDtmf` | 0x1604 exec |
| call end | `StopChannel`, `DeleteChannel` | 0x1603 stop, 0x1601 channel (close) |
| around the call | `SetImsCallStatus`, `SendSignalingInfo` | 0x027c call status, 0x1619 / 0x0280 signalling info |
| in the call | `SetTimeRef`, `GetLastPlayedVoiceTime` | 0x160a frame time |

A failed step deletes the channel again (every setter in `CreateChannel` is followed by a
`SAE_DeleteChannel` error path). `Channel::isReady()` requires a codec and an option set to be
recorded before the channel counts as started.

Events on the way back: 0x160B dedicated bearer (added/released, QCI, filters), 0x1605 media
events (RTP/RTCP timeout ends the call in `CallSession`; statistics), 0x160E RRC events.
`CallManager::OnDedicatedBearerEvent` ties the bearer to the call's QoS precondition
(`CallSession::UpdateQos`): the call only proceeds to ringing once the bearer is there.

## 2. Registration

Read: class and event inventory, the header and token strings in the library, the O2 CZ
profile, the Java governor's method list. Not read line by line: `RegistrationManager::
TryRegister`, `CreateIPSec`.

Flow, with the component that does each step:

1. Java `RegistrationManagerBase` waits for SIM ready, the IMS PDN (`PdnController`, event
   `PDN_CONNECTED`) and a P-CSCF list; `RegistrationGovernorEur.isReadyToRegister()` decides.
2. Java builds a `UaProfile` from the operator profile and sends `RequestUACreation`, then
   `RequestRegistration` (state `RegisteringState`).
3. Native `RegistrationManager::TryRegister` sends REGISTER with `Security-Client`
   (`ipsec-3gpp`; algorithms `hmac-md5-96`, `hmac-sha-1-96`; encryption `aes-cbc`,
   `des-ede3-cbc` or none), `Require`/`Proxy-Require: sec-agree`, `P-Access-Network-Info`,
   `+sip.instance` with the IMEI URN, the MMTEL feature tag.
4. The 401 carries the AKA challenge and `Security-Server`. Native notifies Java
   (`NotifyRegiAuth`); Java asks the SIM (`SimManager.requestIsimAuthentication`) and answers
   with `RequestUpdateAkaResp`. Digest algorithms known to the library: `AKAv1-MD5`,
   `AKAv2-MD5`.
5. Native `RegistrationSession::CreateIPSec` fills `IPSecInterface::SetIPSecInformation` with
   four SPIs (UE client -> P-CSCF server, UE server -> P-CSCF client and the two reverse
   directions), four ports (UE client/server, P-CSCF client/server), the two algorithms, the
   IP version and local address, then installs the associations (`IPSecApplySAAssoc`) and
   the policies (`IPSecApplySAPolicy`) over PF_KEY. `imsd` exists because the app cannot
   open that socket itself. In the open stack this is Android's `IpSecManager`.
6. Second REGISTER on the protected ports with `Security-Verify`; 200 OK. Native subscribes
   to the registration event package (`TrySubscribe`); Java goes to `RegisteredState`.
7. Refresh before expiry (`ReRegisteringState`); old security associations are deleted after
   the new ones work (`AddOldSaInfo`, `DeleteOldIPSec`, event `DEL_OLD_SA`).

The library also has `gruu`, `outbound`, SigComp and `Path`/`Service-Route` handling.

O2 CZ specifics, all from data or code:
- Profile: APN `ims`, IPv6, port 5060, IPsec with integrity only, protected ports 6000
  (client) and 5000 (server), `reg_expires` default, session timer 3600 s.
- The Java app has no O2 CZ branch in registration or call setup. Its only `TELEFONICA_CZ`
  checks are in supplementary services over XCAP (`UtUtils.isRealmFromUsername`, call barring
  rule handling in `UtServiceModule`).
- `RegistrationGovernorEur` (649 lines) holds the European policy: error handling
  (`onRegistrationError`, `onSipError`), throttling (`isThrottled`, `releaseThrottle`), PDN
  failure retry times, service filtering by network (`filterService`, `applyVoPsPolicy`:
  no voice service without the network's voice-over-PS indication), delayed deregistration,
  SRVCC case.

## 3. Call setup

Read: class inventory, state names, token strings. Not read line by line: `CallUacSession`,
`CallUasSession`, `SdpCreator`.

- Java `ImsCallSession` states: `ReadyToCall` -> `OutgoingCall` -> `AlertingCall` -> `InCall`
  (or `IncomingCall` -> `InCall`), `HoldingCall`/`HeldCall`/`ResumingCall`, `EndingCall`.
  Requests to native: `RequestMakeCall`, `RequestProgressIncomingCall`, `RequestAcceptCall`,
  `RequestRejectCall`, `RequestHoldCall`, `RequestResumeCall`, `RequestEndCall`,
  `RequestHandleDtmf`, `RequestUpdateCall`.
- Native builds the INVITE with `P-Preferred-Service`/`Accept-Contact` carrying the MMTEL
  service identifier, `Supported: 100rel, precondition, timer` (tokens present),
  `P-Early-Media`, `Session-Expires`/`Min-SE`, and SDP with the QoS attributes (`curr:`,
  `des:`, `conf:`; `SdpCreator::CreatePreconditionFailureSdp`, state `OnSip_WaitQos`).
- Provisional responses are acknowledged with PRACK (`RAck`/`RSeq`); the QoS state is updated
  with UPDATE once the dedicated bearer event arrives.
- Emergency: an `alternative-service` body in an error response can ask for emergency
  registration (`ResipVolteHandler`, per-operator list). We route emergency calls to the
  circuit-switched side instead.

## 4. What the Java app passes down

From `StackIF` (the builders of `RequestUACreation`, `RequestRegistration`, `RequestMakeCall`).
This is the full set of knobs Samsung's SIP layer takes, and so the list of profile settings
that matter:

- Identity and network: IMPI, IMPU list, domain, realm, password, interface, network id, PDN,
  P-CSCF address list and port, RAT, current PANI, phone id, operator id.
- Transport and security: transport type, IPsec on/off with encryption and authentication
  algorithm, register algorithm, self port, keep-alive, compact headers, MSS size, TCP reset
  error codes, hostname.
- Registration: service list, feature tag list, instance id and UUID, GRUU, outbound, session
  id, subscribe-to-reg, subscriber timer, reg expiry, user agent string.
- SIP timers: T1, T2, T4, A-K, ringback and ringing timers, dedicated-bearer timer
  (`DbrTimer`), early-media RTP timeout.
- Call behaviour: preconditions (enabled, initial sendrecv, 183 for IR.92 v9, upgrade
  support), 100rel, 183 on progress, session expires, Min-SE, session refresher, privacy
  restrictions, P-Early-Media, history-info, explicit call transfer, remote URI type.
- Media: audio engine type, audio codec list and port, AMR/AMR-WB payload types (octet
  aligned and bandwidth efficient), DTMF payload types, mode sets, max-red, AS/RS/RR
  bandwidths, ptime/maxptime, RTP and RTCP timeouts, RTCP on active call, RTCP-XR, SRTP, EVS
  settings. (Video and RCS settings exist and are not needed.)
- Per call: peer URI, call type, codec, direction, CLI mode, dialled number, additional SIP
  headers, "LTE EPS-only attached" flag.

## Open

- Line-by-line reads of `TryRegister`, `CreateIPSec`, `CallUacSession::*`, `SdpCreator::*`
  for exact header values and the order of UPDATE/PRACK against the bearer event.
- The mapping from the JSON profile keys to these builder settings (`RegisterTask`,
  `ResipRegistrationManager`): names mostly match, not checked one by one.
