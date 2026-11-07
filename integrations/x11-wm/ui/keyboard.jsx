// The on-screen keyboard. Keys go to the host as wm:typeKey: into the shell's
// own text field while it has the keyboard (the launcher's search), otherwise
// into the focused X client (XTest). Shift and Ctrl are one-shot. The letter
// layer is a tablet's (letters, shift, return, space); the ?123 layer has the
// numbers, symbols and the keys a terminal needs (esc, tab, ctrl, arrows).
import React, { memo, useState } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import { useAnchor } from './anchors.js';
import { Icon } from './icons.jsx';
import { font, radius, useTheme } from './theme.js';
import { setSetting, wm } from './wm.js';

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
const keyDown = () => {
  typingInto = TextInput.State.currentlyFocusedInput() || typingInto;
};
const refocus = () => {
  if (typingInto && TextInput.State.currentlyFocusedInput() !== typingInto) TextInput.State.focusTextInput(typingInto);
};

const Key = memo(function Key({ k, label, icon, flex, alt, on, onPress }) {
  const t = useTheme();
  const anchor = useAnchor(`osk.${k}`);
  return (
    <Pressable
      ref={anchor.ref}
      onLayout={anchor.onLayout}
      focusable={false}
      onPressIn={keyDown}
      onPress={() => {
        refocus();
        onPress();
      }}
      style={[styles.key, { flex, backgroundColor: on ? t.accent : alt ? t.keyAlt : t.key }]}
      pressedStyle={{ backgroundColor: t.accent, transform: [{ scale: 0.94 }] }}
    >
      {icon ? (
        <Icon name={icon} size={24} color={on ? t.accentText : t.text} />
      ) : (
        <Text style={[styles.keyText, { color: on ? t.accentText : t.text, fontSize: label.length > 2 ? 16 : 22 }]}>{label}</Text>
      )}
    </Pressable>
  );
});

export const Keyboard = memo(function Keyboard({ screen, onMeasure, onHide }) {
  const t = useTheme();
  const [layer, setLayer] = useState('abc');
  const [shift, setShift] = useState(false);
  const [ctrl, setCtrl] = useState(false);
  const rows = layer === 'abc' ? LETTERS : SYMBOLS;
  const press = (k) => {
    if (k === 'Shift') return setShift(!shift);
    if (k === 'Ctrl') return setCtrl(!ctrl);
    if (k === '?123') return setLayer('sym');
    if (k === 'abc') return setLayer('abc');
    if (k === 'Hide') {
      if (onHide) onHide();
      return setSetting('osk', false);
    }
    const mods = (ctrl ? 2 : 0);
    if (k.length > 1 && !ICONS[k] && !LABELS[k] && k !== 'Escape' && k !== 'Tab') {
      // Multi-character helpers on the symbol layer ("&&", "~/"): type each.
      for (const ch of k) wm.typeKey(ch, 0);
    } else {
      const key = k.length === 1 && shift ? k.toUpperCase() : k;
      wm.typeKey(key, mods);
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
        <View key={r} style={styles.row}>
          {row.map((k) => {
            const label = LABELS[k] != null ? LABELS[k] : k.length === 1 && shift ? k.toUpperCase() : k;
            return (
              <Key
                key={k}
                k={k}
                label={label}
                icon={ICONS[k]}
                flex={WIDE[k] || 1}
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
  board: { position: 'absolute', left: 0, right: 0, bottom: 0, paddingHorizontal: 10, paddingTop: 10, paddingBottom: 12, borderTopWidth: 1, borderTopLeftRadius: 24, borderTopRightRadius: 24 },
  row: { flexDirection: 'row', height: 52, marginBottom: 8 },
  key: { marginHorizontal: 4, borderRadius: radius.s, alignItems: 'center', justifyContent: 'center' },
  keyText: { fontFamily: font, fontWeight: '500' },
});
