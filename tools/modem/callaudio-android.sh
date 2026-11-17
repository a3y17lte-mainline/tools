#!/system/bin/sh
# Call audio hook for the shannond CLI daemon on Android (--audio-hook; the audio HAL does not know about the
# call): the values of tools/modem/voicecall.sh, earpiece + main mic.
C=/data/local/tmp/ctl  # tools/modem/ctl.c
log() { echo "callaudio: $*" > /dev/kmsg; }

log "call audio on"
SPK=$($C "Speaker Switch")
$C "Speaker Switch" off
$C "Mono Mix" Left
$C "Earpiece Playback Volume" 2
$C "MIC1 Volume" 11
$C "MIC1 Boost Volume" 2
$C "ADCL Mixer MIC1 Switch" on
$C "ADCR Mixer MIC1 Switch" on
$C "Modem Uplink Source" Codec
$C "Modem Playback Volume" 7
$C "Earpiece Switch" on
$C "DMIX Modem Switch" on

read -r _

log "call audio off"
$C "DMIX Modem Switch" off
$C "Earpiece Switch" off
$C "Modem Uplink Source" Mixer
$C "ADCL Mixer MIC1 Switch" off
$C "ADCR Mixer MIC1 Switch" off
$C "Mono Mix" Disable
$C "Speaker Switch" "${SPK:-on}"
