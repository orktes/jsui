// A game's social layer, live: friends with presence, a party you can invite
// them into, chat you can type in, and an incoming invite that slides in.
// Party and chat changes animate with LayoutAnimation; the invite toast and
// the presence pulse run on the native driver.
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, LayoutAnimation, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';

const native = { useNativeDriver: true };

const FRIENDS = [
  { id: 'nyx', name: 'Nyx', tag: '#2041', status: 'online', activity: 'In the lobby', from: '#b26bff', to: '#4fd1ff' },
  { id: 'kade', name: 'Kade', tag: '#0007', status: 'ingame', activity: 'Ranked · Rift 3-2', from: '#ff9a3c', to: '#ff4d6a' },
  { id: 'mira', name: 'Mira', tag: '#7712', status: 'online', activity: 'Customizing loadout', from: '#4ade80', to: '#4fd1ff' },
  { id: 'oren', name: 'Oren', tag: '#3150', status: 'ingame', activity: 'Raid · Ashen Vault', from: '#f2c35b', to: '#ff9a3c' },
  { id: 'sol', name: 'Sol', tag: '#9001', status: 'away', activity: 'Away · 12 min', from: '#7aa2ff', to: '#b26bff' },
];
const STATUS = { online: colors.green, ingame: colors.cyan, away: colors.gold };
const SCRIPT = [
  ['kade', 'gg, that last round was close'],
  ['mira', 'anyone up for the vault after?'],
  ['oren', 'need one more for the raid'],
  ['nyx', 'queue me in, two minutes'],
  ['kade', 'new season pass is out btw'],
  ['mira', 'the rift skin is so good'],
];
const byId = (id) => FRIENDS.find((f) => f.id === id);
const ease = () => LayoutAnimation.configureNext(LayoutAnimation.create(260, 'easeInEaseOut', 'opacity'));

function Avatar({ f, size = 34, ring }) {
  return (
    <View style={{ width: size, height: size }}>
      <View
        style={[
          styles.avatar,
          { width: size, height: size, borderRadius: size / 2, experimental_backgroundImage: `linear-gradient(135deg, ${f.from} 0%, ${f.to} 100%)` },
          ring && { borderWidth: 2, borderColor: colors.text },
        ]}
      >
        <Text style={[styles.initial, { fontSize: size * 0.42 }]}>{f.name[0]}</Text>
      </View>
      {f.status ? <View style={[styles.presence, { backgroundColor: STATUS[f.status] }]} /> : null}
    </View>
  );
}

const ME = { id: 'me', name: 'You', from: '#ffe29a', to: '#f2c35b' };

export function SocialPanel({ width }) {
  const twoCol = width >= 600;
  const [party, setParty] = useState(['nyx']);
  const [pending, setPending] = useState(null);
  const [chat, setChat] = useState(() => [
    { key: 1, from: 'nyx', text: 'ready when you are' },
    { key: 2, system: true, text: 'Nyx joined the party' },
  ]);
  const [draft, setDraft] = useState('');
  const nextKey = useRef(3);
  const say = (line) => {
    ease();
    setChat((c) => [...c.slice(-4), { key: nextKey.current++, ...line }]);
  };

  // Friends chatting on their own.
  useEffect(() => {
    let i = 0;
    const id = setInterval(() => {
      const [from, text] = SCRIPT[i++ % SCRIPT.length];
      say({ from, text });
    }, 4200);
    return () => clearInterval(id);
  }, []);

  // An invite is accepted a moment after it is sent.
  useEffect(() => {
    if (!pending) return undefined;
    const t = setTimeout(() => {
      ease();
      setParty((p) => (p.length < 3 && !p.includes(pending) ? [...p, pending] : p));
      setPending(null);
      say({ system: true, text: `${byId(pending).name} joined the party` });
    }, 1100);
    return () => clearTimeout(t);
  }, [pending]);

  const invite = (id) => {
    if (pending || party.includes(id)) return;
    if (party.length >= 3) {
      // Full: the oldest member leaves to make room.
      ease();
      const [gone, ...rest] = party;
      setParty(rest);
      say({ system: true, text: `${byId(gone).name} left the party` });
    }
    setPending(id);
  };
  const send = () => {
    const t = draft.trim();
    if (!t) return;
    setDraft('');
    say({ from: 'me', text: t });
  };

  const friends = (
    <View style={{ gap: 4 }}>
      <View style={styles.sectionRow}>
        <Text style={styles.sectionLabel}>Friends</Text>
        <Text style={styles.sectionCount}>{FRIENDS.filter((f) => f.status !== 'away').length} online</Text>
      </View>
      {FRIENDS.map((f) => {
        const inParty = party.includes(f.id);
        const busy = pending === f.id;
        return (
          <View key={f.id} style={styles.friend}>
            <Avatar f={f} />
            <View style={{ flex: 1, minWidth: 0 }}>
              <Text style={styles.name} numberOfLines={1}>
                {f.name}
                <Text style={styles.tag}> {f.tag}</Text>
              </Text>
              <Text style={[styles.activity, { color: f.status === 'away' ? colors.textMute : STATUS[f.status] }]} numberOfLines={1}>
                {f.activity}
              </Text>
            </View>
            <Pressable
              onPress={() => invite(f.id)}
              disabled={inParty || busy}
              accessibilityLabel={`Invite ${f.name}`}
              style={[styles.invite, inParty && styles.inviteDone, busy && styles.inviteBusy]}
              hoveredStyle={!inParty && !busy ? styles.inviteHot : undefined}
              focusVisibleStyle={styles.focus}
              pressedStyle={{ transform: [{ scale: 0.94 }] }}
              transition={{ duration: 120, pressed: 40 }}
            >
              <Text style={[styles.inviteText, inParty && { color: colors.green }, busy && { color: colors.cyan }]}>
                {inParty ? 'In party' : busy ? 'Sent…' : 'Invite'}
              </Text>
            </Pressable>
          </View>
        );
      })}
    </View>
  );

  const chatBox = (
    <View style={[styles.chat, twoCol && { flex: 1 }]}>
      <View style={styles.sectionRow}>
        <Text style={styles.sectionLabel}>Party chat</Text>
        <View style={styles.liveDot} />
      </View>
      <View style={styles.lines}>
        {chat.map((m) =>
          m.system ? (
            <Text key={m.key} style={styles.system}>
              {m.text}
            </Text>
          ) : (
            <Text key={m.key} style={styles.line} numberOfLines={2}>
              <Text style={[styles.lineName, { color: m.from === 'me' ? colors.gold : byId(m.from).from }]}>{m.from === 'me' ? 'You' : byId(m.from).name}  </Text>
              {m.text}
            </Text>
          ),
        )}
      </View>
      <View style={styles.composer}>
        <TextInput
          value={draft}
          onChangeText={setDraft}
          onSubmitEditing={send}
          placeholder="Say something…"
          placeholderTextColor={colors.textMute}
          maxLength={80}
          style={styles.input}
        />
        <Pressable onPress={send} style={styles.send} hoveredStyle={styles.inviteHot} pressedStyle={{ transform: [{ scale: 0.92 }] }} transition={{ duration: 100, pressed: 40 }}>
          <Text style={styles.sendText}>↵</Text>
        </Pressable>
      </View>
    </View>
  );

  return (
    <View style={[styles.panel, { width }]}>
      <View pointerEvents="none" style={styles.glow} />
      <View style={styles.header}>
        <Text style={styles.title}>Social</Text>
        <View style={{ flex: 1 }} />
        <Text style={styles.hint}>Invite a friend, or type in the chat</Text>
      </View>
      <Party party={party} pending={pending} />
      <View style={[styles.body, twoCol ? { flexDirection: 'row' } : { flexDirection: 'column' }]}>
        <View style={twoCol ? { width: Math.round(width * 0.48) } : null}>{friends}</View>
        {chatBox}
      </View>
      <InviteToast />
    </View>
  );
}

// You plus up to three friends; an invited one waits in a dashed slot.
function Party({ party, pending }) {
  const slots = [ME, ...party.map(byId)];
  return (
    <View style={styles.party}>
      <Text style={styles.sectionLabel}>Party</Text>
      <View style={styles.slots}>
        {[0, 1, 2, 3].map((i) => {
          const f = slots[i];
          if (f) {
            return (
              <View key={f.id} style={styles.slot}>
                <Avatar f={{ ...f, status: null }} size={38} ring={f.id === 'me'} />
                <Text style={styles.slotName} numberOfLines={1}>
                  {f.name}
                </Text>
              </View>
            );
          }
          const waiting = pending && i === slots.length;
          return (
            <View key={'empty' + i} style={styles.slot}>
              <View style={[styles.emptySlot, waiting && { borderColor: colors.cyan }]}>
                <Text style={[styles.plus, waiting && { color: colors.cyan }]}>{waiting ? '…' : '+'}</Text>
              </View>
              <Text style={[styles.slotName, { color: colors.textMute }]}>{waiting ? 'Invited' : 'Open'}</Text>
            </View>
          );
        })}
      </View>
    </View>
  );
}

// Every few seconds a friend invites you to their match: the toast slides in,
// its timer bar drains, and it slides away (all native).
function InviteToast() {
  const t = useRef(new Animated.Value(0)).current;
  const bar = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.delay(2600),
        Animated.parallel([
          Animated.spring(t, { toValue: 1, stiffness: 260, damping: 22, ...native }),
          Animated.timing(bar, { toValue: 1, duration: 0, ...native }),
        ]),
        Animated.timing(bar, { toValue: 0, duration: 4200, easing: Easing.linear, ...native }),
        Animated.timing(t, { toValue: 0, duration: 260, easing: Easing.in(Easing.cubic), ...native }),
        Animated.delay(5200),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [t, bar]);
  const kade = byId('kade');
  return (
    <Animated.View
      pointerEvents="none"
      style={[
        styles.toast,
        { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [-16, 0] }) }, { scale: t.interpolate({ inputRange: [0, 1], outputRange: [0.96, 1] }) }] },
      ]}
    >
      <Avatar f={kade} size={30} />
      <View style={{ flex: 1 }}>
        <Text style={styles.toastTitle}>Kade invited you</Text>
        <Text style={styles.toastSub}>Ranked · Rift 3-2</Text>
      </View>
      <Text style={styles.toastKey}>A</Text>
      <View style={styles.toastTrack}>
        <Animated.View style={[styles.toastBar, { transform: [{ scaleX: bar }] }]} />
      </View>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  panel: {
    padding: 18,
    gap: 14,
    borderRadius: 20,
    borderWidth: 1,
    borderColor: 'rgba(79, 209, 255, 0.28)',
    backgroundColor: 'rgba(9, 13, 26, 0.94)',
    overflow: 'hidden',
    boxShadow: '0 30px 80px rgba(0,0,0,0.55), inset 0 1px 0 rgba(255,255,255,0.05)',
  },
  glow: { ...StyleSheet.absoluteFillObject, experimental_backgroundImage: 'radial-gradient(ellipse at 100% 0%, rgba(79,209,255,0.14) 0%, rgba(9,13,26,0) 55%)' },
  header: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  title: { fontFamily: font, fontWeight: '700', fontSize: 18, letterSpacing: 0.4, color: colors.text },
  hint: { fontFamily: font, fontSize: 12, color: colors.textMute },
  party: { gap: 10, padding: 12, borderRadius: 14, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(255,255,255,0.02)' },
  slots: { flexDirection: 'row', gap: 10 },
  slot: { flex: 1, alignItems: 'center', gap: 6 },
  slotName: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.text },
  emptySlot: { width: 38, height: 38, borderRadius: 19, borderWidth: 1.5, borderStyle: 'dashed', borderColor: 'rgba(148,170,255,0.35)', alignItems: 'center', justifyContent: 'center' },
  plus: { fontFamily: font, fontWeight: '600', fontSize: 18, color: colors.textMute, marginTop: -2 },
  body: { gap: 16 },
  sectionRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', height: 22, paddingHorizontal: 2 },
  sectionLabel: { fontFamily: mono, fontWeight: '700', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute },
  sectionCount: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.green },
  friend: { flexDirection: 'row', alignItems: 'center', gap: 11, height: 50, paddingHorizontal: 6, borderRadius: 12 },
  avatar: { alignItems: 'center', justifyContent: 'center' },
  initial: { fontFamily: font, fontWeight: '700', color: '#0a0f1d' },
  presence: { position: 'absolute', right: -1, bottom: -1, width: 11, height: 11, borderRadius: 6, borderWidth: 2, borderColor: '#090d1a' },
  name: { fontFamily: font, fontWeight: '700', fontSize: 14, color: colors.text },
  tag: { fontFamily: mono, fontWeight: '400', fontSize: 11, color: colors.textMute },
  activity: { fontFamily: font, fontSize: 12, marginTop: 1 },
  invite: { height: 30, paddingHorizontal: 12, borderRadius: 9, borderWidth: 1, borderColor: 'rgba(79,209,255,0.45)', justifyContent: 'center', cursor: 'pointer' },
  inviteHot: { backgroundColor: 'rgba(79,209,255,0.12)' },
  inviteDone: { borderColor: 'rgba(74,222,128,0.35)', backgroundColor: 'rgba(74,222,128,0.06)' },
  inviteBusy: { borderColor: 'rgba(79,209,255,0.25)' },
  inviteText: { fontFamily: font, fontWeight: '700', fontSize: 12, color: colors.cyan },
  focus: { borderColor: colors.gold },
  chat: { gap: 8, padding: 12, borderRadius: 14, borderWidth: 1, borderColor: site.stroke, backgroundColor: 'rgba(3,5,12,0.6)' },
  liveDot: { width: 7, height: 7, borderRadius: 4, backgroundColor: colors.green, boxShadow: '0 0 8px rgba(74,222,128,0.9)' },
  lines: { flex: 1, minHeight: 168, justifyContent: 'flex-end', gap: 7, overflow: 'hidden' },
  line: { fontFamily: font, fontSize: 13, lineHeight: 18, color: colors.textDim },
  lineName: { fontWeight: '700' },
  system: { fontFamily: font, fontStyle: 'italic', fontSize: 12, color: colors.textMute },
  composer: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  input: {
    flex: 1,
    height: 36,
    paddingHorizontal: 12,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: site.strokeStrong,
    backgroundColor: 'rgba(255,255,255,0.03)',
    fontFamily: font,
    fontSize: 13,
    color: colors.text,
  },
  send: { width: 36, height: 36, borderRadius: 10, borderWidth: 1, borderColor: 'rgba(79,209,255,0.45)', alignItems: 'center', justifyContent: 'center', cursor: 'pointer' },
  sendText: { fontFamily: font, fontWeight: '700', fontSize: 15, color: colors.cyan },
  toast: {
    position: 'absolute',
    right: 14,
    top: 12,
    width: 236,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 12,
    paddingTop: 10,
    paddingBottom: 13,
    borderRadius: 14,
    borderWidth: 1,
    borderColor: 'rgba(255,154,60,0.5)',
    backgroundColor: 'rgba(20, 14, 10, 0.97)',
    boxShadow: '0 16px 40px rgba(0,0,0,0.6)',
    overflow: 'hidden',
  },
  toastTitle: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.text },
  toastSub: { fontFamily: font, fontSize: 11.5, color: colors.orange },
  toastKey: { fontFamily: font, fontWeight: '700', fontSize: 11, color: '#0a0f1d', backgroundColor: colors.green, width: 20, height: 20, borderRadius: 10, textAlign: 'center', lineHeight: 20, overflow: 'hidden' },
  toastTrack: { position: 'absolute', left: 0, right: 0, bottom: 0, height: 3, backgroundColor: 'rgba(255,255,255,0.06)' },
  toastBar: { height: 3, width: '100%', backgroundColor: colors.orange, transformOrigin: 'left' },
});
