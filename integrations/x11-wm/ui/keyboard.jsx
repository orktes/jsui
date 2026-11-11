// The on-screen keyboard. Keys go to the host as wm:typeKey: into the shell's
// own text field while it has the keyboard (the launcher's search), otherwise
// into the focused X client (XTest). Shift and Ctrl are one-shot. The letter
// layer is a tablet's (letters, shift, return, space); the ?123 layer has the
// numbers, symbols and the keys a terminal needs (esc, tab, ctrl, arrows).
import React, { memo, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View, useHostState } from 'react-native';
import { useAnchor } from './anchors.js';
import { Icon } from './icons.jsx';
import { font, radius, useTheme } from './theme.js';
import { send, setSetting, wm } from './wm.js';

const LETTERS = [
  ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 'Backspace'],
  ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', "'", 'Enter'],
  ['Shift', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-'],
  ['?123', ' ', 'Hide'],
];
const SYMBOLS = [
  ['Escape', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'Backspace'],
  ['Tab', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', 'Enter'],
  ['Ctrl', '~', '`', '|', '\\', '=', '+', '[', ']', '{', '}', '_'],
  ['<', '>', '"', ':', ';', '/', '?', '&&', '~/', '..', 'ArrowUp'],
  ['abc', ' ', 'ArrowLeft', 'ArrowDown', 'ArrowRight', 'Hide'],
];

const WIDE = { Backspace: 1.6, Tab: 1.3, Enter: 1.6, Shift: 1.6, Ctrl: 1.3, Escape: 1.2, ' ': 6, '?123': 1.6, abc: 1.6, Hide: 1.6 };
const ICONS = { Backspace: 'backspace', Enter: 'enter', Shift: 'shift', ArrowUp: 'chevronUp', ArrowDown: 'chevronDown', ArrowLeft: 'left', ArrowRight: 'right', Hide: 'keyboard' };
const LABELS = { Escape: 'esc', Tab: 'tab', Ctrl: 'ctrl', ' ': '' };

// A press anywhere moves focus (a press on a key would blur the shell's text
// field the keys are for): the field focused when a key goes down gets the
// focus back before the key is typed.
let typingInto = null;
// A light tap of the vibrator per key press (settings.keyHaptics, on by
// default), as a phone's keyboard gives; jsui-wl sends it to hfd in process.
let keyHaptics = true;
const keyDown = () => {
  typingInto = TextInput.State.currentlyFocusedInput() || typingInto;
  if (keyHaptics) wm.haptic(KEY_TAP_MS);
};
const KEY_TAP_MS = 10;
const refocus = () => {
  if (typingInto && TextInput.State.currentlyFocusedInput() !== typingInto) TextInput.State.focusTextInput(typingInto);
};

// Rollover, as a phone's keyboard types: a key commits when its finger
// lifts, or as soon as the next finger lands. Fast two-thumb typing overlaps
// touches: without it a second tap on a key still held by the first finger
// was lost (one press per Pressable), and overlapping keys typed in the
// order their fingers lifted.
let pending = null; // the press of the key whose finger is still down
const commit = () => {
  const p = pending;
  pending = null;
  if (p) p();
};
// Typing trace (jsui-wl's wm.keyTrace, see its checkKeyTrace): the keys'
// press events go to ~/caps/keytrace.log next to the host's touches.
let keyTrace = false;
const trace = (s) => keyTrace && send('wm:trace', { s });
const where = (e) => {
  const n = e && (e.nativeEvent || e);
  return n ? ` #${n.pointerId} ${Math.round(n.x)},${Math.round(n.y)}` : '';
};

const Key = memo(function Key({ k, label, icon, flex, alt, on, last, onPress }) {
  const t = useTheme();
  const anchor = useAnchor(`osk.${k}`);
  const pressRef = useRef(onPress);
  pressRef.current = onPress;
  const mine = useRef(null); // this key's latest finger's press
  const armed = useRef(null); // the one the Pressable's own press began with
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      focusable={false}
      // Every finger that lands (the Pressable reports a second finger on a
      // key it already holds only here).
      onPointerDown={(e) => {
        trace(`down ${k}${where(e)}${pending ? ' (rolls over the held key)' : ''}`);
        keyDown();
        commit();
        const fire = () => {
          trace(`commit ${k}`);
          refocus();
          pressRef.current();
        };
        pending = fire;
        mine.current = fire;
      }}
      onPressIn={() => {
        armed.current = mine.current;
      }}
      onPress={(e) => {
        trace(`press ${k}${where(e)}${pending && pending === mine.current ? '' : ' (already committed)'}`);
        if (pending && pending === mine.current) commit();
      }}
      onHoverOut={(e) => trace(`leave ${k}${where(e)}`)}
      // Cancelled (the finger slid well off) before anything committed it:
      // it types nothing. Checked after the dispatch: a release held past
      // minPressDuration sends onPressOut right before its onPress (and the
      // press's start can be stamped with an old frame time after a pause),
      // so cancelling here at once dropped most taps that followed a pause.
      // (A quick second tap flushes the first press's delayed onPressOut
      // after its own onPointerDown, hence `armed`, not `mine`.)
      onPressOut={() => {
        const was = armed.current;
        Promise.resolve().then(() => {
          if (pending && pending === was) {
            trace(`cancel ${k}`);
            pending = null;
          }
        });
      }}
      // The key takes touches up to the middle of the gaps around it (its
      // padding): a tap between two rows or keys used to hit only the board.
      group
      style={[styles.key, { flex }, last && styles.keyLast]}
    >
      <View
        style={[styles.keyFace, { backgroundColor: on ? t.accent : alt ? t.keyAlt : t.key }]}
        groupPressedStyle={{ backgroundColor: t.accent, transform: [{ scale: 0.94 }] }}
      >
        {icon ? (
          <Icon name={icon} size={24} color={on ? t.accentText : t.text} />
        ) : (
          <Text style={[styles.keyText, { color: on ? t.accentText : t.text, fontSize: label.length > 2 ? 16 : 22 }]}>{label}</Text>
        )}
      </View>
    </Pressable>
  );
});

export const Keyboard = memo(function Keyboard({ screen, onMeasure, onHide, haptics = true }) {
  keyHaptics = haptics;
  keyTrace = useHostState('wm.keyTrace', false) === true;
  const t = useTheme();
  const [layer, setLayer] = useState('abc');
  const [shift, setShiftState] = useState(false);
  const [ctrl, setCtrlState] = useState(false);
  // Read through refs: a rollover can commit the next key before the
  // render that a Shift / Ctrl tap asked for.
  const mods = useRef({ shift: false, ctrl: false }).current;
  const setShift = (v) => setShiftState((mods.shift = v));
  const setCtrl = (v) => setCtrlState((mods.ctrl = v));
  const rows = layer === 'abc' ? LETTERS : SYMBOLS;
  const press = (k) => {
    const { shift, ctrl } = mods;
    if (k === 'Shift') return setShift(!shift);
    if (k === 'Ctrl') return setCtrl(!ctrl);
    if (k === '?123') return setLayer('sym');
    if (k === 'abc') return setLayer('abc');
    if (k === 'Hide') {
      if (onHide) onHide();
      return setSetting('osk', false);
    }
    const modBits = ctrl ? 2 : 0;
    if (k.length > 1 && !ICONS[k] && !LABELS[k] && k !== 'Escape' && k !== 'Tab') {
      // Multi-character helpers on the symbol layer ("&&", "~/"): type each.
      for (const ch of k) wm.typeKey(ch, 0);
    } else {
      const key = k.length === 1 && shift ? k.toUpperCase() : k;
      wm.typeKey(key, modBits);
    }
    if (shift) setShift(false);
    if (ctrl) setCtrl(false);
  };
  return (
    <View
      onLayout={(e) => onMeasure(e.nativeEvent.layout)}
      onStartShouldSetResponder={() => true}
      style={[styles.board, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px -10px 40px ${t.shadow}` }]}
    >
      {rows.map((row, r) => (
        <View key={r} style={[styles.row, r === rows.length - 1 && styles.rowLast]}>
          {row.map((k) => {
            const label = LABELS[k] != null ? LABELS[k] : k.length === 1 && shift ? k.toUpperCase() : k;
            return (
              <Key
                key={k}
                k={k}
                label={label}
                icon={ICONS[k]}
                flex={WIDE[k] || 1}
                last={r === rows.length - 1}
                alt={k.length > 1 && k !== ' ' && !/^[^a-z]{2}$/.test(k)}
                on={(k === 'Shift' && shift) || (k === 'Ctrl' && ctrl)}
                onPress={() => press(k)}
              />
            );
          })}
        </View>
      ))}
    </View>
  );
});

const styles = StyleSheet.create({
  // Keys are 52 px tall with 8 px gaps; each key's padding is half a gap.
  // The bottom row's keys reach down to the screen edge (thumbs hit the
  // space bar low: taps in the board's bottom padding typed nothing).
  board: { position: 'absolute', left: 0, right: 0, bottom: 0, paddingHorizontal: 10, paddingTop: 6, borderTopWidth: 1, borderTopLeftRadius: 24, borderTopRightRadius: 24 },
  row: { flexDirection: 'row', height: 60 },
  rowLast: { height: 76 },
  key: { paddingHorizontal: 4, paddingVertical: 4 },
  keyLast: { paddingBottom: 20 },
  keyFace: { flex: 1, borderRadius: radius.s, alignItems: 'center', justifyContent: 'center' },
  keyText: { fontFamily: font, fontWeight: '500' },
});
