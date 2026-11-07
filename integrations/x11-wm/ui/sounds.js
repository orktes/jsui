// The shell's own sounds, played by the host (JSUI's Sound: on Ubuntu Touch
// through PulseAudio's default sink, so the system volume and mute apply):
// a soft chime when a toast comes up, a tick when the volume buttons change
// the level.
import { Sound } from 'react-native';
import notifyWav from './sounds/notify.wav';
import tickWav from './sounds/tick.wav';

let lastTick = 0;

export function playNotify() {
  Sound.play(notifyWav, { bus: 'ui', volume: 0.7 });
}

// Held volume buttons repeat: one tick per 60 ms at most.
export function playTick() {
  const now = Date.now();
  if (now - lastTick < 60) return;
  lastTick = now;
  Sound.play(tickWav, { bus: 'ui', volume: 0.6 });
}
