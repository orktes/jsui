# Sound

The host plays the UI's sounds (Godot: its audio buses; browser: Web Audio;
native viewer: built-in miniaudio). The UI only asks. Full details:
docs/AUDIO.md in the repo.

```jsx
import { Pressable } from 'react-native';
import { Sound, useSound, useMusic, UISounds, useUISound } from '@orktes/jsui-react';
import tick from './sounds/tick.wav';        // imports evaluate to a URL next to the bundle
import confirm from './sounds/confirm.wav';
import back from './sounds/back.wav';
import theme from './music/menu.ogg';

function MainMenu() {
  useMusic(theme, { volume: 0.6, fadeIn: 1200, fadeOut: 800 });   // loops while mounted
  return (
    <UISounds sounds={{ hoverIn: tick, focus: tick, press: confirm }} volume={0.8}>
      <Pressable onPress={play}>...</Pressable>
      <Pressable onPress={goBack} sounds={{ press: back }}>...</Pressable>
      <Pressable onPress={noop} sounds={false}>...</Pressable>  {/* silent */}
    </UISounds>
  );
}
```

- Put one `<UISounds>` near the root of a screen (or the app) instead of
  `sounds` on every element. Defaults apply to elements with `onPress` /
  `onPressIn` / `onLongPress`.
- Events: `hoverIn` (mouse, not touch), `focus` (keyboard / gamepad
  navigation only, so clicks don't double up with `press`), `pressIn`,
  `press`, `longPress`. Entry: a source or `{ source, volume, pitch, bus }`.
- `Sound.play(src, { bus: 'sfx', volume, pitch, pan, loop, fadeIn, offset })`
  returns a voice: `stop({ fade })`, `pause()`, `resume()`,
  `setVolume(v, { fade })`, `setPitch`, `setPan`, `await voice.ended` ->
  `{ reason: 'finished' | 'stopped' | 'failed' }`.
- `const play = useSound(src, defaults)`: preloaded on mount; `play()` per use.
- `useUISound()` -> `(event) => void` for custom widgets (slider steps,
  carousel moves, tab switches) using the surrounding UISounds set.
- Settings screens: `Sound.setBusVolume('music', v)`, `Sound.setBusMuted('ui', true)`,
  `Sound.getBus(name)`. These are the UI's own bus gains on top of the engine's.
- Buses: 'ui' (interaction sounds), 'sfx' (Sound.play), 'music' (useMusic).
- Keep UI sounds short (< 300 ms), quiet (volume 0.4-0.8) and varied a little
  (`pitch: 0.95 + Math.random() * 0.1` for repeated ticks).
- Browsers block audio until the first click / key press; never rely on a
  sound for information, and don't start critical flows from `voice.ended`
  (a voice that cannot play ends with 'failed').
- Headless tools (`jsui screenshot` / `run`) are silent.
