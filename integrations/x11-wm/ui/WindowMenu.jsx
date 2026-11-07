// Tablet mode: the menu of a full-screen or split app's header (the other
// modes, Minimize and Close), a popout under its "more" button. It is an <Overlay> rendered inside the button's wrapper,
// so the runtime places it against the button every frame (and keeps it on
// screen) with no measuring; the shell catches taps outside it (shell.jsx).
import React from 'react';
import { Overlay, Pressable, StyleSheet, Text, View } from 'react-native';
import { useAnchor } from './anchors.js';
import { Icon } from './icons.jsx';
import { font, radius, useTheme } from './theme.js';
import { wm } from './wm.js';

export function WindowMenu({ win, onClose }) {
  const rows = [
    { icon: 'maximize', label: 'Full screen', fn: () => wm.maximize(win.id, true), on: win.maximized },
    { icon: 'snapLeft', label: 'Split left', fn: () => wm.snap(win.id, 'left'), on: win.snap === 'left' },
    { icon: 'snapRight', label: 'Split right', fn: () => wm.snap(win.id, 'right'), on: win.snap === 'right' },
    { icon: 'restore', label: 'Float', fn: () => (win.snap ? wm.snap(win.id, null) : wm.maximize(win.id, false)) },
    { icon: 'minimize', label: 'Minimize', fn: () => wm.minimize(win.id) },
    { icon: 'close', label: 'Close', fn: () => wm.close(win.id) },
  ].filter((r) => !r.on); // the mode it is in isn't a choice
  return <ActionMenu rows={rows} onClose={onClose} />;
}

// A popout of actions ({ icon, label, fn }) under the element it is rendered
// in; the shell catches taps outside it.
export function ActionMenu({ rows, onClose, placement = 'bottom-end' }) {
  const t = useTheme();
  return (
    <Overlay placement={placement} offset={8} collisionPadding={12}>
      <View style={[styles.menu, { backgroundColor: t.panel, borderColor: t.panelLine, boxShadow: `0px 16px 48px ${t.shadow}` }]}>
        {rows.map((r) => (
          <MenuRow key={r.label} r={r} t={t} onClose={onClose} />
        ))}
      </View>
    </Overlay>
  );
}

function MenuRow({ r, t, onClose }) {
  const anchor = useAnchor(`menu.${r.label.toLowerCase().replace(/\s+/g, '-')}`);
  return (
    <Pressable ref={anchor.ref} onLayout={anchor.onLayout} onPress={() => { r.fn(); onClose(); }}
      style={styles.menuRow} pressedStyle={{ backgroundColor: t.panelLine }}>
      <Icon name={r.icon} size={20} color={r.on ? t.accent : t.text} />
      <Text style={[styles.menuText, { color: r.on ? t.accent : t.text }]}>{r.label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  menu: { width: 280, borderRadius: radius.l, borderWidth: 1, paddingVertical: 8 },
  menuRow: { height: 52, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 18 },
  menuText: { fontFamily: font, fontSize: 16, marginLeft: 14, flex: 1 },
});
