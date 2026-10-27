// Hero feature highlights: a compact two-column list under the hero copy, each
// row a link to the page (or doc) that shows it, plus the one-line agent skill
// install with a copy button.
import React, { useRef, useState } from 'react';
import { Animated, Host, Linking, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../../theme.js';
import { useRouter } from '../../router.js';
import { GITHUB, Reveal, useLayout } from '../../components/ui.jsx';

export const SKILL_INSTALL = 'npx @orktes/jsui-cli skill install';

export const HIGHLIGHTS = [
  { glyph: '>_', title: 'Built for agents', body: 'Skill, headless screenshots, shared live view', color: colors.cyan, href: GITHUB + '#coding-agents-and-headless-checks' },
  { glyph: 'HMR', title: 'Live on every edit', body: 'Fast Refresh keeps component state', color: colors.gold, to: '/embed' },
  { glyph: 'GPU', title: 'GPU everywhere', body: 'WebGPU, WebGL, Metal and Vulkan', color: colors.purple, to: '/embed' },
  { glyph: 'C', title: 'Embeds in your engine', body: 'Godot, C API or WASM; input passes through', color: colors.orange, to: '/embed' },
  { glyph: 'A', title: 'Gamepad-first focus', body: 'Spatial D-pad navigation, built in', color: colors.green, to: '/features/input' },
  { glyph: '◆', title: 'Native animation', body: 'Animated in C++, plus Lottie', color: colors.blue, to: '/features/motion' },
  { glyph: 'Fx', title: 'Shaders and video', body: 'SkSL effects, engine textures, video', color: colors.red, to: '/examples/rift' },
  { glyph: '01', title: 'Per-frame game data', body: 'Typed arrays, no JS per frame', color: colors.goldLight, to: '/examples/cinematic' },
];

function Highlight({ h, width }) {
  const { go } = useRouter();
  const lift = useRef(new Animated.Value(0)).current;
  const to = (v) => Animated.spring(lift, { toValue: v, stiffness: 360, damping: 24, useNativeDriver: true }).start();
  return (
    <Pressable
      onPress={() => (h.href ? Linking.openURL(h.href) : go(h.to))}
      onHoverIn={() => to(1)}
      onHoverOut={() => to(0)}
      onFocus={() => to(1)}
      onBlur={() => to(0)}
      style={{ width, flex: 1, cursor: 'pointer' }}
    >
      {({ hovered, focused, pressed }) => (
        <Animated.View
          style={[
            styles.row,
            { flex: 1 },
            (hovered || focused) && { borderColor: h.color + '44', backgroundColor: 'rgba(16,22,40,0.7)' },
            focused && { boxShadow: `0 0 0 2px ${site.bg}, 0 0 0 4px ${colors.cyan}` },
            { transform: [{ translateX: lift.interpolate({ inputRange: [0, 1], outputRange: [0, 3] }) }, { scale: pressed ? 0.98 : 1 }] },
          ]}
        >
          <View style={[styles.icon, { borderColor: h.color + '66', backgroundColor: h.color + '1a' }, (hovered || focused) && { boxShadow: `0 0 14px ${h.color}55` }]}>
            <Text style={[styles.iconText, { color: h.color }, h.glyph.length > 2 && { fontSize: 9.5, letterSpacing: 0 }]}>{h.glyph}</Text>
          </View>
          <View style={{ flex: 1, gap: 2 }}>
            <Text style={styles.title}>{h.title}</Text>
            <Text style={styles.body}>{h.body}</Text>
          </View>
        </Animated.View>
      )}
    </Pressable>
  );
}

/** The agent skill one-liner with a copy button (the page copies it to the system clipboard). */
export function SkillInstall() {
  const [copied, setCopied] = useState(false);
  const { phone } = useLayout();
  const copy = () => {
    Host.postMessage('copy', SKILL_INSTALL);
    setCopied(true);
    setTimeout(() => setCopied(false), 1600);
  };
  return (
    <View style={styles.install}>
      <Text style={styles.prompt}>$</Text>
      <Text style={[styles.cmd, phone && { fontSize: 12 }]} numberOfLines={1}>{SKILL_INSTALL}</Text>
      <Pressable onPress={copy} style={{ cursor: 'pointer' }}>
        {({ hovered, focused }) => (
          <View style={[styles.copy, (hovered || focused) && styles.copyHot, focused && { boxShadow: `0 0 0 2px ${colors.cyan}` }, copied && { borderColor: colors.green + '88' }]}>
            <Text style={[styles.copyText, copied && { color: colors.green }]}>{copied ? 'Copied' : 'Copy'}</Text>
          </View>
        )}
      </Pressable>
    </View>
  );
}

/** `width`: the column width; two columns when there is room. */
export function Highlights({ width, delay = 0 }) {
  const cols = width >= 900 ? 4 : width >= 400 ? 2 : 1;
  const gap = 4;
  // Rows bleed into the gutter by their padding, so icons line up with the text above.
  const w = Math.floor((width + 16 - gap * (cols - 1)) / cols);
  return (
    <View style={[styles.grid, { gap, marginHorizontal: -8 }]}>
      {HIGHLIGHTS.map((h, i) => (
        <Reveal key={h.title} delay={delay + 40 * Math.floor(i / cols)} distance={10}>
          <Highlight h={h} width={w} />
        </Reveal>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  grid: { flexDirection: 'row', flexWrap: 'wrap' },
  row: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    gap: 11,
    paddingVertical: 8,
    paddingHorizontal: 8,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'rgba(148,170,255,0)',
  },
  icon: { width: 30, height: 30, borderRadius: 8, borderWidth: 1, alignItems: 'center', justifyContent: 'center', marginTop: 1 },
  iconText: { fontFamily: mono, fontWeight: '700', fontSize: 11.5, letterSpacing: -0.3 },
  title: { fontFamily: font, fontWeight: '600', fontSize: 14, lineHeight: 19, color: colors.text },
  body: { fontFamily: font, fontWeight: '400', fontSize: 12.5, lineHeight: 17, color: colors.textMute },
  install: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    height: 44,
    paddingLeft: 14,
    paddingRight: 6,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'rgba(79,209,255,0.28)',
    backgroundColor: site.code,
    boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.04), 0 10px 30px rgba(0,0,0,0.35)',
  },
  prompt: { fontFamily: mono, fontWeight: '700', fontSize: 13, color: colors.cyan },
  cmd: { flex: 1, fontFamily: mono, fontSize: 13, color: '#d6def5' },
  copy: { height: 30, paddingHorizontal: 12, borderRadius: 8, borderWidth: 1, borderColor: site.strokeStrong, justifyContent: 'center' },
  copyHot: { backgroundColor: 'rgba(255,255,255,0.06)', borderColor: 'rgba(79,209,255,0.5)' },
  copyText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
});
