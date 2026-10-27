// /features/native: native components, view classes the host registers
// (jsui_register_component in C / C++, registerComponent on the web) that
// draw straight into Skia, used from React with requireNativeComponent. On
// this site the classes are page JS (site/public/components.js, registered
// with createJsui's `components`); in a game they are its own C++ reading its
// own state, like the engine examples' radar (integrations/shared/radar.h).
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, Pressable, StyleSheet, Text, View, codegenNativeCommands, hasNativeComponent, requireNativeComponent } from 'react-native';
import { colors, font, mono, site } from '../theme.js';
import { Column, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { DemoButton, DemoCard } from './motion/controls.jsx';

const Radar = requireNativeComponent('Radar');
const RadarCommands = codegenNativeCommands({ supportedCommands: ['select'] });
const Gauge = requireNativeComponent('Gauge');
const native = { useNativeDriver: true };

function Missing() {
  return (
    <View style={styles.missing}>
      <Text style={styles.stat}>This host registered no native components.</Text>
    </View>
  );
}

// --- 01: the radar ------------------------------------------------------------------

function RadarDemo() {
  const { phone } = useLayout();
  const size = phone ? 240 : 300;
  const ref = useRef(null);
  const flash = useRef(new Animated.Value(0)).current;
  const [count, setCount] = useState(7);
  const [picked, setPicked] = useState(null);
  const renders = useRef(0);
  renders.current++;
  const onSelect = (e) => {
    const { index, name, hue } = e.nativeEvent;
    setPicked(index < 0 ? null : { name, hue });
    if (index < 0) return;
    flash.setValue(1);
    Animated.timing(flash, { toValue: 0, duration: 700, easing: Easing.out(Easing.quad), ...native }).start();
  };
  return (
    <DemoCard
      index={1}
      title="A radar the game draws"
      blurb="The radar is a class registered by the host. Its sweep and contacts are drawn with Skia every frame, outside React: in a game that is C++ reading the units straight from the simulation. React lays it out and talks to it with props, ref commands (tap a contact) and events back. The rim's flash is an Animated prop on the native driver."
      tags={[['requireNativeComponent', colors.orange], ['commands', colors.cyan], ['events', colors.purple], ['Animated props', colors.green]]}
      codeTitle="Radar.jsx"
      code={`
import { requireNativeComponent, codegenNativeCommands } from 'react-native';

const Radar = requireNativeComponent('Radar');
const Commands = codegenNativeCommands({ supportedCommands: ['select'] });

<Pressable onPress={(e) =>
  Commands.select(ref.current, e.nativeEvent.locationX, e.nativeEvent.locationY)}>
  <Radar
    ref={ref}
    count={count}                 // plain props: props(json) natively
    accent="#4fd1ff"
    flash={flash}                 // Animated, native driver: no React render
    onSelect={(e) => setPicked(e.nativeEvent.name)}  // jsui_component_emit
    style={{ width: 300, height: 300 }}
  />
</Pressable>`}
    >
      <View style={styles.center}>
        {hasNativeComponent('Radar') ? (
          <Pressable
            accessibilityLabel="Radar: tap a contact"
            onPress={(e) => RadarCommands.select(ref.current, e.nativeEvent.locationX, e.nativeEvent.locationY)}
            style={{ cursor: 'crosshair' }}
          >
            <Radar ref={ref} count={count} accent={colors.cyan} flash={flash} onSelect={onSelect} style={{ width: size, height: size }} />
          </Pressable>
        ) : (
          <Missing />
        )}
        <Text style={[styles.picked, picked && { color: picked.hue }]}>
          {picked ? `Locked on ${picked.name}` : 'Tap a contact to lock on'}
        </Text>
        <View style={styles.bar}>
          <DemoButton label="Fewer" onPress={() => setCount((c) => Math.max(1, c - 2))} />
          <DemoButton label="More" primary onPress={() => setCount((c) => Math.min(24, c + 2))} />
          <Text style={styles.stat}>React renders: {renders.current}</Text>
        </View>
      </View>
    </DemoCard>
  );
}

// --- 02: a gauge on an Animated prop ------------------------------------------------

function GaugeDemo() {
  const value = useRef(new Animated.Value(0.2)).current;
  const [boost, setBoost] = useState(false);
  useEffect(() => {
    const anim = boost
      ? Animated.loop(Animated.sequence([
        Animated.timing(value, { toValue: 0.96, duration: 900, easing: Easing.out(Easing.cubic), ...native }),
        Animated.timing(value, { toValue: 0.78, duration: 500, easing: Easing.inOut(Easing.quad), ...native }),
      ]))
      : Animated.spring(value, { toValue: 0.2, stiffness: 90, damping: 12, ...native });
    anim.start();
    return () => anim.stop();
  }, [boost]);
  return (
    <DemoCard
      index={2}
      title="A gauge on the native driver"
      blurb="A class without a clock repaints only when its props change. Its value is an Animated.Value on the native driver: each frame the runtime hands the new value to draw(), and React never renders. The C++ side reads it with jsui_prop_number."
      tags={[['no clock', colors.gold], ['native driver', colors.green], ['C API', colors.cyan]]}
      codeTitle="gauge.cpp"
      code={`
struct Gauge : jsui::Component {
  void draw(jsui::Canvas& c, const JsuiDrawInfo& info) override {
    float v = float(c.number("value", 0));   // Animated: its current value
    float r = std::min(info.width, info.height) / 2 - 10;
    c.arc(10, 10, 2 * r, 2 * r, 150, 240, false,
          jsui::Paint(0x14ffffff).stroke(12).round());
    c.arc(10, 10, 2 * r, 2 * r, 150, 240 * v, false,
          jsui::Paint(0xff4ade80).stroke(12).round());
    c.text(std::to_string(int(v * 9000)).c_str(), r + 10, r + 30,
           jsui::Font(26, 800).centered(), jsui::Paint(0xffe8ecf6));
  }
};

jsui::registerComponent<Gauge>(rt, "Gauge");`}
    >
      <View style={styles.center}>
        {hasNativeComponent('Gauge') ? (
          <Gauge value={value} label="RPM" style={{ width: 260, height: 200 }} />
        ) : (
          <Missing />
        )}
        <View style={styles.bar}>
          <DemoButton label={boost ? 'Ease off' : 'Boost'} primary={!boost} onPress={() => setBoost((b) => !b)} />
        </View>
      </View>
    </DemoCard>
  );
}

export function NativePage() {
  const { phone } = useLayout();
  return (
    <Column style={{ paddingTop: phone ? 36 : 64, paddingBottom: 40 }}>
      <Reveal>
        <SectionHead
          eyebrow="Native components"
          title="Your own views, drawn by the game, used from React."
          lead="Register a view class from C, C++ or page JS that draws with Skia, and the app uses it like any React Native native component: requireNativeComponent, props (Animated ones included), ref commands and events. Minimaps, radars, gauges and damage graphs draw straight from game state, every frame, without a React render."
        />
      </Reveal>
      <View style={{ gap: phone ? 18 : 28, marginTop: phone ? 32 : 56 }}>
        <RadarDemo />
        <GaugeDemo />
      </View>
    </Column>
  );
}

const styles = StyleSheet.create({
  center: { alignItems: 'center', justifyContent: 'center', gap: 14, paddingVertical: 8 },
  bar: { flexDirection: 'row', alignItems: 'center', gap: 16, flexWrap: 'wrap', justifyContent: 'center' },
  picked: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute },
  stat: { fontFamily: mono, fontSize: 13, color: colors.textDim },
  missing: { width: 260, height: 200, borderRadius: 18, borderWidth: 1, borderColor: site.stroke, alignItems: 'center', justifyContent: 'center' },
});
