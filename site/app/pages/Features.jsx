// /features and /features/<page>: what the runtime can do. The overview links
// every capability to where it is shown live (a feature page, an example
// screen or the docs); each feature page is a lab of live demos, under a tab
// strip that switches between them.
import React from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../theme.js';
import { useRouter } from '../router.js';
import { FEATURE_PAGES } from '../components/sections.js';
import { Column, GITHUB, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { Features } from './home/Features.jsx';
import { InteractionsPage } from './Interactions.jsx';
import { InputPage } from './Input.jsx';
import { MotionPage } from './Motion.jsx';
import { VectorPage } from './Vector.jsx';

const PAGES = { interactions: InteractionsPage, input: InputPage, motion: MotionPage, vector: VectorPage };

const DOCS = GITHUB + '/blob/main/docs/';

// The four labs, as big cards at the top of the overview.
const LABS = [
  { glyph: 'UX', title: 'Interactions', body: 'A whole game hub driven by mouse, touch, keyboard and gamepad: springs, carousels, flings, text input and panels.', color: colors.cyan, to: '/features/interactions', cta: 'Open the hub' },
  { glyph: 'A', title: 'Input', body: 'useGamepad live, key events with real codes, spatial focus with arrows and D-pad, pointer and touch labs.', color: colors.gold, to: '/features/input', cta: 'Grab a controller' },
  { glyph: '◆', title: 'Motion', body: 'Animated and LayoutAnimation on the native driver: spring lab, easings, loops, interpolation, layout and fling.', color: colors.purple, to: '/features/motion', cta: 'Open the motion lab' },
  { glyph: 'Sk', title: 'SVG & Skia Canvas', body: "react-native-svg, @shopify/react-native-skia and react-native-worklets, imported by their usual names and drawn natively.", color: colors.green, to: '/features/vector', cta: 'Draw something' },
];

// Every capability, linked to where it can be seen working.
const CAPABILITIES = [
  { glyph: '</>', title: 'React Native API', body: "View, Text, Image, ScrollView, FlatList, TextInput, Modal, Pressable, StyleSheet. Import from 'react-native' and it just works.", color: colors.cyan, to: '/playground', cta: 'Try it live' },
  { glyph: 'A', title: 'Gamepad-first focus', body: 'Spatial D-pad and stick navigation, focus scopes for modals, focus-visible rings, Tab order and hold-to-repeat, in the runtime.', color: colors.gold, to: '/features/input', cta: 'Grab a controller' },
  { glyph: '◆', title: 'Native Animated driver', body: 'useNativeDriver: true moves springs, timings, decays and interpolations into C++. They keep running while your JS is busy.', color: colors.purple, to: '/features/motion', cta: 'Open the motion lab' },
  { glyph: 'Sk', title: 'Skia Canvas', body: 'The @shopify/react-native-skia API: Groups, Paths, gradients, blur masks, trim paths and the imperative Skia object, animated natively.', color: colors.green, to: '/features/vector', cta: 'See the canvas' },
  { glyph: 'SVG', title: 'SVG', body: 'react-native-svg: paths, gradients, clip paths, masks, <Use> and SvgXml, painted by Skia in C++ and crisp at any scale.', color: colors.cyan, to: '/features/vector', cta: 'See the icons' },
  { glyph: 'W', title: 'Worklets', body: "react-native-worklets: 'worklet' functions run on a small UI runtime the render loop ticks, writing Animated values directly.", color: colors.goldLight, to: '/features/vector', cta: 'Run the swarm' },
  { glyph: 'Fx', title: 'SkSL shaders', body: 'ShaderView runs your own Skia shaders with Animated uniforms; shaders can be precompiled off-frame so nothing hitches.', color: colors.red, to: '/examples/rift', cta: 'Enter the Rift' },
  { glyph: '▶', title: 'Video and host textures', body: 'A <Video> element backed by each host’s player, and <HostTexture> to show any texture the engine renders: 3D views, minimaps, cameras.', color: colors.orange, to: '/examples/cinematic', cta: 'Watch the trailer' },
  { glyph: 'Lo', title: 'Lottie', body: 'LottieView on Skia’s Skottie: After Effects animations as JSON, played natively, scrubbable with Animated progress.', color: colors.blue, to: '/examples/victory', cta: 'See the victory screen' },
  { glyph: 'dB', title: 'Audio', body: 'Sound, useSound, useMusic with crossfades, and UISounds for every hover, focus and press; the host plays them (Web Audio, Godot buses).', color: colors.gold, to: '/examples/main-menu', cta: 'Turn the sound on' },
  { glyph: 'Aa', title: 'Text, fonts and i18n', body: 'HarfBuzz shaping, bidi and RTL, rich nested text, variable fonts, WOFF2 and fallbacks, and a native TextInput with selection and undo.', color: colors.red, to: '/examples/dialogue', cta: 'Read the dialogue' },
  { glyph: '≡', title: 'Virtualized lists', body: 'FlatList and SectionList window their cells like React Native, keep the focused row mounted and glide it into view.', color: colors.goldLight, to: '/features/interactions', cta: 'Fling a list' },
  { glyph: ':h', title: 'Native state styles', body: 'pressedStyle, hoveredStyle and focusedStyle (and group styles for children) switch natively, with transitions and no React render.', color: colors.purple, href: DOCS + 'NATIVE_FIRST.md', cta: 'Native-first patterns' },
  { glyph: 'JS', title: 'JS off the render path', body: 'On native hosts React runs on its own JS thread: a heavy commit never holds a frame, and the UI keeps animating meanwhile.', color: colors.green, href: DOCS + 'PERFORMANCE.md', cta: 'Read the numbers' },
  { glyph: 'GPU', title: 'GPU everywhere', body: 'Skia Graphite on WebGPU, Metal, Vulkan and D3D12, Ganesh on GL; drawn straight into the engine’s own target. CPU only as a fallback.', color: colors.blue, to: '/embed', cta: 'Rendering options' },
  { glyph: 'F5', title: 'Fast Refresh and a debugger', body: 'jsui dev reloads on save keeping component state, in any host; Chrome DevTools and VS Code debug the app’s JS.', color: colors.orange, to: '/embed', cta: 'Set up a host' },
];

export function FeaturesPage({ params = [] }) {
  const Lab = PAGES[params[0]];
  if (Lab) {
    return (
      <View>
        <Tabs current={'/features/' + params[0]} />
        <Lab />
      </View>
    );
  }
  return <Overview />;
}

function Overview() {
  const { phone } = useLayout();
  return (
    <View style={{ paddingBottom: 40 }}>
      <Tabs current="/features" />
      <Column style={{ paddingTop: phone ? 28 : 52 }}>
        <Reveal>
          <SectionHead
            eyebrow="Features"
            title="Everything a game UI needs, in one small runtime."
            lead="Each lab below is a page of live demos you can poke at with a mouse, touch, keyboard or controller. Under them, every capability links to where it is running on this site."
          />
        </Reveal>
        <View style={{ marginTop: phone ? 28 : 44 }}>
          <Features items={LABS} />
        </View>
        <View style={{ marginTop: phone ? 56 : 88, gap: phone ? 24 : 36 }}>
          <SectionHead eyebrow="Capabilities" title="And everything else" lead="React Native's API plus what games need on top of it. Each card opens the page or example screen that shows it." />
          <Features items={CAPABILITIES} />
        </View>
      </Column>
    </View>
  );
}

// The strip of feature pages under the nav bar: Overview plus each lab.
function Tabs({ current }) {
  const { go } = useRouter();
  const { gutter } = useLayout();
  const tabs = [{ to: '/features', label: 'Overview' }, ...FEATURE_PAGES];
  return (
    <View style={styles.strip}>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={{ flexGrow: 1, justifyContent: 'center' }}>
        <View style={[styles.tabs, { paddingHorizontal: gutter }]}>
          <Text style={styles.label}>Features</Text>
          {tabs.map((t) => {
            const active = t.to === current;
            return (
              <Pressable
                key={t.to}
                onPress={() => (active ? null : go(t.to))}
                accessibilityRole="tab"
                accessibilityState={{ selected: active }}
                style={[styles.tab, active && styles.tabActive]}
                hoveredStyle={styles.tabHot}
                focusVisibleStyle={styles.tabFocus}
                pressedStyle={{ transform: [{ scale: 0.96 }] }}
                transition={{ duration: 120, pressed: 40 }}
              >
                <Text style={[styles.tabText, active && { color: colors.text }]}>{t.label}</Text>
              </Pressable>
            );
          })}
        </View>
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  strip: { borderBottomWidth: 1, borderBottomColor: site.stroke, backgroundColor: 'rgba(5, 7, 15, 0.55)' },
  tabs: { flexDirection: 'row', alignItems: 'center', gap: 6, height: 54, width: '100%', maxWidth: 1240 + 80, alignSelf: 'center' },
  label: { fontFamily: mono, fontSize: 11, letterSpacing: 1.6, textTransform: 'uppercase', color: colors.textMute, marginRight: 10 },
  tab: { height: 34, paddingHorizontal: 14, borderRadius: 17, justifyContent: 'center', borderWidth: 1, borderColor: 'transparent', cursor: 'pointer' },
  tabActive: { backgroundColor: 'rgba(242, 195, 91, 0.10)', borderColor: 'rgba(242, 195, 91, 0.45)' },
  tabHot: { backgroundColor: 'rgba(255, 255, 255, 0.05)' },
  tabFocus: { borderColor: colors.cyan },
  tabText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
});
