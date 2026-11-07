#!/bin/sh
# Speaker sound on the Galaxy Tab A7: run on the tablet as the phablet user,
#
#   sh ~/jsui-wm/audio-on.sh            # try it, then keep it if you heard the tone
#   sh ~/jsui-wm/audio-on.sh --remove   # stop doing it at boot
#
# 1. binds the audio HAL wrapper (sudo: your PIN), restarts PulseAudio and
#    plays a tone; nothing is kept yet (a reboot undoes it);
# 2. if you heard the tone, installs jsui-wm-audio.service so it is done at
#    every boot. Safe to run again.
HERE=$(dirname "$(readlink -f "$0")")
if [ "$1" = "--remove" ]; then
  sudo systemctl disable jsui-wm-audio.service 2>/dev/null
  sudo rm -rf /etc/systemd/system/jsui-wm-audio.service /etc/writable/jsui-wm-audio
  sudo systemctl daemon-reload
  echo "jsui-wm-audio.service removed (the binds go at the next reboot)."
  exit 0
fi
echo "== 1/2: speaker sound (sudo may ask for your PIN)"
sudo sh "$HERE/jsui-wm-audio-up" || { echo "binding the audio HAL wrapper failed; nothing installed."; exit 1; }
# Our own PulseAudio too, if the root side could not reach it.
systemctl --user restart pulseaudio.service 2>/dev/null || pulseaudio -k 2>/dev/null
for i in $(seq 20); do pactl list short sinks 2>/dev/null | grep -q primary && break; sleep 1; done
pactl list short sinks
if ! pactl list short sinks 2>/dev/null | grep -q primary; then
  echo "PulseAudio has no speaker sink (sink.primary-out): the bridge still failed; nothing installed."
  echo "Logs: /tmp/jsui-wm-audio.log, and journalctl --user -b -u pulseaudio | tail -40"
  exit 1
fi
pactl set-default-sink sink.primary-out
pactl set-sink-mute sink.primary-out 0
pactl set-sink-volume sink.primary-out 50%
echo "Playing a tone..."
for i in 1 2 3; do paplay /usr/share/sounds/lomiri/notifications/A440.ogg 2>/dev/null || paplay /usr/share/sounds/lomiri/notifications/Blip.ogg; done
printf "Did you hear the tone? [y/N] "
read -r heard
case "$heard" in y*|Y*) ;; *) echo "Not installed; a reboot undoes what was done."; exit 1 ;; esac
echo "== 2/2: speaker sound at every boot"
sudo install -m 755 -D "$HERE/jsui-wm-audio-up" /etc/writable/jsui-wm-audio/jsui-wm-audio-up &&
  sudo install -m 644 "$HERE/jsui-wm-audio-policy.xml" /etc/writable/jsui-wm-audio/jsui-wm-audio-policy.xml &&
  sudo install -m 644 "$HERE/jsui-wm-audio.service" /etc/systemd/system/jsui-wm-audio.service &&
  sudo systemctl daemon-reload &&
  sudo systemctl enable jsui-wm-audio.service &&
  echo "jsui-wm-audio.service installed and enabled. (Undo: sh $HERE/audio-on.sh --remove)" ||
  { echo "installing jsui-wm-audio.service failed."; exit 1; }
