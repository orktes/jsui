// Playground: edit React Native code, compiled by Sucrase inside the JSUI
// runtime (QuickJS) and rendered live next to the editor.
import React, { Component, memo, useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, Text, View } from 'react-native';
import { Button, Card, Column, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { colors, font, mono, site, text } from '../theme.js';
import { Segmented } from './embed/Segmented.jsx';
import { Editor } from './playground/Editor.jsx';
import { EXAMPLES } from './playground/examples.js';
import { compile } from './playground/compile.js';

const DEBOUNCE = 320;

// Remembers edits per example while the site runs (switching tabs keeps them).
const drafts = {};

class Boundary extends Component {
  constructor(props) {
    super(props);
    this.state = { error: null };
  }
  static getDerivedStateFromError(error) {
    return { error };
  }
  componentDidCatch(error) {
    if (this.props.onError) this.props.onError(error);
  }
  render() {
    if (this.state.error) return null;
    return this.props.children;
  }
}

function LiveDot({ color }) {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(t, { toValue: 1, duration: 900, easing: Easing.out(Easing.quad), useNativeDriver: true }),
        Animated.timing(t, { toValue: 0, duration: 0, useNativeDriver: true }),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, []);
  return (
    <View style={{ width: 10, height: 10, alignItems: 'center', justifyContent: 'center' }}>
      <Animated.View
        style={{
          position: 'absolute',
          width: 10,
          height: 10,
          borderRadius: 5,
          backgroundColor: color,
          opacity: t.interpolate({ inputRange: [0, 1], outputRange: [0.6, 0] }),
          transform: [{ scale: t.interpolate({ inputRange: [0, 1], outputRange: [1, 2.6] }) }],
        }}
      />
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: color }} />
    </View>
  );
}

function Status({ state }) {
  let color = colors.green;
  let label = '';
  if (state.pending) {
    color = colors.cyan;
    label = 'Compiling…';
  } else if (state.error) {
    color = colors.red;
    label = state.error.kind === 'compile' ? 'Syntax error' + (state.error.line ? ` · line ${state.error.line}` : '') : 'Runtime error';
  } else {
    label = state.ms < 1 ? 'Compiled in <1 ms' : `Compiled in ${Math.round(state.ms)} ms`;
  }
  return (
    <View style={[styles.status, { borderColor: color + '55', backgroundColor: color + '12' }]}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: color }} />
      <Text style={[styles.statusText, { color }]}>{label}</Text>
    </View>
  );
}

function ErrorPanel({ error, code }) {
  const t = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    t.setValue(0);
    Animated.spring(t, { toValue: 1, speed: 16, bounciness: 6, useNativeDriver: true }).start();
  }, [error.message]);
  const srcLine = error.line ? code.split('\n')[error.line - 1] : null;
  return (
    <Animated.View
      style={[
        styles.errPanel,
        { opacity: t, transform: [{ translateY: t.interpolate({ inputRange: [0, 1], outputRange: [24, 0] }) }] },
      ]}
    >
      <Text style={styles.errKind}>{error.kind === 'compile' ? 'COMPILE ERROR' : 'RUNTIME ERROR'}</Text>
      <Text style={styles.errMsg} numberOfLines={4}>
        {error.message}
      </Text>
      {srcLine != null ? (
        <View style={styles.errFrame}>
          <Text style={styles.errCode} numberOfLines={1}>
            <Text style={{ color: '#5c6a8f' }}>{String(error.line).padStart(3, ' ')} │ </Text>
            {srcLine}
          </Text>
          {error.column ? (
            <Text style={styles.errCode}>
              <Text style={{ color: '#5c6a8f' }}>{'    │ '}</Text>
              <Text style={{ color: colors.red }}>{' '.repeat(Math.max(0, error.column - 1)) + '^'}</Text>
            </Text>
          ) : null}
        </View>
      ) : null}
      <Text style={styles.errHint}>The preview recovers as soon as the code compiles and renders.</Text>
    </Animated.View>
  );
}

function Mounted({ onMount }) {
  useEffect(() => onMount(), []);
  return null;
}

const Preview = memo(function Preview({ Comp, version, onError, onMount }) {
  if (!Comp) return null;
  return (
    <Boundary key={version} onError={onError}>
      <Comp />
      <Mounted onMount={() => onMount(Comp)} />
    </Boundary>
  );
});

export function PlaygroundPage() {
  const { phone, tablet } = useLayout();
  const [exampleId, setExampleId] = useState(EXAMPLES[0].id);
  const example = EXAMPLES.find((e) => e.id === exampleId);
  const [code, setCode] = useState(() => drafts[exampleId] || example.code);
  const [state, setState] = useState(() => {
    const r = compile(drafts[exampleId] || example.code);
    return { Comp: r.ok ? r.Component : null, version: 1, ms: r.ms, error: r.ok ? null : r, pending: false };
  });
  const editor = useRef(null);
  const refocus = useRef(false);
  const first = useRef(true);

  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    drafts[exampleId] = code;
    setState((s) => (s.pending ? s : { ...s, pending: true }));
    const id = setTimeout(() => {
      const r = compile(code);
      refocus.current = !!(editor.current && editor.current.isFocused());
      setState((s) =>
        r.ok
          ? { Comp: r.Component, version: s.version + 1, ms: r.ms, error: null, pending: false }
          : { ...s, ms: r.ms, error: r, pending: false },
      );
    }, DEBOUNCE);
    return () => clearTimeout(id);
  }, [code]);

  // Code in the preview may grab focus (autoFocus): keep typing in the editor.
  useEffect(() => {
    if (!refocus.current) return;
    refocus.current = false;
    const id = setTimeout(() => {
      if (editor.current && !editor.current.isFocused()) editor.current.focus();
    }, 0);
    return () => clearTimeout(id);
  }, [state.version]);

  const pick = (id) => {
    if (id === exampleId) return;
    good.current = null;
    setExampleId(id);
    setCode(drafts[id] || EXAMPLES.find((e) => e.id === id).code);
  };
  const reset = () => {
    delete drafts[exampleId];
    setCode(example.code);
  };
  // A component that rendered once is "good": a later render error falls
  // back to it (dimmed, under the error panel) instead of an empty stage.
  const good = useRef(null);
  const onMount = (Comp) => {
    good.current = Comp;
  };
  const onRuntimeError = (e) => {
    const error = { kind: 'runtime', message: String((e && e.message) || e) };
    setState((s) =>
      good.current && good.current !== s.Comp
        ? { ...s, Comp: good.current, version: s.version + 1, error }
        : { ...s, error },
    );
  };

  const paneH = phone ? 440 : 620;
  const lines = code.split('\n').length;
  const dirty = code !== example.code;

  return (
    <Column style={{ paddingTop: phone ? 40 : 64, paddingBottom: 24, gap: phone ? 22 : 30 }}>
      <Reveal>
        <SectionHead
          eyebrow="Playground"
          title="Write React Native. Watch it run."
          lead="Your code is compiled by Sucrase running inside the JSUI runtime itself (QuickJS, in WebAssembly), then rendered by the same Yoga + Skia pipeline that draws this page. No server, no iframe, no DOM."
        />
      </Reveal>

      <Reveal delay={80} style={{ gap: 14 }}>
        <View style={[styles.toolbar, phone && { flexDirection: 'column', alignItems: 'stretch' }]}>
          <Segmented items={EXAMPLES} value={exampleId} onChange={pick} scrollable={phone} />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Status state={state} />
            <Button label="Reset" kind="ghost" small onPress={reset} style={!dirty && { opacity: 0.5 }} />
          </View>
        </View>

        <View style={[styles.workspace, tablet && { flexDirection: 'column' }]}>
          <View style={[styles.pane, tablet ? { height: paneH } : { flex: 1.15, height: paneH }]}>
            <View style={styles.paneBar}>
              <View style={styles.fileTab}>
                <View style={styles.fileIcon}><Text style={styles.fileIconText}>JSX</Text></View>
                <Text style={styles.fileName}>App.jsx</Text>
                {dirty ? <View style={styles.dirtyDot} /> : null}
              </View>
              <View style={{ flex: 1 }} />
              <Text style={styles.paneMeta}>{lines} lines · JSX</Text>
            </View>
            <Editor ref={editor} code={code} onChange={setCode} height={paneH - 44} />
          </View>

          <View style={[styles.pane, styles.previewPane, tablet ? { height: phone ? 420 : 520 } : { flex: 1, height: paneH }]}>
            <View style={styles.paneBar}>
              <LiveDot color={state.error ? colors.red : colors.green} />
              <Text style={styles.paneTitle}>Preview</Text>
              <View style={{ flex: 1 }} />
              <Text style={styles.paneMeta}>QuickJS · Yoga · Skia</Text>
            </View>
            <View style={styles.stage}>
              <View pointerEvents="none" style={styles.grid} />
              <View style={[styles.stageInner, state.error && { opacity: 0.3 }]}>
                <Preview Comp={state.Comp} version={state.version} onError={onRuntimeError} onMount={onMount} />
              </View>
              {state.error ? <ErrorPanel error={state.error} code={code} /> : null}
            </View>
          </View>
        </View>
      </Reveal>

      <Reveal delay={160}>
        <View style={[styles.notes, tablet && { flexDirection: 'column' }]}>
          <Note
            step="01"
            title="Compiled in the runtime"
            body="Sucrase's JSX and imports transforms are bundled into the site (about 200 KB minified) and run on QuickJS. A typical example compiles in a few milliseconds."
          />
          <Note
            step="02"
            title="Real modules"
            body="The output runs through new Function with a require() that returns this page's own React and react-native, so everything on this site is available: Animated, Pressable, useGamepad, TextInput…"
          />
          <Note
            step="03"
            title="Errors stay contained"
            body="Syntax errors point at the line and column. Render errors are caught by an error boundary and the last good preview stays on screen until the code works again."
          />
        </View>
      </Reveal>
    </Column>
  );
}

function Note({ step, title, body }) {
  return (
    <Card style={{ flex: 1, gap: 10, padding: 22 }}>
      <Text style={styles.noteStep}>{step}</Text>
      <Text style={text.h3}>{title}</Text>
      <Text style={text.body}>{body}</Text>
    </Card>
  );
}

const styles = StyleSheet.create({
  toolbar: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 14, flexWrap: 'wrap' },
  status: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 36, paddingHorizontal: 12, borderRadius: 10, borderWidth: 1 },
  statusText: { fontFamily: font, fontWeight: '600', fontSize: 13 },
  workspace: { flexDirection: 'row', gap: 18 },
  pane: {
    backgroundColor: site.code,
    borderRadius: 16,
    borderWidth: 1,
    borderColor: site.stroke,
    overflow: 'hidden',
    boxShadow: '0 30px 80px rgba(0,0,0,0.5), inset 0 1px 0 rgba(255,255,255,0.04)',
  },
  previewPane: { backgroundColor: '#070a14' },
  paneBar: {
    height: 44,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    paddingHorizontal: 14,
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
    backgroundColor: 'rgba(255,255,255,0.02)',
  },
  fileTab: { flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 12, borderRadius: 8, backgroundColor: 'rgba(148,170,255,0.08)' },
  fileIcon: { paddingHorizontal: 4, height: 16, borderRadius: 4, justifyContent: 'center', backgroundColor: 'rgba(79,209,255,0.16)' },
  fileIconText: { fontFamily: mono, fontWeight: '700', fontSize: 9, color: colors.cyan },
  fileName: { fontFamily: mono, fontSize: 12, color: colors.text },
  dirtyDot: { width: 6, height: 6, borderRadius: 3, backgroundColor: colors.gold },
  paneTitle: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },
  paneMeta: { fontFamily: mono, fontSize: 11, color: colors.textMute },
  stage: { flex: 1, overflow: 'hidden' },
  grid: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'radial-gradient(circle at 50% 40%, rgba(79,140,255,0.14) 0%, rgba(7,10,20,0) 60%), linear-gradient(180deg, rgba(13,19,34,0.6) 0%, rgba(7,10,20,1) 100%)',
  },
  stageInner: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 20 },
  errPanel: {
    position: 'absolute',
    left: 14,
    right: 14,
    bottom: 14,
    padding: 16,
    gap: 8,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: 'rgba(255, 77, 106, 0.55)',
    backgroundColor: 'rgba(40, 10, 18, 0.94)',
    boxShadow: '0 20px 50px rgba(0,0,0,0.6)',
  },
  errKind: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, color: colors.red },
  errMsg: { fontFamily: mono, fontSize: 13, lineHeight: 19, color: '#ffdcd7' },
  errFrame: { paddingVertical: 8, paddingHorizontal: 10, borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.35)' },
  errCode: { fontFamily: mono, fontSize: 12, lineHeight: 18, color: '#d6def5' },
  errHint: { fontFamily: font, fontSize: 12, color: '#c99aa3' },
  notes: { flexDirection: 'row', gap: 18 },
  noteStep: { fontFamily: mono, fontWeight: '700', fontSize: 12, color: colors.gold, letterSpacing: 1 },
});
