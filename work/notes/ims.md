# IMS / VoLTE: our own stack in shannond

Status 2026-11-17 evening: **registered with O2 CZ's IMS from the phone** (gates 1-3 done):
IMS PDN (IPv6, two P-CSCFs), REGISTER with USIM AKA, IPsec (algorithm not logged), 200 OK
for 3600 s, reg event SUBSCRIBE 200 + NOTIFY, deregistration 200, the
modem took IPC_IMS_REG_STATE. Run with the Linux daemon on Android (below). Plan: `~/.claude/plans/well-anything-s-that-cheap-piped-thunder.md`.
Stage 2 (calls) written 2026-11-17, host tests only (section "Calls"); next is gate 4, a test
call from the phone, which needs the user's go.
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

## Calls (stage 2, shannond 4bddb6f, 37c4bdd, a719d89, 1844070)

- `core/ims/sdp`: one audio stream, AMR-WB/AMR in both packetizations + telephone-event, the
  profile's payload types (offer order AMR-WB BE 116, OA 107, AMR BE 118, OA 96, DTMF 111/110),
  b=AS/RS/RR, RFC 3312 precondition lines. Offer: our resources "mandatory", the far end's
  "optional" (IR.92). The answer keeps the offer's first voice codec and its payload type.
- `core/ims/voice_engine` (modem, group 0x16), stock order: 0x1609 engine on (mode byte = low
  byte of audio_port_start, 1234 -> 0xd2) and 0x1606 PDN (`lte_ims`, flag 1) at
  registration; per call 0x1606 again, 0x1601 create (fields 0xff) + change (addresses,
  ports host order, RTCP = RTP + 1, stream type 1), 0x1602 codec (mask 0x21ff, packetization
  1 octet-aligned / 2 bandwidth-efficient, bit rate = highest mode-set mode, byte @0x23 = our
  receive payload type), 0x1605 options (RTP/RTCP timeout 20 s, RTCP on call with RS/RR),
  0x1604 DTMF set, 0x1603 start (status 3 sendrecv, 0 held; last byte 1 for IPv6). Hold /
  re-INVITE: only what changed, then 0x1603 modify. End: 0x1603 stop, 0x1601 delete.
  Channels 0-7 (stock AllocateChannel). Local RTP ports from 1234, even.
  Inferred, check on the phone: in 0x1604 set (stock `SAE_SetDTMFOptions(ch, a, b, 0, u16)`
  -> @8 a, @9 0, @0x0a u16, @0x0c b) which payload type is the receive one and that the u16
  is the clock rate; 0x1604 execute = channel, RFC 4733 event code, mode 0, operation 1.
  The media event numbers (0x1605 notifications, e.g. RTP timeout) are not mapped: logged
  as "media event N on channel C"; a call shows which one is which.
- `core/ims/call` (ImsCalls):
  - Outgoing: INVITE (Supported 100rel, precondition, timer; P-Preferred-Service and
    Accept-Contact MMTEL; P-Early-Media; Session-Expires 3600, Min-SE 90), PRACK each reliable
    1xx, UPDATE with our resources once a QCI 1 dedicated bearer is reported (0x160B), 180 ->
    alerting, ringback timer 90 s (O2) then CANCEL, 200 -> ACK. Early media: the voice
    channel opens with the SDP answer. CS fallback for the profile's codes (O2: 403) and when
    the network never answers.
  - Request URIs: SIP (stock `remote_uri_type` sip): `sip:+420...@<domain>;user=phone`, local
    numbers `sip:N;phone-context=<domain>@<domain>;user=phone`.
  - Incoming: 100, then the answer in a reliable 183 (Require 100rel, precondition; asks the
    network to confirm its resources), 180 when both sides' resources are there (the
    network's UPDATE, our bearer: our own UPDATE first if the bearer came later); ringing
    timer 90 s (O2) then 480. Answer: 200 (no SDP when the 183 carried it) resent until the
    ACK. No preconditions: 180 at once, the answer in the 200. Offerless INVITEs: 488.
  - Hold/resume: re-INVITE sendonly/sendrecv. Remote re-INVITE/UPDATE answered (direction
    from our hold state and the offer), 491 while ours is pending. Session timer per RFC 4028
    (refresh by UPDATE when we are the refresher, BYE when the other side does not refresh).
  - Audio: Calls::setImsAudio: the same modem audio path, volume and audio hook as a CS call,
    while a call has a voice channel (outgoing from early media, incoming once answered).
  - Endpoint: timer B stops at an INVITE's first provisional answer; incoming INVITE
    transactions live until the final answer; retransmitted 2xx are re-ACKed; CANCEL.
- Tests: `test_sdp` (parser, answer, round trip, voice-engine frames against the fake modem),
  `test_call` (fake P-CSCF as the far end: outgoing with preconditions, early media, lost
  ACK, DTMF, hold, BYE; 403 -> CS; CANCEL; incoming with preconditions; incoming without,
  cancelled; remote hold; registration loss). 89 host tests; `fuzz/fuzz_sip.cc` (SIP, digest,
  Security-Server, SDP): 2.5 M inputs clean (`-DSHANNOND_FUZZ=ON -DSHANNOND_SANITIZE=ON`, clang).
- CLI: `ims dial NUMBER`, `ims calls`, `ims answer|hangup|hold|resume ID`, `ims dtmf ID DIGIT`;
  events `ims-call ID:state:in|out:number[ (end status, retry on CS)]`.
- Not done: 422 (session interval too small) retry, offerless incoming INVITE, forked 2xx
  beyond the first early dialog, SRVCC, emergency over IMS (CS), the RTP timeout (event
  number unknown), early media gating by P-Early-Media.

Gate 4 checklist (phone, each call needs the user's go): Android loop as above, `ims on`,
wait for registered, then (a) `ims dial <number>`: 183/PRACK, the 0x160B bearer event in the
log ("dedicated bearer N qci 1 added"), UPDATE, 180, 200; audio both ways; `ims dtmf`; hold
and resume; hangup. (b) a call to the phone: `event ims-call N:incoming`, `ims answer N`. Watch
the log for "voice engine ... failed (0x83xx)" (MapIPCError codes in voice-engine.md) and for
"media event" lines.

## Not done

- Stage 3: Android (private AIDL, ShannonIms app, RadioNetwork VoPS flag, srvccStateNotify,
  overlay/carrier config/sepolicy; the HAL needs CAP_WAKE_ALARM for the refresh alarm).

## Phone results 2026-11-17 (user: "try the IMS bits we already have")

- Gate 1: the card has only a USIM (no ISIM): identities from the IMSI. The LTE cell says
  VoLTE yes (0x0805 byte).
- Gate 2: `ims on`: the IMS context (cid 3, IPv6, P-CSCF request) came up on rmnet2 with two
  IPv6 P-CSCFs; the global address from the router advertisement.
- Gate 3, what it took (shannond d0cc109, 3bd2a57):
  - AKA type: 0 is IMS AKA on the ISIM only; this card needs USIM AKA (2): with 0 the modem
    answered once properly, once with a 4-byte RES and no keys within 8 ms.
  - The protected REGISTER must repeat the Security-Client (else 400 Bad Request).
  - After registration: the default public identity (first P-Associated-URI: a sip: and a
    tel: URI came back) and the P-CSCF in the Route; the IMSI-derived identity got
    `403 Forbidden - 186` on SUBSCRIBE.
  - The P-CSCF's UDP answers and requests come from its port-c (e.g. 32773), as expected.
  - Not seen yet: a refresh (expires 3600 s, refresh at 3000 s), a re-challenge.

Android test loop (no Spark build; root shell, SELinux permissive):
- Static build: `cmake -S src/shannond -B out/shannond-aarch64-static -G Ninja
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/src/shannond/cmake/alpine-aarch64.cmake
  -DALPINE_SYSROOT=$PWD/out/modem-sysroot "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -static"
  -DOPENSSL_USE_STATIC_LIBS=ON` (the sysroot got openssl-libs-static, zlib-static, zlib-dev),
  push to /data/local/tmp.
- `stop vendor.radio-a3y17lte`; start the daemon with setsid and all fds redirected, under a
  host `timeout` (adb otherwise waits for it): `--nv /data/vendor/modem/nv/nv_data.bin
  --socket /data/local/tmp/shannond.sock --audio-hook ""`. It takes the running modem over.
- Commands: `shannond --socket ... --send "ims on" --listen N`.
- Routing: RA routes land in the per-interface table (ifindex + 1000) and nothing sends
  unmarked traffic there: `ip -6 rule add oif rmnet2 lookup <ifindex+1000> pref 9000` after
  the PDN is up (the index changes with every new link). The Android app path (sockets on
  the IMS Network) will not need this.
- Back: `ims off` (deregisters), kill the daemon, delete the rule, `start
  vendor.radio-a3y17lte`, then REBOOT until shannond 3702608 is in the build: the HAL's
  take-over left Android without the ICCID (no data). Check the NV files' owner afterwards
  (`ls -la /data/vendor/modem/nv`: radio:radio); builds before 7074691 left them root.

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
