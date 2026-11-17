# IMS / VoLTE: our own stack in shannond

Status 2026-11-17: **stage 1 (registration) written and host-tested, not run on the phone.**
Nothing has been sent to the network. Plan: `~/.claude/plans/well-anything-s-that-cheap-piped-thunder.md`.
Stock reference: `work/notes/ril/` (voice-engine.md, ims-ipc.md, ims-sequences.md).

## Decisions (user, 2026-11-17)

- The SIP stack is native, in shannond's core (`core/ims/`), and our own code written for the
  IMS profile only (no reSIProcate, no Kotlin stack). Android gets a thin ImsService app
  without protocol logic (stage 3).
- No Samsung userspace; Wi-Fi calling not now but possible (transport and media are
  interfaces; the operator table keeps services per access network).

## What the stock image says about this phone and O2 CZ

- The firmware is the O2 build (CSC `O2C`) with VoLTE on: `CscFeature_IMS_EnableVoLTE`,
  `customer.xml` IMSSetting "O2 - CZ" (VoLTE on, Wi-Fi calling off, video off, SMS over IP
  off, emergency on CS, USSD on CS, voice domain "IMS PS voice preferred", SRVCC version 10,
  supplementary services over XCAP when registered, CSFB on IMS errors).
- `imsprofile.json` "Telefonica for Czech VoLTE": APN `ims`, IPv6, P-CSCF port 5060, IPsec
  with integrity only (`enc_algo` none), protected ports client 6000 / server 5000,
  preconditions, session timer 3600 s, ECT, conference URI
  `sip:mmtel@conf-factory.ims.mnc002.mcc230.3gppnetwork.org`, service `mmtel` on LTE.
  Defaults: `reg_expires` 600000, UDP preferred, retry 30-1800 s. `globalsettings.json`:
  CSFB error code 403.
- User-Agent (user, 2026-11-17: send Samsung's): `buildUserAgentString` fills the profile's
  `useragent` template; O2's profile has none, so the default
  `[PRODUCT_MODEL]-[BUILD_VERSION] Samsung IMS 6.0`, with Build.MODEL `SM-A320FL` and, for
  European operators, the last three characters of ro.build.PDA `A320FLXXS9CTK2`:
  **`SM-A320FL-TK2 Samsung IMS 6.0`**. (That O2 inherits the default template is inferred:
  the profile merge code is in a framework jar not extracted, but the builder calls
  `.replace` on the template unconditionally.) `userAgentFor()` in profile.cc.
- The profile's `secure_client_port` 6000 / `secure_server_port` 5000 are not what the
  native stack offers (next section).

## Modem side (read from the stock RIL / IMS library)

- VoPS: `IPC_NET_REGIST` (0x0805) payload offset 13 (frame @0x14) = VoLTE support (1 yes,
  2 no), offset 14 = IMS emergency support, offset 11 = TAC, offset 16 = PCI. Stock's
  `NetRegState::IsVolteAllowed` = that byte == 1. In shannond: `Registration::volte`.
- IMS PDN: `IPC_GPRS_DEFINE_PDP_CONTEXT` (0x0D01) carries the PDP type (stock: IPv4 2, IPv6 5,
  IPv4v6 6) and, last byte, a P-CSCF request (1 IPv4, 2 IPv6, 3 both) that stock sets when
  the APN needs P-CSCF. shannond used to send IPv4 without the request for every context; the
  IMS context now gets both (others unchanged). P-CSCFs come back in 0x0D09.
- Registration state to the modem: `IPC_IMS_REG_STATE` (0x1608) on every change: status
  (1 registered), features (1 voice, 2 SMS, 8 video), network type (0/1, names not found:
  we send 0), ECMP, RAT (LTE 6, IWLAN 5). Stock sends it from `ImsManager::OnImsRegistrationChanged`.
- AKA: `IPC_SEC_IMS_AUTH` answer status 0 = RES, CK, IK; 4/5 = synchronisation failure with
  AUTS first (stock builds the 0xDC answer from it); 1-3 = failures (MAC).
- Voice engine (stage 2): codec ids, bit-rate bits, the 0x1605 option block, 0x1606 with the
  profile name `lte_ims`, 0x1609 engine on/off: voice-engine.md "Read 2026-11-17". On
  Shannon the IMS stack never sends 0x027c / 0x0280 (Qualcomm only).

## Code (src/shannond, commits ad72677 and 36cb9eb; libsamsung-ipc c04f26d, 0d41777, 1082abd)

`core/ims/`: sip (messages, framing), digest (AKAv1-MD5, AUTS), secagree (33.203), endpoint
(UDP/TCP, transactions, protected and unprotected sockets), network (sockets + IPsec per
socket; LinuxImsNetwork = XFRM netlink + socket policies, the IpSecManager model),
registration, identity/isim (ISIM files or IMSI), profile (O2 CZ), ims (policy).
`core/alarm`: boot-time timer (CLOCK_BOOTTIME_ALARM) for the registration refresh.

Protocol choices worth knowing:
- Via and Contact carry the protected server port (port_us) for UDP and TCP; the P-CSCF
  answers UDP requests from its client port to it. The endpoint reads every socket and
  matches answers by branch, so either behaviour works.
- Ports, SPIs and re-keying as the stock stack (user, 2026-11-17: follow stock):
  - ports from `UserAgentManager::AllocateUserIdentity`: a counter starting at 6000 that
    steps by 100 for every user agent created (wraps at 10000 to 6100); port-s = counter,
    port-c = counter + 1. First registration: 6100 / 6101. Kept for the user agent's life
    (ours: one ImsRegistration::start()).
  - SPIs new in every Security-Client (`SipMessageBuilder::GenerateSecurityClient`):
    spi-c = rand() % 100000 + 100, spi-s = spi-c + 1. Reserved with XFRM ALLOCSPI min = max.
  - SA sets are keyed by the client port (`RegistrationSession::CreateIPSec`); with the same
    port it logs "Vaild SA already exsits" and creates none. So a challenged refresh goes on
    with the existing SAs (the newly offered SPIs are never installed). Old sets are kept
    only when the client port changed (`RegiReConnecting::Process200OkResponse` ->
    `AddOldSaInfo`, deleted at the old registration's expiry), which within one user agent
    does not happen. Concern for gate 3+: if O2 ever re-challenges a refresh and moves to the
    new SPIs, this loses the protected path; stock would have the same problem.
- PANI only on protected requests. Messages over 1300 bytes go over TCP.
- Logs carry methods and CSeq only, never identities or keys.

Tests (host, ASan/UBSan): 75 in `shannond_tests` (SIP parser, digest RFC 2617, Milenage
TS 35.208 set 1, security agreement, registration flows against `sim/fake_pcscf`: success,
AUTS, failover, 423, 403 back-off, re-keyed refresh, reg event termination, TCP; full stack
with the fake modem's ISIM files), and `shannond_ipsec_tests` under `unshare -rn`:
registration over real ESP (IPv6, HMAC-SHA-1-96) and a plain packet to the protected port
dropped. `ctest` runs both.

Linux front end: `ims on|off|status` (VoLTE off until asked), `--ims-ignore-vops`.
Mainline config: XFRM_USER, INET_ESP, INET6_ESP as modules (tools/mkconfig-a3y17lte).

## Not done

- Stage 2: calls (voice engine, SDP, INVITE with preconditions, PRACK, UPDATE, hold, session
  timer, CS fallback). Read first: `CallUacSession`, `CallUasSession`, `SdpCreator`, the
  0x1603 status values and the four unnamed codec bytes.
- Stage 3: Android (private AIDL, ShannonIms app, RadioNetwork VoPS flag, srvccStateNotify,
  overlay/carrier config/sepolicy; the HAL needs CAP_WAKE_ALARM for the refresh alarm).

## Phone gates (each send needs the user's go)

Development loop: the cross-built Linux daemon (`out/shannond-aarch64/shannond`) from a root
shell on Android with the radio HAL stopped, or the Alpine RAM boot.
1. Read only: ISIM present (`sim apps`)? VoLTE byte in the registration (`status`, log line
   "VoLTE yes/no")?
2. `ims on` up to the PDN: address and P-CSCF list arrive on the IMS context (cid 3)?
3. REGISTER: the network's first answer decides the fixes (401 with AKA + Security-Server
   expected). Watch for: P-CSCF answering on which port, User-Agent, Contact form.
4. Calls (after stage 2): one outgoing, one incoming, audio both ways.
5. The Android build.
