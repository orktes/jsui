// "Runs in" band under the hero: the hosts JSUI has a working example for
// (monochrome marks, tinted to the site palette), then the ones in progress.
// Logos: the Godot mark (Andrea Calabró, CC BY 4.0, used to say JSUI runs in
// Godot), the WebAssembly logo (CC0) and the Android robot's head (Google,
// CC BY 3.0: "The Android robot is reproduced or modified from work created
// and shared by Google and used according to terms described in the Creative
// Commons 3.0 Attribution License"); the other glyphs are generic (raylib,
// SDL3, sokol, Unity, Unreal, Rust and Go included: their own logos are not
// used).
import React from 'react';
import { Image, Linking, Pressable, StyleSheet, Text, View } from 'react-native';
import { colors, font, site } from '../../theme.js';
import { useRouter } from '../../router.js';
import { GITHUB, useLayout } from '../../components/ui.jsx';

const TARGETS = [
  { name: 'Godot 4', icon: require('../../assets/engines/godot@2x.png'), to: '/embed' },
  { name: 'Web', sub: 'WebAssembly', icon: require('../../assets/engines/webassembly@2x.png'), to: '/embed' },
  { name: 'iOS', sub: 'Metal', icon: require('../../assets/engines/ios@2x.png'), href: GITHUB + '/tree/main/integrations/ios' },
  { name: 'Android', sub: 'GLES', icon: require('../../assets/engines/android@2x.png'), href: GITHUB + '/tree/main/integrations/android' },
  { name: 'Custom engine', sub: 'C API', icon: require('../../assets/engines/custom@2x.png'), to: '/embed' },
  { name: 'Rust', sub: 'crate', icon: require('../../assets/engines/rust@2x.png'), href: GITHUB + '/tree/main/bindings/rust' },
  { name: 'Go', sub: 'cgo package', icon: require('../../assets/engines/go@2x.png'), href: GITHUB + '/tree/main/bindings/go' },
  { name: 'raylib', icon: require('../../assets/engines/raylib@2x.png'), href: GITHUB + '/tree/main/integrations/raylib' },
  { name: 'SDL3', icon: require('../../assets/engines/sdl@2x.png'), href: GITHUB + '/tree/main/integrations/sdl3' },
  { name: 'sokol', icon: require('../../assets/engines/sokol@2x.png'), href: GITHUB + '/tree/main/integrations/sokol' },
  { name: 'Unity', sub: 'C# package', icon: require('../../assets/engines/unity@2x.png'), href: GITHUB + '/tree/main/integrations/unity' },
  { name: 'Unreal', sub: 'UE5 plugin', icon: require('../../assets/engines/unreal@2x.png'), href: GITHUB + '/tree/main/integrations/unreal' },
  { name: 'Standalone', sub: 'SDL2 viewer', icon: require('../../assets/engines/standalone@2x.png'), href: GITHUB + '/tree/main/tools/jsui-viewer' },
];
const SOON = [];

function Target({ t }) {
  const { go } = useRouter();
  return (
    <Pressable onPress={() => (t.href ? Linking.openURL(t.href) : go(t.to))} style={{ cursor: 'pointer' }}>
      {({ hovered, focused }) => {
        const hot = hovered || focused;
        return (
          <View style={[styles.target, hot && styles.targetHot, focused && { boxShadow: `0 0 0 2px ${colors.cyan}` }]}>
            <Image source={t.icon} style={styles.icon} tintColor={hot ? colors.text : colors.textDim} />
            <Text style={[styles.name, hot && { color: colors.text }]}>{t.name}</Text>
            {t.sub ? <Text style={styles.sub}>{t.sub}</Text> : null}
          </View>
        );
      }}
    </Pressable>
  );
}

export function RunsIn() {
  const { phone } = useLayout();
  return (
    <View style={[styles.band, phone && { paddingHorizontal: 8 }]}>
      <Text style={styles.label}>Runs in</Text>
      <View style={styles.list}>
        {TARGETS.map((t) => <Target key={t.name} t={t} />)}
        {SOON.map((n) => (
          <View key={n} style={[styles.target, { opacity: 0.6 }]}>
            <Text style={styles.name}>{n}</Text>
            <View style={styles.soon}><Text style={styles.soonText}>Soon</Text></View>
          </View>
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  band: {
    gap: 8,
    paddingTop: 14,
    paddingBottom: 10,
    paddingHorizontal: 12,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: site.stroke,
    backgroundColor: 'rgba(10,15,29,0.55)',
  },
  label: { marginLeft: 10, fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2.2, textTransform: 'uppercase', color: colors.textMute },
  list: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 4 },
  target: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 36, paddingHorizontal: 10, borderRadius: 10, borderWidth: 1, borderColor: 'rgba(148,170,255,0)' },
  targetHot: { borderColor: site.strokeStrong, backgroundColor: 'rgba(255,255,255,0.04)' },
  icon: { width: 20, height: 20 },
  name: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
  sub: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  soon: { paddingHorizontal: 6, height: 18, borderRadius: 9, borderWidth: 1, borderColor: site.strokeStrong, justifyContent: 'center' },
  soonText: { fontFamily: font, fontWeight: '700', fontSize: 9.5, letterSpacing: 0.8, textTransform: 'uppercase', color: colors.textMute },
});
