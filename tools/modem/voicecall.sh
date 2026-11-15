#!/bin/sh
# Call audio hook for samsungipcd on the Galaxy A3 2017 (mainline): samsungipcd runs this when a
# call starts and writes a line to its stdin when the call ends. Installed by build-ram-img as
# /usr/libexec/samsungipcd/voicecall.sh.
#
# Earpiece and main microphone to and from the modem, with the values of stock Android's
# incall_*-handset paths (mixer_paths.xml, mixer_gains.xml): mono mix left, earpiece +2 dB,
# MIC1 volume 11 with boost 2, the modem downlink into the mixer at 0 dB, the codec as uplink.
# The loudspeaker amplifier sits on the same codec bus as the codec, so its "Speaker" pin switch
# is turned off for the call (otherwise the call plays on the speaker) and restored afterwards.
C="amixer -q -c a3y17lte cset"
log() { echo "voicecall: $*" > /dev/kmsg; }

log "call audio on"
SPK=$(amixer -c a3y17lte cget name="Speaker Switch" | sed -n "s/.*: values=//p")
$C name="Speaker Switch" off
$C name="Mono Mix" Left
$C name="Earpiece Playback Volume" 2
$C name="MIC1 Volume" 11
$C name="MIC1 Boost Volume" 2
$C name="ADCL Mixer MIC1 Switch" on
$C name="ADCR Mixer MIC1 Switch" on
$C name="Modem Uplink Source" Codec
$C name="Modem Playback Volume" 7
$C name="Earpiece Switch" on
$C name="DMIX Modem Switch" on

read -r _

log "call audio off"
$C name="DMIX Modem Switch" off
$C name="Earpiece Switch" off
$C name="Modem Uplink Source" Mixer
$C name="ADCL Mixer MIC1 Switch" off
$C name="ADCR Mixer MIC1 Switch" off
$C name="Mono Mix" Disable
$C name="Speaker Switch" "${SPK:-on}"
