// Settings (interactive). Feature spotlight: real controls and focus.
//
// Five working tabs (click, LB / RB or Q / E); rows are navigated by focus
// (D-pad / arrows / Tab) and every control works with every input: sliders
// are PanResponder drags or left / right steps, toggles, segmented
// selectors, dropdown menus with their own focus loop, a TextInput callsign
// and key-binding capture fields. Changes are tracked against the saved
// values (row and tab markers); Apply (X / F) or Revert (Y / R) settle them,
// and the preview in the help panel reacts live.
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Animated, Easing, Image, PanResponder, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';

export const feature = {
  title: 'Real controls & focus',
  points: [
    'Sliders are PanResponder drags that also step with ← → and the D-pad',
    'A TextInput callsign with caret, selection and clipboard, and key-binding fields that capture the next key or button',
    'Dropdowns open their own focus loop; Esc / B closes them and focus returns to the row',
    'Every change is tracked: changed rows and tabs are marked until you Apply or Revert',
  ],
  keys: ['↑ ↓ / D-pad: move, ← →: change value', 'Q E / LB RB: switch tab', 'F / X: apply · R / Y: revert'],
};
import { colors, font } from '../../../examples/theme.js';

const PREVIEWS = {
  gameplay: require('../../../examples/assets/card-ember.png'),
  graphics: require('../../../examples/assets/card-tide.png'),
  audio: require('../../../examples/assets/card-void.png'),
  controls: require('../../../examples/assets/card-verdant.png'),
  accessibility: require('../../../examples/assets/card-tide.png'),
};

const PRESETS = ['Low', 'Medium', 'High', 'Ultra', 'Custom'];
const PRESET_VALUES = {
  Low: { shadows: 'Low', motionBlur: false, rt: false, fps: 60 },
  Medium: { shadows: 'Medium', motionBlur: false, rt: false, fps: 120 },
  High: { shadows: 'High', motionBlur: true, rt: false, fps: 144 },
  Ultra: { shadows: 'Epic', motionBlur: true, rt: true, fps: 144 },
};

const TABS = [
  {
    id: 'gameplay',
    label: 'Gameplay',
    rows: [
      { section: 'Challenge' },
      { key: 'difficulty', label: 'Difficulty', type: 'seg', options: ['Story', 'Normal', 'Hard', 'Mythic'], help: 'Scales enemy health, damage and how forgiving checkpoints are. You can change this at any time outside combat.', impact: 0 },
      { key: 'aimAssist', label: 'Aim Assist', sub: 'Controller only', type: 'toggle', help: 'Gently slows and pulls your reticle toward targets while aiming with a controller.', impact: 0 },
      { key: 'shake', label: 'Camera Shake', type: 'slider', min: 0, max: 100, step: 5, fmt: (v) => v + '%', help: 'Strength of impacts, explosions and heavy footsteps on the camera.', impact: 0 },
      { section: 'Profile' },
      { key: 'callsign', label: 'Callsign', sub: 'Shown to your party', type: 'text', help: 'A real TextInput: click or press Enter / A to edit, with caret, selection and clipboard. Enter or Esc returns to the row.', impact: 0 },
      { section: 'Interface' },
      { key: 'hudOpacity', label: 'HUD Opacity', type: 'slider', min: 20, max: 100, step: 5, fmt: (v) => v + '%', help: 'Fades the combat HUD so more of the world shows through.', impact: 0 },
      { key: 'autoLoot', label: 'Auto-loot Commons', type: 'toggle', help: 'Picks up common and uncommon items automatically when you walk over them.', impact: 0 },
      { key: 'language', label: 'Subtitle Language', type: 'drop', options: ['English', 'Suomi', 'Deutsch', 'Français', '日本語'], help: 'Language for subtitles and interface text. Voice language is set per installed pack.', impact: 0 },
    ],
  },
  {
    id: 'graphics',
    label: 'Graphics',
    header: 'preset',
    rows: [
      { section: 'Display' },
      { key: 'resolution', label: 'Resolution', sub: 'Native 2560 × 1440', type: 'drop', options: ['1920 × 1080 (16:9)', '2560 × 1440 (16:9)', '3440 × 1440 (21:9)', '3840 × 2160 (16:9)'], help: 'Rendering resolution of the game world. The interface always renders at native resolution.', impact: 4 },
      { key: 'windowMode', label: 'Window Mode', type: 'seg', options: ['Fullscreen', 'Borderless', 'Windowed'], help: 'Borderless keeps alt-tab instant; exclusive fullscreen can lower input latency slightly.', impact: 1 },
      { key: 'vsync', label: 'Vertical Sync', type: 'toggle', help: 'Synchronises frames with the display to remove tearing, at the cost of a little latency.', impact: 1 },
      { key: 'fps', label: 'Frame Rate Limit', type: 'slider', min: 30, max: 240, step: 1, kstep: 5, fmt: (v) => (v >= 240 ? 'Unlimited' : v + ' FPS'), help: "Caps the maximum frames rendered per second. Matching your display's refresh rate reduces power draw and keeps frame pacing smooth.", impact: 2 },
      { key: 'fov', label: 'Field of View', type: 'slider', min: 60, max: 120, step: 1, kstep: 2, fmt: (v) => v + '°', help: 'Horizontal field of view. Wider shows more of the world but makes distant things smaller. Watch the preview.', impact: 2 },
      { section: 'Quality' },
      { key: 'shadows', label: 'Shadow Quality', type: 'drop', options: ['Off', 'Low', 'Medium', 'High', 'Epic'], quality: true, help: 'Resolution and filtering of dynamic shadows. Epic adds contact shadows.', impact: 3 },
      { key: 'motionBlur', label: 'Motion Blur', type: 'toggle', quality: true, help: 'Per-object motion blur. Purely cosmetic; some players find it uncomfortable.', impact: 1 },
      { key: 'rt', label: 'Ray-Traced Reflections', sub: 'Requires a compatible GPU', type: 'toggle', quality: true, help: 'Accurate reflections on water, metal and glass. The single most expensive option here.', impact: 5 },
    ],
  },
  {
    id: 'audio',
    label: 'Audio',
    rows: [
      { section: 'Volume' },
      { key: 'master', label: 'Master Volume', type: 'slider', min: 0, max: 100, step: 1, kstep: 5, fmt: (v) => v + '%', help: 'Overall output level. The meter above follows it.', impact: 0, accent: colors.gold },
      { key: 'music', label: 'Music', type: 'slider', min: 0, max: 100, step: 1, kstep: 5, fmt: (v) => v + '%', help: 'Score and ambient music.', impact: 0 },
      { key: 'sfx', label: 'Effects', type: 'slider', min: 0, max: 100, step: 1, kstep: 5, fmt: (v) => v + '%', help: 'Combat, footsteps, spells and the world.', impact: 0 },
      { key: 'voice', label: 'Dialogue', type: 'slider', min: 0, max: 100, step: 1, kstep: 5, fmt: (v) => v + '%', help: 'Spoken dialogue and party chatter.', impact: 0 },
      { section: 'Output' },
      { key: 'speakers', label: 'Speaker Setup', type: 'seg', options: ['Stereo', 'Headphones', '5.1', '7.1'], help: 'Headphones enables binaural spatial audio.', impact: 1 },
      { key: 'subtitles', label: 'Subtitles', type: 'toggle', help: 'Show subtitles for dialogue and important sounds.', impact: 0 },
      { key: 'bgMute', label: 'Mute in Background', type: 'toggle', help: 'Silences the game while another window has focus.', impact: 0 },
    ],
  },
  {
    id: 'controls',
    label: 'Controls',
    rows: [
      { section: 'Camera' },
      { key: 'sens', label: 'Look Sensitivity', type: 'slider', min: 1, max: 10, step: 0.1, kstep: 0.5, fmt: (v) => v.toFixed(1), help: 'How fast the camera turns with the right stick or mouse.', impact: 0, accent: colors.gold },
      { key: 'invertY', label: 'Invert Y Axis', type: 'toggle', help: 'Pushing up looks down, like a flight stick.', impact: 0 },
      { key: 'deadzone', label: 'Stick Dead Zone', type: 'slider', min: 0, max: 30, step: 1, fmt: (v) => v + '%', help: 'Ignore small stick movements. Raise it if the camera drifts on its own.', impact: 0 },
      { section: 'Input' },
      { key: 'interact', label: 'Interact', type: 'bind', help: 'Press Enter / A, then any key or gamepad button to bind it. Esc cancels.', impact: 0 },
      { key: 'dodge', label: 'Dodge', type: 'bind', help: 'Press Enter / A, then any key or gamepad button to bind it. Esc cancels.', impact: 0 },
      { key: 'layout', label: 'Button Layout', type: 'seg', options: ['Default', 'Southpaw', 'Legacy'], help: 'Southpaw swaps the sticks; Legacy moves dodge to the shoulder.', impact: 0 },
      { key: 'vibration', label: 'Vibration', type: 'slider', min: 0, max: 100, step: 5, fmt: (v) => (v === 0 ? 'Off' : v + '%'), help: 'Controller rumble strength for hits, abilities and the world.', impact: 0 },
      { key: 'sprint', label: 'Hold to Sprint', type: 'toggle', help: 'Off: click the stick once to toggle sprinting.', impact: 0 },
    ],
  },
  {
    id: 'accessibility',
    label: 'Accessibility',
    rows: [
      { section: 'Vision' },
      { key: 'textSize', label: 'Text Size', type: 'seg', options: ['S', 'M', 'L', 'XL'], help: 'Scales subtitles and interface text.', impact: 0 },
      { key: 'colorblind', label: 'Colour Filter', type: 'drop', options: ['Off', 'Protanopia', 'Deuteranopia', 'Tritanopia', 'Monochrome'], help: 'Remaps the palette so rarity, team and danger colours stay distinct. Monochrome shows on the preview.', impact: 0 },
      { key: 'brightness', label: 'Brightness', type: 'slider', min: 0, max: 100, step: 1, kstep: 5, fmt: (v) => String(v), help: 'Adjust until the emblem in the preview is barely visible in the darkest shadows.', impact: 0, accent: colors.gold },
      { section: 'Comfort' },
      { key: 'reduceMotion', label: 'Reduce Motion', type: 'toggle', help: 'Tones down camera bob, screen shake and large interface animations.', impact: 0 },
      { key: 'flash', label: 'Flash Intensity', type: 'slider', min: 0, max: 100, step: 5, fmt: (v) => v + '%', help: 'Limits bright full-screen flashes from explosions and spells.', impact: 0 },
      { key: 'holdToggle', label: 'Hold Actions as Toggles', type: 'toggle', help: 'Every "hold" input becomes a press to start and a press to stop.', impact: 0 },
    ],
  },
];

const DEFAULTS = {
  difficulty: 1, aimAssist: true, shake: 70, hudOpacity: 100, autoLoot: true, language: 0,
  preset: 4, resolution: 1, windowMode: 1, vsync: true, fps: 144, fov: 90, shadows: 3, motionBlur: false, rt: true,
  master: 80, music: 60, sfx: 85, voice: 90, speakers: 1, subtitles: true, bgMute: false,
  sens: 5.5, invertY: false, deadzone: 8, layout: 0, vibration: 70, sprint: true,
  callsign: 'Valkyrie_07', interact: 'E', dodge: 'Space',
  textSize: 1, colorblind: 0, brightness: 50, reduceMotion: false, flash: 100, holdToggle: false,
};
const ALL_ROWS = {};
TABS.forEach((t) => t.rows.forEach((r) => r.key && (ALL_ROWS[r.key] = r)));
const SHADOW_OPTIONS = ALL_ROWS.shadows.options;

const TRACK_W = 196;
const ROW_H = 52;
const spring = (v, toValue, native = true) => Animated.spring(v, { toValue, stiffness: 420, damping: 30, mass: 0.8, useNativeDriver: native });
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

// --- controls ---------------------------------------------------------------

const Toggle = memo(function Toggle({ on }) {
  const x = useRef(new Animated.Value(on ? 1 : 0)).current;
  useEffect(() => spring(x, on ? 1 : 0).start(), [on]);
  return (
    <View style={[styles.toggle, on && styles.toggleOn]}>
      <Animated.View style={[styles.knob, on && styles.knobOn, { transform: [{ translateX: x.interpolate({ inputRange: [0, 1], outputRange: [0, 20] }) }] }]} />
    </View>
  );
});

const Slider = memo(function Slider({ row, value, onChange }) {
  const p = (value - row.min) / (row.max - row.min);
  const pos = useRef(new Animated.Value(p)).current;
  const [dragging, setDragging] = useState(false);
  useEffect(() => spring(pos, p).start(), [p]);
  const fromX = (x) => {
    const raw = row.min + clamp(x / TRACK_W, 0, 1) * (row.max - row.min);
    const v = Math.round(raw / row.step) * row.step;
    onChange(row.key, +clamp(v, row.min, row.max).toFixed(2));
  };
  const accent = row.accent || colors.cyan;
  const fromXRef = useRef(fromX);
  fromXRef.current = fromX;
  // locationX is in the slider's own (unscaled) coordinates, so drags stay
  // exact however the Stage scales the screen.
  const pan = useMemo(
    () =>
      PanResponder.create({
        onStartShouldSetPanResponder: () => true,
        onMoveShouldSetPanResponder: () => true,
        onPanResponderGrant: (e) => {
          setDragging(true);
          fromXRef.current(e.nativeEvent.locationX - 8);
        },
        onPanResponderMove: (e) => fromXRef.current(e.nativeEvent.locationX - 8),
        onPanResponderRelease: () => setDragging(false),
        onPanResponderTerminate: () => setDragging(false),
      }),
    [],
  );
  return (
    <View style={styles.sliderWrap}>
      <View style={styles.sliderHit} {...pan.panHandlers}>
        <View pointerEvents="none" style={styles.sliderTrack}>
          <Animated.View style={[styles.sliderFill, { backgroundColor: accent, transform: [{ translateX: pos.interpolate({ inputRange: [0, 1], outputRange: [-TRACK_W, 0] }) }] }]} />
        </View>
        <Animated.View
          pointerEvents="none"
          style={[
            styles.thumb,
            { borderColor: accent, transform: [{ translateX: pos.interpolate({ inputRange: [0, 1], outputRange: [0, TRACK_W] }) }, { scale: dragging ? 1.25 : 1 }] },
            dragging && { boxShadow: `0 0 0 6px ${accent}33, 0 2px 6px rgba(0,0,0,0.5)` },
          ]}
        />
      </View>
      <Text style={styles.sliderValue}>{row.fmt(value)}</Text>
    </View>
  );
});

const Segmented = memo(function Segmented({ options, value, onSelect, small }) {
  const lay = useRef([]).current;
  const x = useRef(new Animated.Value(0)).current;
  const w = useRef(new Animated.Value(0)).current;
  const [ready, setReady] = useState(false);
  const place = (animate) => {
    const l = lay[value];
    if (!l) return;
    if (animate) {
      spring(x, l.x, false).start();
      spring(w, l.width, false).start();
    } else {
      x.setValue(l.x);
      w.setValue(l.width);
    }
  };
  useEffect(() => place(true), [value]);
  return (
    <View style={styles.segmented}>
      <Animated.View style={[styles.segmentActive, { opacity: ready ? 1 : 0, width: w, transform: [{ translateX: x }] }]} />
      {options.map((o, i) => (
        <Pressable
          key={o}
          focusable={false}
          onPress={() => onSelect(i)}
          onLayout={(e) => {
            lay[i] = e.nativeEvent.layout;
            if (i === value) {
              place(false);
              setReady(true);
            }
          }}
          style={({ hovered }) => [styles.segment, small && { paddingHorizontal: 9 }, hovered && i !== value && { backgroundColor: 'rgba(255,255,255,0.05)' }]}
        >
          <Text style={[styles.segmentText, i === value && styles.segmentTextActive]}>{o}</Text>
        </Pressable>
      ))}
    </View>
  );
});

function Dropdown({ value, open }) {
  return (
    <View style={[styles.dropdown, open && styles.dropdownOpen]}>
      <Text style={styles.dropdownText}>{value}</Text>
      <View style={[styles.caret, open && { transform: [{ rotate: '225deg' }], marginTop: 4 }]} />
    </View>
  );
}

// --- rows ---------------------------------------------------------------------

const PAD_NAMES = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'View', 'Menu', 'L3', 'R3', 'D-pad ↑', 'D-pad ↓', 'D-pad ←', 'D-pad →'];
const keyLabel = (k) =>
  k === ' ' ? 'Space' : k.length === 1 ? k.toUpperCase() : { ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→' }[k] || k;

const Row = memo(function Row({ autoFocus, row, value, changed, open, onChange, onStep, onActivate, onHot, registerRef, onRowLayout }) {
  const [listening, setListening] = useState(false);
  const input = useRef(null);
  const self = useRef(null);
  const stop = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const keyStep = (e, dir) => {
    stop(e);
    onStep(row, dir);
  };
  const press = () => {
    if (row.type === 'text') input.current && input.current.focus();
    else if (row.type === 'bind') setListening((l) => !l);
    else onActivate(row);
  };
  return (
    <Pressable
      ref={(el) => {
        self.current = el;
        registerRef(row.key, el);
      }}
      autoFocus={autoFocus}
      onPress={press}
      onFocus={() => onHot(row, true)}
      onBlur={() => setListening(false)}
      onHoverIn={() => onHot(row, false)}
      onLayout={onRowLayout ? (e) => onRowLayout(row.key, e.nativeEvent.layout) : undefined}
      onKeyDown={(e) => {
        if (listening) {
          stop(e);
          if (e.key !== 'Escape') onChange(row.key, keyLabel(e.key));
          setListening(false);
        } else if (e.key === 'ArrowLeft') keyStep(e, -1);
        else if (e.key === 'ArrowRight') keyStep(e, 1);
      }}
      onGamepadButton={(e) => {
        if (!e.pressed) return;
        if (listening) {
          stop(e);
          onChange(row.key, PAD_NAMES[e.buttonIndex] || 'Pad ' + e.buttonIndex);
          setListening(false);
        } else if (e.buttonIndex === 14) keyStep(e, -1);
        else if (e.buttonIndex === 15) keyStep(e, 1);
      }}
      style={{ cursor: 'pointer', zIndex: open ? 5 : 0 }}
    >
      {({ hovered, focused }) => (
        <View style={[styles.row, hovered && styles.rowHover, focused && styles.rowFocused]}>
          <View style={[styles.changed, { opacity: changed ? 1 : 0 }]} />
          <View style={{ flex: 1 }}>
            <Text style={[styles.rowLabel, (focused || hovered) && { color: '#fff' }]}>{row.label}</Text>
            {row.sub ? <Text style={styles.rowSub}>{row.sub}</Text> : null}
          </View>
          <View style={styles.control}>
            {row.type === 'toggle' ? <Toggle on={value} /> : null}
            {row.type === 'slider' ? <Slider row={row} value={value} onChange={onChange} /> : null}
            {row.type === 'seg' ? <Segmented options={row.options} value={value} onSelect={(i) => onChange(row.key, i)} /> : null}
            {row.type === 'drop' ? <Dropdown value={row.options[value]} open={open} /> : null}
            {row.type === 'text' ? (
              <TextInput
                ref={input}
                value={value}
                maxLength={18}
                onChangeText={(t) => onChange(row.key, t)}
                onSubmitEditing={() => self.current && self.current.focus()}
                onKeyDown={(e) => {
                  e.stopPropagation(); // typing must not trigger screen shortcuts
                  if (e.key === 'Escape') {
                    e.preventDefault();
                    if (self.current) self.current.focus();
                  }
                }}
                onFocus={() => onHot(row, true)}
                selectTextOnFocus
                cursorColor={colors.gold}
                selectionColor="rgba(242,195,91,0.35)"
                style={styles.input}
              />
            ) : null}
            {row.type === 'bind' ? (
              <View style={[styles.bind, listening && styles.bindListening]}>
                <Text style={[styles.bindText, listening && { color: colors.gold }]}>{listening ? 'Press a key or button…' : value}</Text>
              </View>
            ) : null}
          </View>
          {focused && (row.type === 'slider' || row.type === 'seg') ? (
            <View style={styles.arrows} pointerEvents="none">
              <Text style={styles.arrowText}>◀ ▶</Text>
            </View>
          ) : null}
        </View>
      )}
    </Pressable>
  );
});

function Menu({ row, value, y, onPick, onClose }) {
  const refs = useRef([]).current;
  const [hot, setHot] = useState(value);
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    Animated.timing(t, { toValue: 1, duration: 160, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    const id = setTimeout(() => refs[value] && refs[value].focus(), 0);
    return () => clearTimeout(id);
  }, []);
  const move = (e, d) => {
    e.preventDefault();
    e.stopPropagation();
    const n = (hot + d + row.options.length) % row.options.length;
    setHot(n);
    if (refs[n]) refs[n].focus();
  };
  const close = (e) => {
    e.preventDefault();
    e.stopPropagation();
    onClose();
  };
  return (
    <>
      <Pressable focusable={false} style={styles.backdrop} onPress={onClose} />
      <Animated.View
        style={[styles.menu, { top: y + ROW_H - 7, opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [-6, 0] }) }] }]}
      >
        {row.options.map((o, i) => (
          <Pressable
            key={o}
            ref={(r) => (refs[i] = r)}
            onPress={() => onPick(i)}
            onFocus={() => setHot(i)}
            onHoverIn={() => setHot(i)}
            onKeyDown={(e) => {
              if (e.key === 'ArrowDown') move(e, 1);
              else if (e.key === 'ArrowUp') move(e, -1);
              else if (e.key === 'Escape' || e.key === 'Backspace') close(e);
              else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight' || e.key === 'Tab') e.preventDefault();
            }}
            onGamepadButton={(e) => {
              if (!e.pressed) return;
              if (e.buttonIndex === 13) move(e, 1);
              else if (e.buttonIndex === 12) move(e, -1);
              else if (e.buttonIndex === 1) close(e);
              else if (e.buttonIndex === 14 || e.buttonIndex === 15) e.preventDefault();
            }}
            style={[styles.menuItem, i === hot && styles.menuItemActive, { cursor: 'pointer' }]}
          >
            <Text style={[styles.menuText, (i === hot || i === value) && { color: '#fff' }]}>{o}</Text>
            {i === value ? <View style={styles.checkMark} /> : null}
          </Pressable>
        ))}
      </Animated.View>
    </>
  );
}

// --- help panel ----------------------------------------------------------------

function Meter({ level, active }) {
  const bars = useRef(Array.from({ length: 14 }, () => new Animated.Value(0.3))).current;
  useEffect(() => {
    if (!active) return undefined;
    const loops = bars.map((b, i) =>
      Animated.loop(
        Animated.sequence([
          Animated.timing(b, { toValue: 0.35 + ((i * 37) % 60) / 100, duration: 260 + ((i * 53) % 220), easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
          Animated.timing(b, { toValue: 0.15 + ((i * 17) % 30) / 100, duration: 300 + ((i * 29) % 260), easing: Easing.inOut(Easing.quad), useNativeDriver: true }),
        ]),
      ),
    );
    loops.forEach((l) => l.start());
    return () => loops.forEach((l) => l.stop());
  }, [active]);
  return (
    <View style={styles.meter}>
      {bars.map((b, i) => (
        <Animated.View
          key={i}
          style={[styles.meterBar, { opacity: 0.35 + level * 0.65, transform: [{ scaleY: Animated.multiply(b, 0.1 + level * 0.9) }] }]}
        />
      ))}
    </View>
  );
}

function previewFilter(tab, v) {
  const f = [];
  if (tab === 'graphics') {
    const p = PRESETS[v.preset];
    if (p === 'Low') f.push('blur(2px)', 'saturate(0.6)');
    else if (p === 'Medium') f.push('blur(0.8px)', 'saturate(0.85)');
    else if (p === 'Ultra') f.push('saturate(1.25)', 'contrast(1.08)');
    if (v.shadows === 0) f.push('brightness(1.15)');
  }
  if (v.colorblind === 4) f.push('grayscale(1)');
  if (tab === 'accessibility') f.push(`brightness(${(0.45 + v.brightness / 90).toFixed(2)})`);
  return f.length ? f.join(' ') : undefined;
}

function Help({ tab, row, values, active }) {
  const fade = useRef(new Animated.Value(1)).current;
  const id = row ? row.key : tab.id;
  useEffect(() => {
    fade.setValue(0.2);
    Animated.timing(fade, { toValue: 1, duration: 220, useNativeDriver: true }).start();
  }, [id]);
  const zoom = tab.id === 'graphics' ? 1 + (120 - values.fov) / 110 : 1.02;
  return (
    <View style={styles.help}>
      <View style={styles.previewBox}>
        <Image source={PREVIEWS[tab.id]} style={[styles.preview, { transform: [{ scale: zoom }], filter: previewFilter(tab.id, values) }]} resizeMode="cover" />
        {tab.id === 'audio' ? <Meter level={values.master / 100} active={active} /> : null}
        {tab.id === 'gameplay' ? (
          <View style={[styles.miniHud, { opacity: values.hudOpacity / 100 }]}>
            <View style={styles.miniHp}>
              <View style={[styles.miniHpFill, { width: ['90%', '70%', '45%', '25%'][values.difficulty] }]} />
            </View>
            <Text style={styles.miniHudText}>{ALL_ROWS.difficulty.options[values.difficulty]}</Text>
          </View>
        ) : null}
        <View style={styles.previewTag}>
          <Text style={styles.previewTagText}>Live preview</Text>
        </View>
      </View>
      <Animated.View style={[styles.helpBody, { opacity: fade }]}>
        <Text style={styles.helpTitle}>{row ? row.label : tab.label}</Text>
        <Text style={styles.helpText}>{row ? row.help : 'Move between settings with the D-pad or arrow keys. Left and right change the focused value.'}</Text>
        <Text style={styles.impactLabel}>Performance impact</Text>
        <View style={styles.impact}>
          {[0, 1, 2, 3, 4].map((i) => {
            const n = row ? row.impact : 0;
            const c = n >= 4 ? colors.red : n >= 3 ? colors.orange : colors.green;
            return <View key={i} style={[styles.impactSeg, i < n && { backgroundColor: c }]} />;
          })}
        </View>
        {tab.id === 'graphics' ? (
          <>
            <View style={styles.statLine}>
              <Text style={styles.statKey}>GPU estimate</Text>
              <Text style={styles.statVal}>{gpuMs(values).toFixed(1)} ms</Text>
            </View>
            <View style={styles.statLine}>
              <Text style={styles.statKey}>VRAM</Text>
              <Text style={styles.statVal}>{vram(values).toFixed(1)} / 12 GB</Text>
            </View>
          </>
        ) : null}
      </Animated.View>
    </View>
  );
}

const gpuMs = (v) => 2.2 + v.resolution * 1.1 + v.shadows * 0.6 + (v.rt ? 2.4 : 0) + (v.motionBlur ? 0.3 : 0) + (v.fov - 60) * 0.01;
const vram = (v) => 2.6 + v.resolution * 0.8 + v.shadows * 0.35 + (v.rt ? 1.6 : 0);

// --- screen ----------------------------------------------------------------------

function Hint({ glyph, label, color = colors.textDim }) {
  return (
    <View style={styles.hint}>
      <View style={[styles.glyph, { borderColor: color }]}>
        <Text style={[styles.glyphText, { color }]}>{glyph}</Text>
      </View>
      <Text style={styles.hintText}>{label}</Text>
    </View>
  );
}

export default function Settings({ active = true }) {
  const [tabIdx, setTabIdx] = useState(1);
  const [values, setValues] = useState(DEFAULTS);
  const [saved, setSaved] = useState(DEFAULTS);
  const [hot, setHot] = useState('fps');
  const [menu, setMenu] = useState(null); // row key
  const [toast, setToast] = useState(null);
  const rowRefs = useRef({}).current;
  const rowY = useRef({}).current;
  const focusFirst = useRef(false);
  const tab = TABS[tabIdx];

  const rail = useRef(new Animated.Value(tabIdx)).current;
  const slide = useRef(new Animated.Value(1)).current;
  const dir = useRef(1);
  const toastV = useRef(new Animated.Value(0)).current;
  const pendingV = useRef(new Animated.Value(0)).current;

  const changedKeys = useMemo(() => Object.keys(values).filter((k) => values[k] !== saved[k]), [values, saved]);
  const changedTabs = useMemo(() => {
    const s = {};
    TABS.forEach((t) => (s[t.id] = t.rows.some((r) => r.key && values[r.key] !== saved[r.key]) || (t.header && values.preset !== saved.preset)));
    return s;
  }, [values, saved]);
  const pending = changedKeys.length;

  useEffect(() => {
    spring(pendingV, pending ? 1 : 0).start();
  }, [pending > 0]);

  const switchTab = useCallback((i, fromKeys) => {
    setTabIdx((cur) => {
      const n = (i + TABS.length) % TABS.length;
      if (n === cur) return cur;
      dir.current = n > cur ? 1 : -1;
      return n;
    });
    setMenu(null);
    setHot(null);
    focusFirst.current = !!fromKeys;
  }, []);

  useEffect(() => {
    Animated.spring(rail, { toValue: tabIdx, stiffness: 300, damping: 26, useNativeDriver: true }).start();
    slide.setValue(0);
    Animated.timing(slide, { toValue: 1, duration: 320, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    if (focusFirst.current) {
      focusFirst.current = false;
      const first = TABS[tabIdx].rows.find((r) => r.key);
      const id = setTimeout(() => rowRefs[first.key] && rowRefs[first.key].focus(), 0);
      return () => clearTimeout(id);
    }
    return undefined;
  }, [tabIdx]);

  const onChange = useCallback((key, v) => {
    setValues((cur) => {
      if (cur[key] === v) return cur;
      const next = { ...cur, [key]: v };
      if (key === 'preset' && PRESETS[v] !== 'Custom') {
        const pv = PRESET_VALUES[PRESETS[v]];
        next.shadows = SHADOW_OPTIONS.indexOf(pv.shadows);
        next.motionBlur = pv.motionBlur;
        next.rt = pv.rt;
        next.fps = pv.fps;
      } else if (ALL_ROWS[key] && ALL_ROWS[key].quality) next.preset = PRESETS.indexOf('Custom');
      return next;
    });
  }, []);

  const valuesRef = useRef(values);
  valuesRef.current = values;
  const onStep = useCallback((row, d) => {
    const cur = valuesRef.current[row.key];
    let n = cur;
    if (row.type === 'slider') n = +clamp(cur + d * (row.kstep || row.step), row.min, row.max).toFixed(2);
    else if (row.type === 'seg' || row.type === 'drop') n = clamp(cur + d, 0, row.options.length - 1);
    else if (row.type === 'toggle') n = d > 0;
    onChange(row.key, n);
  }, [onChange]);

  const onActivate = useCallback((row) => {
    if (row.type === 'toggle') onChange(row.key, !valuesRef.current[row.key]);
    else if (row.type === 'drop') setMenu((m) => (m === row.key ? null : row.key));
    else if (row.type === 'seg') onChange(row.key, (valuesRef.current[row.key] + 1) % row.options.length);
  }, [onChange]);

  const onHot = useCallback((row) => setHot(row.key), []);
  const registerRef = useCallback((key, el) => (rowRefs[key] = el), []);
  const onRowLayout = useCallback((key, l) => (rowY[key] = l.y), []);

  const apply = useCallback(() => {
    if (!changedKeysRef.current.length) return;
    const n = changedKeysRef.current.length;
    setSaved(valuesRef.current);
    setToast(`${n} change${n === 1 ? '' : 's'} applied`);
    toastV.setValue(0);
    Animated.sequence([
      Animated.spring(toastV, { toValue: 1, stiffness: 260, damping: 20, useNativeDriver: true }),
      Animated.delay(1600),
      Animated.timing(toastV, { toValue: 0, duration: 300, useNativeDriver: true }),
    ]).start();
  }, []);
  const revert = useCallback(() => {
    setValues(savedRef.current);
    setMenu(null);
  }, []);
  const changedKeysRef = useRef(changedKeys);
  changedKeysRef.current = changedKeys;
  const savedRef = useRef(saved);
  savedRef.current = saved;

  const closeMenu = useCallback(() => {
    const k = menu;
    setMenu(null);
    setTimeout(() => rowRefs[k] && rowRefs[k].focus(), 0);
  }, [menu]);

  // Screen-wide shortcuts for events bubbling up from focused children.
  const handled = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  const onKeyDown = (e) => {
    const k = (e.key || '').toLowerCase();
    if (k === 'q') switchTab(tabIdx - 1, true), handled(e);
    else if (k === 'e') switchTab(tabIdx + 1, true), handled(e);
    else if (k === 'f') apply(), handled(e);
    else if (k === 'r') revert(), handled(e);
  };
  const onPad = (e) => {
    if (!e.pressed) return;
    const b = e.buttonIndex;
    if (b === 4) switchTab(tabIdx - 1, true), handled(e);
    else if (b === 5) switchTab(tabIdx + 1, true), handled(e);
    else if (b === 2) apply(), handled(e);
    else if (b === 3) revert(), handled(e);
  };

  const hotRow = hot && ALL_ROWS[hot] && tab.rows.includes(ALL_ROWS[hot]) ? ALL_ROWS[hot] : null;
  const menuRow = menu ? ALL_ROWS[menu] : null;

  return (
    <View style={styles.root} onKeyDown={onKeyDown} onGamepadButton={onPad}>
      <View style={styles.rail}>
        <Text style={styles.railTitle}>Settings</Text>
        <View>
          <Animated.View style={[styles.railHi, { transform: [{ translateY: rail.interpolate({ inputRange: [0, 1], outputRange: [0, 48] }) }] }]} />
          {TABS.map((t, i) => (
            <Pressable key={t.id} focusable={false} onPress={() => switchTab(i)} style={{ cursor: 'pointer' }}>
              {({ hovered }) => (
                <View style={[styles.cat, hovered && i !== tabIdx && { backgroundColor: 'rgba(255,255,255,0.04)' }]}>
                  <View style={[styles.catIcon, i === tabIdx && { backgroundColor: colors.cyan, transform: [{ rotate: '45deg' }] }]} />
                  <Text style={[styles.catText, (i === tabIdx || hovered) && styles.catTextActive]}>{t.label}</Text>
                  {changedTabs[t.id] ? <View style={styles.dot} /> : null}
                </View>
              )}
            </Pressable>
          ))}
        </View>
        <View style={styles.railKeys}>
          <Hint glyph="LB" label="Q" />
          <Text style={styles.railKeysText}>Switch tab</Text>
          <Hint glyph="RB" label="E" />
        </View>
        <View style={{ flex: 1 }} />
        <Text style={styles.railFoot} numberOfLines={1}>Profile: {values.callsign || '—'}</Text>
      </View>

      <Animated.View
        style={[
          styles.content,
          { opacity: slide, transform: [{ translateX: slide.interpolate({ inputRange: [0, 1], outputRange: [dir.current * 36, 0] }) }] },
        ]}
      >
        <View style={styles.contentHeader}>
          <Text style={styles.h1}>{tab.label}</Text>
          {tab.header === 'preset' ? <Segmented small options={PRESETS} value={values.preset} onSelect={(i) => onChange('preset', i)} /> : null}
        </View>
        <View>
          {tab.rows.map((r, i) =>
            r.section ? (
              <Text key={'s' + i} style={styles.section}>{r.section}</Text>
            ) : (
              <Row
                key={r.key}
                autoFocus={active && i === 1}
                row={r}
                value={values[r.key]}
                changed={values[r.key] !== saved[r.key]}
                open={menu === r.key}
                onChange={onChange}
                onStep={onStep}
                onActivate={onActivate}
                onHot={onHot}
                registerRef={registerRef}
                onRowLayout={onRowLayout}
              />
            ),
          )}
          {menuRow ? (
            <Menu
              key={menu}
              row={menuRow}
              value={values[menu]}
              y={rowY[menu] || 0}
              onClose={closeMenu}
              onPick={(i) => {
                onChange(menu, i);
                closeMenu();
              }}
            />
          ) : null}
        </View>
      </Animated.View>

      <Help tab={tab} row={hotRow} values={values} active={active} />

      <View style={styles.footer}>
        <Animated.View style={[styles.pending, { opacity: pendingV, transform: [{ translateX: pendingV.interpolate({ inputRange: [0, 1], outputRange: [16, 0] }) }] }]}>
          <View style={styles.pendingDot} />
          <Text style={styles.pendingText}>
            {pending} unsaved change{pending === 1 ? '' : 's'}
          </Text>
        </Animated.View>
        <Pressable onPress={revert} disabled={!pending} style={{ cursor: 'pointer' }}>
          {({ hovered, focused }) => (
            <View style={[styles.btn, styles.btnGhost, (hovered || focused) && styles.btnGhostHot, !pending && { opacity: 0.4 }]}>
              <View style={[styles.glyph, styles.glyphY]}>
                <Text style={[styles.glyphText, { color: colors.gold }]}>Y</Text>
              </View>
              <Text style={styles.btnGhostText}>Revert</Text>
            </View>
          )}
        </Pressable>
        <Pressable onPress={apply} disabled={!pending} style={{ cursor: 'pointer' }}>
          {({ hovered, focused, pressed }) => (
            <View style={[styles.btn, styles.btnPrimary, (hovered || focused) && styles.btnPrimaryHot, !pending && { opacity: 0.45 }, pressed && { transform: [{ scale: 0.96 }] }]}>
              <View style={[styles.glyph, { borderColor: '#2a1a03' }]}>
                <Text style={[styles.glyphText, { color: '#2a1a03' }]}>X</Text>
              </View>
              <Text style={styles.btnPrimaryText}>Apply Changes</Text>
            </View>
          )}
        </Pressable>
      </View>

      {toast ? (
        <Animated.View
          pointerEvents="none"
          style={[styles.toast, { opacity: toastV, transform: [{ translateY: toastV.interpolate({ inputRange: [0, 1], outputRange: [20, 0] }) }] }]}
        >
          <View style={styles.toastCheck} />
          <Text style={styles.toastText}>{toast}</Text>
        </Animated.View>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    width: 1280,
    height: 720,
    overflow: 'hidden',
    flexDirection: 'row',
    backgroundColor: '#080c18',
    experimental_backgroundImage: 'linear-gradient(135deg, #0c1428 0%, #070a14 60%)',
  },

  rail: { width: 240, paddingTop: 36, paddingBottom: 28, paddingHorizontal: 18, backgroundColor: 'rgba(0,0,0,0.25)', borderRightWidth: 1, borderRightColor: colors.line },
  railTitle: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 3, textTransform: 'uppercase', color: colors.textMute, marginLeft: 14, marginBottom: 18 },
  railHi: {
    position: 'absolute',
    left: 0,
    right: 0,
    top: 0,
    height: 44,
    borderRadius: 10,
    backgroundColor: 'rgba(79,209,255,0.10)',
    borderWidth: 1,
    borderColor: 'rgba(79,209,255,0.35)',
    boxShadow: 'inset 3px 0 0 #4fd1ff',
  },
  cat: { flexDirection: 'row', alignItems: 'center', gap: 12, height: 44, paddingHorizontal: 14, borderRadius: 10, marginBottom: 4 },
  catIcon: { width: 10, height: 10, borderRadius: 2, backgroundColor: colors.textMute },
  catText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.textDim },
  catTextActive: { color: '#fff' },
  dot: { width: 7, height: 7, borderRadius: 4, backgroundColor: colors.orange, boxShadow: '0 0 8px rgba(255,154,60,0.8)' },
  railKeys: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 18, marginLeft: 14 },
  railKeysText: { fontFamily: font, fontSize: 11, color: colors.textMute },
  railFoot: { fontFamily: font, fontSize: 12, color: colors.textMute, marginLeft: 14 },

  content: { width: 640, paddingTop: 30, paddingHorizontal: 32 },
  contentHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 4, height: 40 },
  h1: { fontFamily: font, fontWeight: '700', fontSize: 30, color: colors.text },
  section: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, textTransform: 'uppercase', color: colors.cyan, marginTop: 16, marginBottom: 4, marginLeft: 14 },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    height: ROW_H,
    paddingHorizontal: 14,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: 'transparent',
    borderBottomColor: 'rgba(148,170,255,0.07)',
  },
  rowHover: { backgroundColor: 'rgba(255,255,255,0.03)' },
  rowFocused: {
    backgroundColor: 'rgba(242,195,91,0.08)',
    borderColor: 'rgba(242,195,91,0.5)',
    borderBottomColor: 'rgba(242,195,91,0.5)',
    boxShadow: '0 0 20px rgba(242,195,91,0.15)',
  },
  changed: { position: 'absolute', left: 3, top: 19, width: 6, height: 6, borderRadius: 3, backgroundColor: colors.gold, boxShadow: '0 0 6px rgba(242,195,91,0.9)' },
  rowLabel: { fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.text },
  rowSub: { fontFamily: font, fontSize: 12, color: colors.textMute, marginTop: 1 },
  control: { width: 290, alignItems: 'flex-end' },
  arrows: { position: 'absolute', right: -26, top: 0, bottom: 0, justifyContent: 'center' },
  arrowText: { fontFamily: font, fontSize: 9, color: colors.gold, letterSpacing: -1 },

  toggle: { width: 46, height: 26, borderRadius: 13, padding: 3, backgroundColor: 'rgba(255,255,255,0.10)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)' },
  toggleOn: { backgroundColor: colors.cyanDeep, borderColor: colors.cyan, boxShadow: '0 0 12px rgba(79,209,255,0.45)' },
  knob: { width: 18, height: 18, borderRadius: 9, backgroundColor: '#8a93ad', boxShadow: '0 1px 3px rgba(0,0,0,0.5)' },
  knobOn: { backgroundColor: '#fff' },

  sliderWrap: { flexDirection: 'row', alignItems: 'center', gap: 10, width: 290 },
  sliderHit: { width: TRACK_W + 16, height: 32, paddingHorizontal: 8, justifyContent: 'center', cursor: 'pointer' },
  sliderTrack: { width: TRACK_W, height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.10)', overflow: 'hidden' },
  sliderFill: { position: 'absolute', left: 0, top: 0, bottom: 0, width: TRACK_W, borderRadius: 3 },
  thumb: {
    position: 'absolute',
    left: -1,
    top: 7,
    width: 18,
    height: 18,
    borderRadius: 9,
    backgroundColor: '#fff',
    borderWidth: 3,
    boxShadow: '0 2px 6px rgba(0,0,0,0.5)',
  },
  sliderValue: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text, textAlign: 'right' },

  segmented: { flexDirection: 'row', padding: 3, borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.35)', borderWidth: 1, borderColor: colors.line },
  segment: { paddingHorizontal: 11, paddingVertical: 6, borderRadius: 6, cursor: 'pointer' },
  segmentActive: {
    position: 'absolute',
    left: 0,
    top: 3,
    bottom: 3,
    borderRadius: 6,
    backgroundColor: '#24314f',
    boxShadow: '0 1px 4px rgba(0,0,0,0.5), inset 0 1px 0 rgba(255,255,255,0.08)',
  },
  segmentText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  segmentTextActive: { color: '#fff' },

  dropdown: {
    width: 220,
    height: 34,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 12,
    borderRadius: 7,
    backgroundColor: 'rgba(0,0,0,0.35)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
  },
  input: {
    width: 220,
    height: 34,
    paddingHorizontal: 12,
    borderRadius: 7,
    backgroundColor: 'rgba(0,0,0,0.35)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
    fontFamily: font,
    fontWeight: '500',
    fontSize: 13,
    color: colors.text,
  },
  bind: { minWidth: 120, height: 34, paddingHorizontal: 14, borderRadius: 7, alignItems: 'center', justifyContent: 'center', borderWidth: 1, borderColor: colors.lineStrong, backgroundColor: 'rgba(255,255,255,0.05)' },
  bindListening: { borderColor: colors.gold, backgroundColor: 'rgba(242,195,91,0.12)', boxShadow: '0 0 14px rgba(242,195,91,0.35)' },
  bindText: { fontFamily: 'JetBrains Mono', fontWeight: '700', fontSize: 13, color: colors.text },
  dropdownOpen: { borderColor: colors.cyan, borderBottomLeftRadius: 0, borderBottomRightRadius: 0, backgroundColor: '#111a30' },
  dropdownText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.text },
  caret: { width: 7, height: 7, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.textDim, transform: [{ rotate: '45deg' }], marginTop: -3 },
  backdrop: { position: 'absolute', left: -400, right: -1000, top: -400, bottom: -400, zIndex: 9 },
  menu: {
    position: 'absolute',
    right: 15,
    width: 220,
    paddingVertical: 4,
    backgroundColor: '#111a30',
    borderWidth: 1,
    borderTopWidth: 0,
    borderColor: colors.cyan,
    borderBottomLeftRadius: 8,
    borderBottomRightRadius: 8,
    boxShadow: '0 18px 30px rgba(0,0,0,0.7)',
    zIndex: 10,
  },
  menuItem: { height: 32, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: 12 },
  menuItemActive: { backgroundColor: 'rgba(79,209,255,0.16)' },
  menuText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
  checkMark: { width: 6, height: 11, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.cyan, transform: [{ rotate: '45deg' }], marginTop: -3 },

  help: {
    position: 'absolute',
    right: 32,
    top: 92,
    width: 320,
    borderRadius: 14,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 20px 40px rgba(0,0,0,0.5)',
  },
  previewBox: { width: '100%', height: 150, overflow: 'hidden', backgroundColor: '#000' },
  preview: { width: '100%', height: 150 },
  previewTag: { position: 'absolute', left: 10, top: 10, paddingHorizontal: 7, paddingVertical: 3, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.6)' },
  previewTagText: { fontFamily: font, fontWeight: '700', fontSize: 9, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textDim },
  meter: { position: 'absolute', left: 20, right: 20, bottom: 16, height: 60, flexDirection: 'row', alignItems: 'flex-end', gap: 5 },
  meterBar: { flex: 1, height: 60, borderRadius: 2, backgroundColor: colors.gold, transformOrigin: 'bottom', boxShadow: '0 0 8px rgba(242,195,91,0.6)' },
  miniHud: { position: 'absolute', left: 14, bottom: 14, gap: 4 },
  miniHp: { width: 140, height: 8, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.6)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.3)', overflow: 'hidden' },
  miniHpFill: { height: '100%', backgroundColor: colors.red },
  miniHudText: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#fff', letterSpacing: 1 },
  helpBody: { padding: 18 },
  helpTitle: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.text },
  helpText: { fontFamily: font, fontSize: 13, lineHeight: 20, color: colors.textDim, marginTop: 8, minHeight: 60 },
  impactLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute, marginTop: 14 },
  impact: { flexDirection: 'row', gap: 4, marginTop: 8, marginBottom: 8 },
  impactSeg: { flex: 1, height: 6, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.10)' },
  statLine: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 4 },
  statKey: { fontFamily: font, fontSize: 13, color: colors.textDim },
  statVal: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  footer: { position: 'absolute', right: 32, bottom: 28, flexDirection: 'row', alignItems: 'center', gap: 12 },
  pending: { flexDirection: 'row', alignItems: 'center', gap: 8, marginRight: 6 },
  pendingDot: { width: 7, height: 7, borderRadius: 4, backgroundColor: colors.gold, boxShadow: '0 0 6px rgba(242,195,91,0.9)' },
  pendingText: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.goldLight },
  btn: { height: 44, flexDirection: 'row', gap: 10, paddingHorizontal: 18, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  btnGhost: { borderWidth: 1, borderColor: colors.lineStrong },
  btnGhostHot: { borderColor: colors.cyan, backgroundColor: 'rgba(79,209,255,0.08)' },
  btnGhostText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
  btnPrimary: {
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c9902d 100%)',
    boxShadow: '0 6px 18px rgba(242,195,91,0.35), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  btnPrimaryHot: { boxShadow: '0 0 0 2px #080c18, 0 0 0 4px rgba(242,195,91,0.8), 0 8px 24px rgba(242,195,91,0.45)' },
  btnPrimaryText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#2a1a03', letterSpacing: 0.5 },
  glyph: { minWidth: 20, height: 20, paddingHorizontal: 4, borderRadius: 10, borderWidth: 1.5, alignItems: 'center', justifyContent: 'center' },
  glyphY: { borderColor: colors.gold },
  glyphText: { fontFamily: font, fontWeight: '700', fontSize: 10 },
  hint: { flexDirection: 'row', alignItems: 'center', gap: 4 },
  hintText: { fontFamily: font, fontWeight: '600', fontSize: 11, color: colors.textMute },

  toast: {
    position: 'absolute',
    left: 500,
    bottom: 34,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 16,
    height: 40,
    borderRadius: 20,
    backgroundColor: 'rgba(10,30,20,0.95)',
    borderWidth: 1,
    borderColor: 'rgba(74,222,128,0.6)',
    boxShadow: '0 10px 30px rgba(0,0,0,0.5), 0 0 20px rgba(74,222,128,0.25)',
  },
  toastCheck: { width: 7, height: 12, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.green, transform: [{ rotate: '45deg' }], marginTop: -3 },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
});
