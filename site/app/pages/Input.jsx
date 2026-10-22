// Input: gamepad, keyboard, spatial focus, pointer / touch, and how events
// travel from the host through the C API into React.
import React from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { Card, Column, Eyebrow, Key, Pill, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { CodeBlock } from '../components/CodeBlock.jsx';
import { colors, font, mono, site, text } from '../theme.js';
import { GamepadPanel } from './input/GamepadViz.jsx';
import { KeyLog } from './input/KeyLog.jsx';
import { FocusGrid } from './input/FocusGrid.jsx';
import { DragSockets, HoverSurface } from './input/PointerLab.jsx';

function Section({ eyebrow, title, body, keys, children, style }) {
  const { phone } = useLayout();
  return (
    <View style={[{ gap: 18 }, style]}>
      <View style={{ gap: 8, maxWidth: 720 }}>
        <Eyebrow color={colors.cyan}>{eyebrow}</Eyebrow>
        <Text style={phone ? text.h3 : text.h2}>{title}</Text>
        {body ? <Text style={text.body}>{body}</Text> : null}
        {keys ? <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, alignItems: 'center', marginTop: 4 }}>{keys}</View> : null}
      </View>
      {children}
    </View>
  );
}

const FLOW = [
  { t: 'Device', d: 'DOM events, Gamepad API, or your engine’s input system', c: colors.textDim },
  { t: 'Host', d: 'web/jsui-web.js, the Godot JsuiView, your game loop', c: colors.blue },
  { t: 'C API', d: 'jsui_send_mouse_* / _touch / _key / _text / _gamepad_*', c: colors.gold },
  { t: 'InputManager', d: 'hit testing, capture, focus scopes, spatial navigation', c: colors.purple },
  { t: 'React', d: 'onPress, onKeyDown, onPointerMove, useGamepad, PanResponder', c: colors.cyan },
];

function Flow() {
  const { tablet } = useLayout();
  return (
    <View style={[styles.flow, tablet && { flexDirection: 'column' }]}>
      {FLOW.map((n, i) => (
        <React.Fragment key={n.t}>
          <View style={[styles.flowNode, tablet && { flex: 0 }]}>
            <View style={[styles.flowBar, { backgroundColor: n.c }]} />
            <Text style={[styles.flowTitle, { color: n.c === colors.textDim ? colors.text : n.c }]}>{n.t}</Text>
            <Text style={styles.flowBody}>{n.d}</Text>
          </View>
          {i < FLOW.length - 1 ? <Text style={[styles.flowArrow, tablet && { alignSelf: 'center' }]}>{tablet ? '↓' : '→'}</Text> : null}
        </React.Fragment>
      ))}
    </View>
  );
}

const C_INPUT = `
// Each frame: forward input, then tick and paint.
// jsui_send_* returns non-zero if the UI consumed it.
if (!jsui_send_mouse_button(rt, mx, my, JSUI_MOUSE_LEFT,
                            1 /*down*/, JSUI_MOD_SHIFT))
    game_click(mx, my);        // missed the UI: game input
jsui_send_mouse_move(rt, mx, my);
jsui_send_mouse_wheel(rt, mx, my, 0.0f, 40.0f);
jsui_send_touch(rt, finger, 0 /*begin*/, tx, ty);

// DOM KeyboardEvent semantics: key, code, down, repeat, mods
jsui_send_key(rt, "ArrowDown", "ArrowDown", 1, 0, 0);
jsui_send_text(rt, "a");       // printable text, after the key

// W3C standard mapping: button 0 = A, axis 0 = left stick X
jsui_set_gamepad_connected(rt, 0, 1, "Xbox Controller");
jsui_send_gamepad_button(rt, 0, 0, 1 /*down*/, 1.0f);
jsui_send_gamepad_axis(rt, 0, 0, 0.42f);

jsui_tick(rt, now_ms);
`;

const WEB_INPUT = `
// Condensed from web/jsui-web.js
canvas.addEventListener('pointerdown', (e) => {
  if (e.pointerType === 'mouse')
    M._jsui_send_mouse_button(rt, x, y, btn, 1, mods);
  else M._jsui_send_touch(rt, e.pointerId, 0, x, y);
});
sink.addEventListener('keydown', (e) => {
  M._jsui_send_key(rt, e.key, e.code, 1, e.repeat, mods);
});
// Every frame: poll navigator.getGamepads(), send changes
if (down !== s.pressed[b])
  M._jsui_send_gamepad_button(rt, i, b, down, btn.value);
if (Math.abs(v - s.axes[a]) > AXIS_EPSILON)
  M._jsui_send_gamepad_axis(rt, i, a, v);
`;

const JSX_INPUT = `
import { Pressable, useGamepad } from 'react-native';

function PauseMenu({ onResume, onClose }) {
  // Start closes the menu. preventDefault() skips
  // the runtime's default action for the event.
  useGamepad({
    onButton: (e) => {
      if (e.buttonIndex !== 9 || !e.pressed) return;
      onClose();
      e.preventDefault();
    },
  });
  return (
    <Pressable
      hasTVPreferredFocus   // first stop for D-pad
      onPress={onResume}    // click, Enter, gamepad A
      onKeyDown={(e) => e.key === 'Escape' && onClose()}
      style={({ focused }) => [s.item, focused && s.ring]}
    />
  );
}
`;

export function InputPage() {
  const { phone, tablet, content, gutter } = useLayout();
  const inner = content - gutter * 2;
  return (
    <Column style={{ paddingTop: phone ? 40 : 64, paddingBottom: 24, gap: phone ? 48 : 72 }}>
      <Reveal>
        <SectionHead
          eyebrow="Input"
          title="Every input a game has."
          lead="Gamepads, keyboards, mice and touch all arrive through the same small C API, and come out the other side as React Native events, focus moves and hooks. Everything below is live: plug in a controller, press keys, click and drag."
        />
      </Reveal>

      <Reveal delay={80}>
        <Section
          eyebrow="Gamepad"
          title="useGamepad, live"
          body="Buttons light up as the runtime receives them, sticks follow the axes and triggers fill. The D-pad and left stick also drive spatial focus everywhere on this page; A presses the focused element, B goes back."
        >
          <Card style={{ padding: phone ? 14 : 28 }}>
            <GamepadPanel phone={tablet} width={tablet ? inner - (phone ? 30 : 58) : Math.min(600, inner - 300 - 28 - 58)} />
          </Card>
        </Section>
      </Reveal>

      <View style={[styles.two, tablet && { flexDirection: 'column' }]}>
        <Section
          style={{ flex: 1 }}
          eyebrow="Keyboard"
          title="Key events with real codes"
          body="onKeyDown / onKeyUp receive DOM-style key and code, repeat and modifiers. The focused element gets them first; preventDefault() keeps arrows and Space from scrolling or moving focus."
        >
          <KeyLog compact={phone} />
        </Section>
        <Section
          style={{ flex: 1 }}
          eyebrow="Spatial focus"
          title="Arrows, D-pad, Tab"
          body="Focus moves to the nearest element in the pressed direction. Rows wrap around with nextFocusLeft / nextFocusRight overrides; the locked level is disabled, so navigation skips it."
          keys={[
            <Key key="a">←</Key>,
            <Key key="b">↑</Key>,
            <Key key="c">↓</Key>,
            <Key key="d">→</Key>,
            <Key key="e">Tab</Key>,
            <Key key="f">Enter</Key>,
            <Text key="g" style={styles.or}>or D-pad + A</Text>,
          ]}
        >
          <FocusGrid cols={phone ? 3 : 4} />
        </Section>
      </View>

      <Section
        eyebrow="Pointer & touch"
        title="Hover, press, drag"
        body="Mouse and touch share one pointer pipeline with implicit capture. onPointerMove only reaches views that ask for it; PanResponder works as in React Native, with release velocity for flings."
      >
        <View style={[styles.two, phone && { flexDirection: 'column' }]}>
          <HoverSurface height={phone ? 240 : 300} />
          <DragSockets height={phone ? 240 : 300} />
        </View>
      </Section>

      <Section
        eyebrow="Under the hood"
        title="How input flows"
        body="The host owns the devices. It forwards each event through the C API (include/jsui/jsui.h); the runtime hit-tests against the last layout, runs focus and gamepad navigation, and dispatches synchronously into React. Return values tell the game whether the UI consumed the event."
      >
        <Flow />
        <View style={[styles.two, tablet && { flexDirection: 'column', alignItems: 'stretch' }]}>
          <CodeBlock code={C_INPUT} lang="c" title="game loop · C API" style={{ flex: 1 }} />
          <View style={{ flex: 1, gap: 18 }}>
            <CodeBlock code={WEB_INPUT} lang="js" title="web/jsui-web.js" />
            <CodeBlock code={JSX_INPUT} lang="js" title="PauseMenu.jsx" />
          </View>
        </View>
        <View style={styles.pills}>
          <Pill color={colors.gold}>Default actions skip on preventDefault()</Pill>
          <Pill color={colors.cyan}>focusVisible like :focus-visible</Pill>
          <Pill color={colors.purple}>Modal focus scopes</Pill>
          <Pill color={colors.green}>D-pad repeat 400 / 120 ms</Pill>
        </View>
      </Section>
    </Column>
  );
}

const styles = StyleSheet.create({
  two: { flexDirection: 'row', gap: 28, alignItems: 'flex-start' },
  or: { fontFamily: font, fontSize: 13, color: colors.textMute, marginLeft: 4 },
  flow: { flexDirection: 'row', alignItems: 'stretch', gap: 10 },
  flowNode: {
    flex: 1,
    gap: 6,
    padding: 16,
    borderRadius: 14,
    backgroundColor: site.card,
    borderWidth: 1,
    borderColor: site.stroke,
    overflow: 'hidden',
  },
  flowBar: { position: 'absolute', left: 0, top: 0, right: 0, height: 2, opacity: 0.8 },
  flowTitle: { fontFamily: font, fontWeight: '700', fontSize: 15 },
  flowBody: { fontFamily: mono, fontSize: 11.5, lineHeight: 17, color: colors.textDim },
  flowArrow: { fontFamily: font, fontSize: 18, color: colors.textMute, alignSelf: 'center' },
  pills: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
});
