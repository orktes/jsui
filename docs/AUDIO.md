# Audio

JSUI plays UI sounds (hover, focus moves, press, confirm / back), music and
ambience that the UI asks for. Like [host textures](ARCHITECTURE.md#host-textures),
playback belongs to the host: the runtime does not mix audio itself, it hands
every request to an **audio handler**. A game engine implements the handler
with its own mixer, so UI sounds go through the game's buses, follow its
volume settings and effects, and pause with it. Hosts that do not care get a
**default implementation**:

| Host | Who plays the sounds |
| --- | --- |
| Native library, no handler (jsui-viewer, simple embedders) | Built-in backend: [miniaudio](https://miniaud.io), CMake option `JSUI_AUDIO` (ON by default on desktop) |
| Browser (`@orktes/jsui-web`) | Web Audio, in `web/jsui-audio.js` |
| Godot (`JsuiView`) | `AudioStreamPlayer`s on Godot's audio buses |
| Any engine | Your `JsuiConfig.on_audio` handler |
| Headless tools, tests | Nobody: `JsuiConfig.audio_disabled = 1` |

## In the UI (React)

```jsx
import { Pressable } from 'react-native';
import { Sound, useSound, useMusic, UISounds } from '@orktes/jsui-react';
import confirm from './sounds/confirm.wav';   // the bundler emits it next to the bundle
import tick from './sounds/tick.wav';
import menuMusic from './music/menu.ogg';

// Interaction sounds for every pressable element inside (an element's own
// `sounds` prop overrides per event; sounds={false} mutes one element).
<UISounds sounds={{ hoverIn: tick, focus: tick, press: confirm }} volume={0.8}>
  <Pressable onPress={start}>...</Pressable>
  <Pressable onPress={quit} sounds={{ press: { source: back, volume: 0.6 } }}>...</Pressable>
</UISounds>

// Music while the screen is mounted; a new source crossfades.
useMusic(menuMusic, { volume: 0.7, fadeIn: 1500, fadeOut: 800 });

// Sounds from code.
const click = useSound(confirm, { bus: 'ui' });  // preloaded on mount; click() plays it
const voice = Sound.play(explosion, { bus: 'sfx', volume: 0.9, pitch: 1.1, pan: -0.3 });
voice.setVolume(0.2, { fade: 300 });
voice.stop({ fade: 500 });
const { reason } = await voice.ended;            // 'finished' | 'stopped' | 'failed'

// Settings screens: the UI's own per-bus volume (multiplies its voices).
Sound.setBusVolume('music', 0.5, { fade: 200 });
Sound.setBusMuted('ui', true);
await Sound.preload(bossTheme);                  // -> { duration } (seconds, or null)
```

* **Sources** are URLs (relative ones resolve against the bundle URL) or
  `{ uri }`. Imported `.wav`, `.ogg`, `.mp3`, `.flac`, `.opus`, `.m4a`
  files evaluate to their URL. Which formats play depends on the host: the
  built-in backend decodes WAV, FLAC, MP3 and Ogg Vorbis; browsers decode
  what their Web Audio decodes (WAV, MP3, Ogg Vorbis / Opus except older
  Safari, AAC); Godot loads WAV, Ogg Vorbis and MP3.
* **Buses** are names: `'ui'` (UISounds / Pressable sounds), `'sfx'`
  (`Sound.play` default), `'music'` (`useMusic` default), or any other.
  Engines map them to their own buses.
* **Interaction events** (`sounds` / `UISounds`): `hoverIn` (mouse or pen
  enters; not touch), `focus` (keyboard or gamepad navigation moved focus
  here from another element; not clicks, not the first focus of a screen),
  `pressIn`, `press` (the press activated: onPress), `longPress`. Each entry
  is a source or `{ source, volume, pitch, bus }`. `UISounds` defaults apply
  only to elements that do something when pressed (`onPress`, `onPressIn`,
  `onLongPress`); an element's own `sounds` prop always applies.
  `useUISound()` plays them from custom widgets (sliders, carousels, tabs).
* `useMusic(source, { volume, bus, loop, fadeIn, fadeOut, pitch, paused })`
  loops by default; changing `volume` ramps (250 ms, `volumeFade`), `paused`
  pauses and resumes, `null` fades out.
* A reload (dev mode, `jsui_reload`) stops every voice.

## The host contract (C API)

See the "Audio" section of [`include/jsui/jsui.h`](../include/jsui/jsui.h).
The runtime calls `JsuiConfig.on_audio(user, const JsuiAudioCommand*)`
synchronously on its thread:

| `op` | Fields | Meaning |
| --- | --- | --- |
| `JSUI_AUDIO_PRELOAD` | `sound` | Load / decode ahead; answer with `jsui_audio_sound_loaded(rt, sound, ok, duration_s)` |
| `JSUI_AUDIO_UNLOAD` | `sound` | No longer needed (playing voices may finish) |
| `JSUI_AUDIO_PLAY` | `voice`, `sound`, `bus`, `volume`, `pitch`, `pan`, `loop`, `fade_ms` (fade in), `offset_s` | Start a voice |
| `JSUI_AUDIO_STOP` | `voice`, `fade_ms` | Fade out, then stop |
| `JSUI_AUDIO_PAUSE` / `_RESUME` | `voice` | |
| `JSUI_AUDIO_SET` | `voice`, `volume`, `pitch`, `pan`, `fade_ms` | Ramp to new values |
| `JSUI_AUDIO_BUS` | `bus`, `volume`, `muted`, `fade_ms` | The UI's volume for a group (buses start at 1, unmuted) |
| `JSUI_AUDIO_STOP_ALL` | `bus` (NULL: all), `fade_ms` | Also sent when the JS context goes away |

* `sound` is an absolute URL. Load it however the engine likes (a `res://`
  bundle gives `res://` sounds), or read the bytes with
  `jsui_fetch(rt, url, done, user)`, which goes through the runtime's loader
  (dev server, files, a custom `JsuiLoadFn`).
* Every `PLAY` must end with exactly one
  `jsui_audio_voice_ended(rt, voice, status)`: `JSUI_AUDIO_FINISHED` (played
  to the end), `JSUI_AUDIO_STOPPED` (`STOP` / `STOP_ALL`), or
  `JSUI_AUDIO_FAILED` (could not load or play). It may be called from inside
  the handler; JS hears about it in the next `jsui_tick()`. Looping voices
  end only when stopped.
* Volumes are linear gains, `pitch` is the playback rate (1 = normal),
  `pan` is -1 (left) to 1 (right).
* Keep the engine's own bus volumes separate from `JSUI_AUDIO_BUS`: the
  player's settings stay the game's; the UI's bus volume multiplies on top.

A handler sketch:

```c
static void on_audio(void* user, const JsuiAudioCommand* c) {
    GameAudio* audio = user;
    switch (c->op) {
    case JSUI_AUDIO_PLAY:
        game_audio_play(audio, c->voice, c->sound, c->bus, c->volume, c->pitch, c->loop);
        break;  /* game_audio calls jsui_audio_voice_ended(rt, voice, ...) when it ends */
    case JSUI_AUDIO_STOP: game_audio_stop(audio, c->voice, c->fade_ms); break;
    /* ... */
    default: break;
    }
}
cfg.on_audio = on_audio;
cfg.audio_user = &game_audio;
```

## Built-in backend (miniaudio)

Without `JsuiConfig.on_audio` (and without `audio_disabled`), the native
library plays the commands itself: `src/audio/audio_builtin.cpp` on
[miniaudio](https://miniaud.io) 0.11.25 (`cmake/Deps.cmake`, compiled in its
own target `jsui_miniaudio` from `src/audio/miniaudio.c`). The CMake option
`JSUI_AUDIO` is ON by default for desktop builds (Linux, macOS, Windows) and
OFF for Emscripten (the browser host uses Web Audio) and iOS; built OFF,
`audio_builtin.cpp` compiles to a stub and voices without a handler fail.
jsui-viewer uses it; jsui-render, jsui-record and the benchmarks set
`audio_disabled`.

* **Device**: the engine and its output device open on the first `PLAY` or
  `PRELOAD`, so a host that never plays a sound never opens one. miniaudio
  picks the platform's backend (WASAPI, Core Audio, PulseAudio / ALSA,
  loaded at run time on Linux; nothing to install at build time). If no
  device opens, that is logged once and every voice and preload fails.
  `JSUI_AUDIO_BACKEND=null` (environment) plays to miniaudio's null device,
  which runs in real time without hardware (CI, `tests/native/audio_builtin_test.cpp`);
  another backend name (`alsa`, `pulseaudio`, `jack`, `coreaudio`, `wasapi`,
  ...) forces that backend.
* **Loading**: bytes come through the runtime's loader (`rt.loader`), like
  the bundle and images: dev-server URLs, files, `data:` URIs and a custom
  `JsuiLoadFn` all work. A worker thread decodes them (WAV, FLAC, MP3, Ogg
  Vorbis via miniaudio's bundled stb_vorbis). Sounds up to 1 MB encoded and
  20 s long are decoded whole to f32 at the device rate and shared by their
  voices; longer ones are kept encoded and each voice decodes as it plays
  (music). One read and decode per URL until `UNLOAD`; a failed load is
  forgotten, so a later request retries. A `PLAY` that arrives before its
  bytes waits for them, and fails if they cannot be loaded or decoded.
* **Mixing**: one `ma_sound_group` per bus (`BUS` ramps the group's fader;
  mute sets its volume to 0). A voice's volume is its fader (fade in on
  `PLAY`, ramps on `SET`, the fade out on `STOP`); `SET` ramps pitch (the
  playback rate) and pan per tick. `STOP` with a fade reports `STOPPED`
  when the fade has finished; without a fade (or for a paused or waiting
  voice) right away.
* **Threads**: end-of-sound callbacks run on the audio thread and are only
  queued there; reports (`voiceEnded`, `soundLoaded`) and freeing finished
  voices happen in `AudioBackend::update()` on the runtime thread, every
  `jsui_tick()`. While decodes, fades or reports are pending,
  `jsui_wants_frame()` stays true.
* **Size** (Linux x64, Release, JSUI_AUDIO ON vs OFF): libjsui.a grows by
  about 100 KB (the backend) and links libjsui_miniaudio.a (about 1 MB
  archive, 640 KB of code and data); a stripped jsui-viewer grows by about
  690 KB. miniaudio is built without
  its resource manager, encoders and generators (`MA_NO_RESOURCE_MANAGER`,
  `MA_NO_ENCODING`, `MA_NO_GENERATION`).

## Web (Web Audio)

In the browser, `@orktes/jsui-web` plays the commands itself with Web Audio
(`web/jsui-audio.js`); `web/native/jsui_web.cpp` sets `JsuiConfig.on_audio`
to a trampoline that hands each command to the page's host object.

```
AudioBufferSourceNode (pitch = playbackRate, loop, offset)
  -> GainNode (voice volume, fades) -> StereoPannerNode (pan)
  -> GainNode per bus (Sound.setBusVolume / setBusMuted)
  -> GainNode master (jsui.audio.setVolume / muted) -> AudioContext.destination
```

* **One `AudioContext` per page**, shared by every runtime on it and closed
  when the last one is destroyed. Each runtime has its own master and bus
  gains. Decoded sounds are cached per page by URL (`fetch` +
  `decodeAudioData`, one load however many voices ask); `UNLOAD` drops the
  cache entry (playing voices keep their buffer).
* **Long files stream**: when a sound's `Content-Length` is over 2 MiB
  (`audio: { streamBytes }`), it is not decoded into memory (minutes of
  music would be hundreds of MB of PCM) but played through an `<audio>`
  element and a `MediaElementAudioSourceNode`, with the same gain / pan /
  bus chain.
* Fades and volume changes are `AudioParam` ramps (`linearRampToValueAtTime`;
  instant changes use a 4 ms `setTargetAtTime` so they do not click). `PAUSE`
  remembers the position and `RESUME` starts a new source there, with 12 ms
  fades either side.
* **Autoplay policy**: browsers allow sound only after a user gesture. The
  host creates (or resumes) the context inside the first `pointerdown`,
  `pointerup`, `mousedown`, `keydown`, `touchend` or `click` on the page,
  and resumes it when the page becomes visible again (a page that already
  had a gesture starts at once). Until audio is unlocked:
  * one-shot voices (UI sounds, effects) are **dropped and end `FAILED`**
    (`Sound.play(...).ended` resolves `{ reason: 'failed' }`), so nothing
    plays late or piles up. Sounds triggered within 400 ms of the unlocking
    gesture wait for the context instead (the press sound of the click that
    unlocked audio plays);
  * looping voices (`useMusic`) wait and start, with their fade-in, once
    audio unlocks; their file is fetched only then;
  * `PRELOAD` decodes at once (with an `OfflineAudioContext`), so UI sounds
    are ready for the first gesture.
* **Gamepads do not unlock audio**: no browser counts gamepad input as a
  user activation. A controller-only player hears nothing until they click,
  tap or press a key; show a hint ("Press any key / click to enable sound")
  and call `jsui.audio.unlock()` from gesture handlers of your own.
* Reports (`jsui_audio_voice_ended`, `jsui_audio_sound_loaded`) are queued
  and delivered in a microtask, never from inside `on_audio`. Voice ids
  cross into JS as doubles (`jsui_web_audio_voice_ended(rt, double, status)`).
* Without Web Audio (Node: `@orktes/jsui-cli` screenshot / inspect / run)
  the runtime is created with `audio_disabled`: voices end `failed`, preloads
  reject, nothing throws.

Page API (`createJsui()` returns the host):

```js
jsui.audio.unlocked                         // can sound play now?
jsui.on('audiounlock', () => ...)           // it can (again: also after an iOS interruption)
jsui.audio.unlock()                         // call from a gesture handler of your own
jsui.audio.setVolume(0.5, { fade: 300 })    // master volume of this runtime
jsui.audio.muted = true
jsui.audio.context                          // the AudioContext, or null before the first gesture
jsui.audio.stats                            // { voices, playing, waiting, sounds }

createJsui({ ..., audio: false })                         // silence (audio_disabled)
createJsui({ ..., audio: { context: gameAudioContext } }) // play into the game's own context (not closed)
createJsui({ ..., audio: (cmd, audio) => { ... } })       // your own player, like JsuiConfig.on_audio:
// cmd = { op, voice, sound, bus, volume, pitch, pan, fadeMs, offset, loop, muted } (AudioOp from
// '@orktes/jsui-web'); report with audio.voiceEnded(voice, AudioStatus.*) and
// audio.soundLoaded(url, ok, durationSeconds).
```

The showcase site (`site/`) uses all of it: a `<UISounds>` set around the
whole app, menu music that crossfades between game screens, live mix sliders
on the Settings screen, and a sound switch in the nav bar that reads "Enable
sound" until the browser unlocks audio. Its sounds and music loops are
synthesized by `site/scripts/gen-ui-sounds.mjs` (CC0).
