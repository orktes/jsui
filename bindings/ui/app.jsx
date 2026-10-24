// Quest log: the UI of the language-binding examples (bindings/rust,
// bindings/go). The host program owns the data and the rules; this file only
// shows them and asks for changes, through the same C API in every language:
//   * Host -> UI shared state (jsui_set_state):
//       `host`   { language, renderer, gpu, os }
//       `quests` { items: [{ id, title, done }] }
//       `player` { level, xp, next }   (the host computes XP and levels)
//       `clock`  "14:05:09"             (formatted by the host once a second)
//   * UI -> host calls with an answer (Host.call -> on_call):
//       quests.add { title } -> { id }, quests.toggle { id } -> { done },
//       quests.remove { id } -> {}
//     The host updates `quests` / `player` itself; the UI never edits them.
//   * Host -> UI messages (jsui_post_message): `toast` { text, tone }.
//   * UI -> host messages (Host.postMessage): `ui:anchors` (where the Add
//     button, the first quest's checkbox and an empty point are, for the
//     hosts' --selftest) and `app:quit`.
import React, { useEffect, useRef, useState } from 'react';
import {
  Animated,
  AppRegistry,
  Host,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
  useHostMessage,
  useHostState,
} from 'react-native';
import { colors, font, panel } from '../../examples/theme.js';

const SUGGESTIONS = ['Slay the ember drake', 'Map the sunken library', 'Forge a starsteel blade', 'Escort the caravan'];

function Header({ host, clock }) {
  return (
    <View style={styles.header}>
      <View style={{ flex: 1 }}>
        <Text style={styles.eyebrow}>{`JSUI · ${host.language || 'host'}`}</Text>
        <Text style={styles.title}>Quest Log</Text>
      </View>
      <View style={styles.headerRight}>
        <Text style={styles.clock}>{clock || '--:--:--'}</Text>
        <Text style={styles.meta}>{host.renderer || '…'}</Text>
        {host.gpu ? <Text style={styles.meta}>{host.gpu}</Text> : null}
      </View>
    </View>
  );
}

function PlayerBar({ player }) {
  const frac = player.next ? Math.min(1, (player.xp || 0) / player.next) : 0;
  const width = useRef(new Animated.Value(frac)).current;
  useEffect(() => {
    Animated.spring(width, { toValue: frac, stiffness: 180, damping: 24, useNativeDriver: false }).start();
  }, [frac]);
  return (
    <View style={styles.player}>
      <View style={styles.levelBadge}>
        <Text style={styles.levelText}>{String(player.level ?? 1)}</Text>
      </View>
      <View style={{ flex: 1 }}>
        <View style={styles.xpRow}>
          <Text style={styles.label}>Experience</Text>
          <Text style={styles.xpText}>{`${player.xp ?? 0} / ${player.next ?? 0} XP`}</Text>
        </View>
        <View style={styles.xpTrack}>
          <Animated.View
            style={[styles.xpFill, { width: width.interpolate({ inputRange: [0, 1], outputRange: ['0%', '100%'] }) }]}
          />
        </View>
      </View>
    </View>
  );
}

function AddRow({ onAnchor }) {
  const [title, setTitle] = useState(SUGGESTIONS[0]);
  const [busy, setBusy] = useState(false);
  const count = useRef(1);
  const add = async () => {
    const t = title.trim();
    if (!t || busy) return;
    setBusy(true);
    try {
      await Host.call('quests.add', { title: t });
      setTitle(SUGGESTIONS[count.current++ % SUGGESTIONS.length]);
    } finally {
      setBusy(false);
    }
  };
  return (
    <View style={styles.addRow}>
      <TextInput
        style={styles.input}
        value={title}
        onChangeText={setTitle}
        onSubmitEditing={add}
        placeholder="New quest…"
        placeholderTextColor={colors.textMute}
      />
      <Pressable
        ref={(r) => onAnchor('add', r)}
        onPress={add}
        style={({ pressed, hovered, focused }) => [
          styles.addButton,
          (hovered || focused) && styles.addButtonHot,
          pressed && { transform: [{ scale: 0.95 }] },
        ]}
      >
        <Text style={styles.addLabel}>{busy ? '…' : 'Add'}</Text>
      </Pressable>
    </View>
  );
}

function QuestRow({ quest, first, onAnchor }) {
  return (
    <View style={[styles.row, quest.done && styles.rowDone]}>
      <Pressable
        ref={first ? (r) => onAnchor('toggle', r) : undefined}
        onPress={() => Host.call('quests.toggle', { id: quest.id })}
        style={({ hovered }) => [styles.check, quest.done && styles.checkOn, hovered && { borderColor: colors.gold }]}
      >
        {quest.done ? <Text style={styles.checkMark}>✓</Text> : null}
      </Pressable>
      <Text style={[styles.rowTitle, quest.done && styles.rowTitleDone]} numberOfLines={1}>
        {quest.title}
      </Text>
      <Pressable
        onPress={() => Host.call('quests.remove', { id: quest.id })}
        style={({ hovered }) => [styles.remove, hovered && { backgroundColor: 'rgba(255, 77, 106, 0.18)' }]}
      >
        <Text style={styles.removeText}>×</Text>
      </Pressable>
    </View>
  );
}

function Toast() {
  const [toast, setToast] = useState(null);
  const t = useRef(new Animated.Value(0)).current;
  useHostMessage('toast', (data) => {
    setToast(data);
    t.setValue(0);
    Animated.sequence([
      Animated.spring(t, { toValue: 1, stiffness: 260, damping: 20, useNativeDriver: true }),
      Animated.delay(1800),
      Animated.timing(t, { toValue: 0, duration: 260, useNativeDriver: true }),
    ]).start();
  });
  if (!toast) return null;
  const accent = toast.tone === 'level' ? colors.gold : colors.green;
  return (
    <View pointerEvents="none" style={styles.toastWrap}>
      <Animated.View
        style={[
          styles.toast,
          { borderColor: accent },
          { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [16, 0] }) }] },
        ]}
      >
        <View style={[styles.toastDot, { backgroundColor: accent, boxShadow: `0 0 10px ${accent}` }]} />
        <Text style={styles.toastText}>{toast.text}</Text>
      </Animated.View>
    </View>
  );
}

function App() {
  const host = useHostState('host', {});
  const quests = useHostState('quests', { items: [] });
  const player = useHostState('player', {});
  const clock = useHostState('clock', '');
  const items = quests.items || [];

  // Where the self-test should click, reported whenever the layout settles.
  const refs = useRef({});
  const onAnchor = (name, r) => {
    refs.current[name] = r;
  };
  useEffect(() => {
    const out = { empty: { x: 12, y: 12, width: 1, height: 1 }, count: items.length, done: items.filter((q) => q.done).length };
    const names = Object.keys(refs.current).filter((k) => refs.current[k]);
    let left = names.length;
    if (!left) return Host.postMessage('ui:anchors', out);
    for (const k of names)
      refs.current[k].measureInWindow((x, y, width, height) => {
        out[k] = { x, y, width, height };
        if (--left === 0) Host.postMessage('ui:anchors', out);
      });
  });

  const open = items.filter((q) => !q.done).length;
  return (
    // The root takes no input, so clicks on the empty background are not
    // consumed and reach the host (it counts them in the footer).
    <View style={styles.root}>
      <View style={styles.card}>
        <Header host={host} clock={clock} />
        <PlayerBar player={player} />
        <AddRow onAnchor={onAnchor} />
        <View style={styles.listHead}>
          <Text style={styles.label}>{`${open} open · ${items.length - open} done`}</Text>
        </View>
        <ScrollView style={styles.list} contentContainerStyle={{ gap: 8 }}>
          {items.map((q, i) => (
            <QuestRow key={q.id} quest={q} first={i === 0} onAnchor={onAnchor} />
          ))}
          {items.length === 0 ? <Text style={styles.empty}>No quests. Add one above.</Text> : null}
        </ScrollView>
        <View style={styles.footer}>
          <Text style={styles.footerText}>
            {`UI in React · data and rules in ${host.language ? host.language.split(' ')[0] : 'the host'} · ${host.os || ''}`}
          </Text>
          <Pressable onPress={() => Host.postMessage('app:quit', {})} style={styles.quit}>
            <Text style={styles.quitText}>Quit</Text>
          </Pressable>
        </View>
      </View>
      <Toast />
    </View>
  );
}

const label = { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute };

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: colors.bg0, alignItems: 'center', justifyContent: 'center', padding: 24 },
  card: { ...panel, width: '100%', maxWidth: 620, flexShrink: 1, padding: 24, borderRadius: 20, gap: 18 },
  header: { flexDirection: 'row', alignItems: 'flex-end' },
  eyebrow: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2.4, color: colors.gold },
  title: { fontFamily: font, fontWeight: '700', fontSize: 32, color: colors.text, marginTop: 4 },
  headerRight: { alignItems: 'flex-end' },
  clock: { fontFamily: font, fontWeight: '700', fontSize: 20, color: colors.cyan, fontVariant: ['tabular-nums'] },
  meta: { fontFamily: font, fontWeight: '500', fontSize: 11, color: colors.textMute, marginTop: 3 },
  label,
  player: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
    padding: 14,
    borderRadius: 14,
    backgroundColor: 'rgba(148, 170, 255, 0.06)',
    borderWidth: 1,
    borderColor: colors.line,
  },
  levelBadge: {
    width: 44,
    height: 44,
    borderRadius: 22,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 2,
    borderColor: colors.gold,
    backgroundColor: 'rgba(242, 195, 91, 0.12)',
    boxShadow: '0 0 14px rgba(242, 195, 91, 0.35)',
  },
  levelText: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.goldLight },
  xpRow: { flexDirection: 'row', justifyContent: 'space-between' },
  xpText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  xpTrack: { height: 8, borderRadius: 4, marginTop: 8, backgroundColor: 'rgba(255, 255, 255, 0.08)', overflow: 'hidden' },
  xpFill: { height: 8, borderRadius: 4, backgroundColor: colors.gold, boxShadow: '0 0 10px rgba(242, 195, 91, 0.7)' },
  addRow: { flexDirection: 'row', gap: 10 },
  input: {
    flex: 1,
    height: 46,
    paddingHorizontal: 14,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: colors.lineStrong,
    backgroundColor: colors.bg1,
    fontFamily: font,
    fontSize: 15,
    color: colors.text,
  },
  addButton: {
    width: 96,
    height: 46,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: colors.gold,
  },
  addButtonHot: { backgroundColor: colors.goldLight, boxShadow: '0 0 16px rgba(242, 195, 91, 0.55)' },
  addLabel: { fontFamily: font, fontWeight: '700', fontSize: 14, letterSpacing: 1.2, textTransform: 'uppercase', color: colors.bg0 },
  listHead: { marginBottom: -8 },
  list: { flexShrink: 1, maxHeight: 300 },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
    height: 52,
    paddingHorizontal: 12,
    borderRadius: 12,
    backgroundColor: colors.panelRaised,
    borderWidth: 1,
    borderColor: colors.line,
  },
  rowDone: { backgroundColor: 'rgba(26, 36, 64, 0.5)' },
  check: {
    width: 26,
    height: 26,
    borderRadius: 8,
    borderWidth: 2,
    borderColor: colors.lineStrong,
    alignItems: 'center',
    justifyContent: 'center',
  },
  checkOn: { backgroundColor: colors.green, borderColor: colors.green, boxShadow: '0 0 10px rgba(74, 222, 128, 0.6)' },
  checkMark: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.bg0 },
  rowTitle: { flex: 1, fontFamily: font, fontWeight: '600', fontSize: 15, color: colors.text },
  rowTitleDone: { color: colors.textMute, textDecorationLine: 'line-through' },
  remove: { width: 30, height: 30, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  removeText: { fontFamily: font, fontWeight: '500', fontSize: 20, color: colors.textMute },
  empty: { fontFamily: font, fontSize: 14, color: colors.textMute, textAlign: 'center', paddingVertical: 18 },
  footer: { flexDirection: 'row', alignItems: 'center', borderTopWidth: 1, borderTopColor: colors.line, paddingTop: 14 },
  footerText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  quit: { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 8, borderWidth: 1, borderColor: colors.line },
  quitText: { ...label, color: colors.textDim },
  toastWrap: { position: 'absolute', left: 0, right: 0, bottom: 28, alignItems: 'center' },
  toast: {
    ...panel,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 18,
    paddingVertical: 12,
    borderRadius: 14,
  },
  toastDot: { width: 8, height: 8, borderRadius: 4 },
  toastText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.text },
});

AppRegistry.registerComponent('App', () => App);
AppRegistry.runApplication('App', {});
