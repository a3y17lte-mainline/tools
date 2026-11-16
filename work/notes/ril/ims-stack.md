# Samsung's IMS stack: where each thing lives

Status 2026-11-16: inventory from the decompiled Java app (`work/system/extract/ims/java`,
jadx; 1,574 files, 55 with decompile defects) and the symbols of `libsec-ims.so` (dump of
Samsung's classes in `work/system/extract/decomp-secims`). A map for reading, not yet a
description of the behaviour.

## Layers

| Layer | Code | Role |
|---|---|---|
| Service and policy | Java `com.sec.internal.ims.imsservice` | when to register, on which network, with which profile |
| Call logic | Java `com.sec.internal.ims.volte2` | call sessions as state machines |
| Stack interface | Java `...imsservice.handler.secims` (`StackIF`, `SecImsIF`, `Resip*Handler`) | protobuf requests and notifications to the native stack |
| SIP | native `libsec-ims.so`: reSIProcate + Samsung classes | messages, transactions, dialogs, SDP, IPsec setup |
| Media control | native `VoIpEngineProxy`, `CPVEInterface`, `OemIPCHandler` | the modem voice engine (voice-engine.md) |

## State machines (Java, Android `StateMachine`)

Registration, `imsservice/UserAgent.java` (1,983 lines): `InitialState`, `ReadyState`,
`RegisteringState`, `RegisteredState`, `ReRegisteringState`, `DeregisteringState`,
`TerminatingState`, `EmergencyState`, `ProhibitedState`. The AKA challenge arrives here and is
answered through `SimManager.requestIsimAuthentication` -> `StackIF.sendAuthResponse`
(`RequestUpdateAkaResp`).

Call, `volte2/ImsCallSession.java` (6,068 lines): `ReadyToCall`, `IncomingCall`,
`OutgoingCall`, `AlertingCall`, `InCall`, `HoldingCall`, `HeldCall`, `ResumingCall`,
`ModifyingCall`, `ModifyRequested`, `HoldingVideo`, `VideoHeld`, `ResumingVideo`,
`EndingCall`. Variants: `ImsConfSession`, `ImsEmergencySession`.

Registration policy, `imsservice/RegistrationManagerBase.java` (7,854 lines) with
`RegisterTask` per profile and one governor per region
(`governor/RegistrationGovernorImpl.java` 896 lines, `...Eur.java` 649 lines for O2 CZ).
Its events show what a registration has to react to: SIM ready/refresh, PDN connected/
disconnected/failed, P-CSCF changed, local IP changed, network type and mode changes, flight
mode, mobile data and roaming settings, VoLTE setting, call status, SSAC re-register,
emergency registration, shutdown; plus the Wi-Fi calling set (ePDG connected/disconnected,
IKE error, handoff timeout), which we leave out.

Other Java pieces: `PdnController` (IMS PDN through `ConnectivityManager.requestNetwork`),
`SimManager` and `SimDataAdaptor*` (ISIM/USIM identities, per operator), `SsacManager`
(barring), `PaniGenerator` (P-Access-Network-Info), `MnoMap`, `GlobalSettingsRepo*`,
`SmsServiceModule` (SMS over IMS), `UtService` (supplementary services over XCAP).

## Native classes (libsec-ims.so)

- Registration: `RegistrationManager` (events `TRY_REGISTER`, `UPDATE_AKA_RESP`,
  `NETWORK_SUSPENDED`, `DEL_OLD_SA`, `TRY_REREGISTER_FROM_KEEPALIVE`, ...; methods
  `TryRegister`, `CreateIPSec`, `DeleteOldIPSec`, `UpdateAkaResponse`, `TrySubscribe`,
  `SuspendTransPort`/`ResumeTransPort`), `RegistrationSession`, `RegiAuth`, `IPSecInterface`
  (talks to `imsd`), `SubscribeSession`, `UserAgentManager`, `PaniData`.
- Calls: `CallManager`, `CallSession` (264 methods, incl. `UpdateQos`), `CallUacSession`,
  `CallUasSession`, `CallMsgCreator`, `SdpCreator`, `SdpUtil`, `MediaConfig`, `AudioSession`.
- Messages: `SipMessageBuilder`, `SipErrorBase`.
- Signalling hints to the modem: `ImsSignalingIpcManager`.
- SMS: `ImsSmsManager`.
- Not relevant to us: `IMManager`, `IMSession`, `FTSession`, `msrp::*`, `PidfCreator`,
  `PublishSession`, `IshSession`, `VshSession` (RCS, presence, sharing).

What Samsung added to reSIProcate for IMS is in these classes, not in patches to the
library's namespaces that show up as symbols: the security agreement and IPsec handling sit
in `RegistrationManager`/`IPSecInterface`, the QoS precondition handling in
`CallSession::UpdateQos` and the SDP classes. Whether the bundled reSIProcate itself was
modified was not checked.

## Operator data

`imsservice.apk` `res/raw`: `mnomap.json` (486 MCC/MNC entries), `imsprofile.json` (364
profiles, 224 operators, 209 distinct settings), `globalsettings.json`, `localconfig.json`,
`dmconfig.xml`. O2 CZ: `23002` -> `Telefonica_CZ`, one profile: APN `ims`, IPv6, port 5060,
IPsec with integrity only (`enc_algo` none), client port 6000 / server port 5000,
preconditions on, session timer 3600 s, LTE only, service `mmtel` (no `smsip`: SMS over IMS
is not enabled for this operator in the table).

## Open

- Samsung's actual sequences: REGISTER headers, the security agreement, call setup with
  preconditions. Read `RegistrationManager::TryRegister`, `CreateIPSec`, `CallUacSession`,
  `SdpCreator` and `RegistrationGovernorEur`.
- The `SecImsIF` messages' fields (the protobuf definitions are fully in the Java class).
- `UaProfile`: which of the 209 profile settings reach the native stack.
