// Embed: how to ship a JSUI UI: Godot, the C API, the engine examples, the browser, dev mode and
// production builds. Everything here mirrors the real APIs in the tree
// (include/jsui/jsui.h, web/jsui-web.js, integrations/godot, integrations/raylib|sdl3|sokol, packages/cli).
import React, { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { Card, Column, Eyebrow, Pill, Reveal, SectionHead, useLayout } from '../components/ui.jsx';
import { CodeBlock } from '../components/CodeBlock.jsx';
import { colors, font, mono, site, text } from '../theme.js';
import { Segmented } from './embed/Segmented.jsx';
import { TARGETS } from './embed/content.js';

function Table({ rows, head }) {
  const { phone } = useLayout();
  return (
    <View style={styles.table}>
      {head && !phone ? (
        <View style={[styles.tr, styles.thRow]}>
          {head.map((h, i) => (
            <Text key={h} style={[styles.th, i === 0 && styles.c0, i === 1 && !phone && styles.c1, i === 1 && phone && { display: 'none' }, i === 2 && { flex: 1 }]}>
              {h}
            </Text>
          ))}
        </View>
      ) : null}
      {rows.map((r, i) => (
        <View key={r[0]} style={[styles.tr, phone && { flexDirection: 'column', alignItems: 'flex-start', gap: 4 }, i === rows.length - 1 && { borderBottomWidth: 0 }]}>
          <Text style={[styles.tdName, !phone && styles.c0]}>{r[0]}</Text>
          {r.length > 2 ? <Text style={[styles.tdType, !phone && styles.c1]}>{r[1]}</Text> : null}
          <Text style={[styles.tdDesc, !phone && { flex: 1 }]}>{r[r.length - 1]}</Text>
        </View>
      ))}
    </View>
  );
}

function Block({ block }) {
  const { tablet } = useLayout();
  switch (block.kind) {
    case 'code':
      return <CodeBlock code={block.code} lang={block.lang} title={block.title} />;
    case 'pair':
      return (
        <View style={[styles.pair, tablet && { flexDirection: 'column', alignItems: 'stretch' }]}>
          {block.items.map((b, i) => (
            <View key={i} style={tablet ? null : { flex: 1, minWidth: 0 }}>
              <Block block={b} />
            </View>
          ))}
        </View>
      );
    case 'table':
      return (
        <View style={{ gap: 10 }}>
          {block.title ? <Text style={styles.blockTitle}>{block.title}</Text> : null}
          <Table rows={block.rows} head={block.head} />
        </View>
      );
    case 'steps':
      return (
        <View style={[styles.steps, tablet && { flexDirection: 'column' }]}>
          {block.items.map((s, i) => (
            <View key={i} style={styles.step}>
              <Text style={styles.stepNum}>{String(i + 1).padStart(2, '0')}</Text>
              <Text style={text.h3}>{s[0]}</Text>
              <Text style={text.body}>{s[1]}</Text>
            </View>
          ))}
        </View>
      );
    case 'groups':
      return (
        <View style={{ gap: 14 }}>
          {block.title ? <Text style={styles.blockTitle}>{block.title}</Text> : null}
          <View style={styles.groups}>
            {block.items.map(([g, fns]) => (
              <View key={g} style={[styles.group, tablet && { width: '100%' }]}>
                <Text style={styles.groupName}>{g}</Text>
                <View style={styles.fnWrap}>
                  {fns.map((f) => (
                    <View key={f} style={styles.fn}>
                      <Text style={styles.fnText}>{f}</Text>
                    </View>
                  ))}
                </View>
              </View>
            ))}
          </View>
        </View>
      );
    case 'note':
      return (
        <View style={[styles.note, { borderColor: (block.color || colors.cyan) + '55' }]}>
          <View style={[styles.noteBar, { backgroundColor: block.color || colors.cyan }]} />
          <Text style={[text.body, { flex: 1 }]}>{block.text}</Text>
        </View>
      );
    default:
      return null;
  }
}

function TargetView({ target }) {
  const { phone } = useLayout();
  return (
    <View style={{ gap: phone ? 22 : 28 }}>
      <View style={[styles.targetHead, phone && { flexDirection: 'column', alignItems: 'flex-start' }]}>
        <View style={{ flex: 1, gap: 10, maxWidth: 760 }}>
          <Eyebrow color={target.color}>{target.eyebrow}</Eyebrow>
          <Text style={phone ? text.h2 : text.h1}>{target.title}</Text>
          <Text style={[text.lead, phone && { fontSize: 16, lineHeight: 25 }]}>{target.lead}</Text>
        </View>
        {target.facts ? (
          <View style={[styles.facts, phone && { width: '100%' }]}>
            {target.facts.map(([k, v]) => (
              <View key={k} style={styles.fact}>
                <Text style={styles.factK}>{k}</Text>
                <Text style={styles.factV}>{v}</Text>
              </View>
            ))}
          </View>
        ) : null}
      </View>
      {target.blocks.map((b, i) => (
        <Reveal key={i} delay={60 + i * 50} distance={12}>
          <Block block={b} />
        </Reveal>
      ))}
    </View>
  );
}

export function EmbedPage() {
  const { phone } = useLayout();
  const [tab, setTab] = useState(TARGETS[0].id);
  const target = TARGETS.find((t) => t.id === tab);
  return (
    <Column style={{ paddingTop: phone ? 40 : 64, paddingBottom: 24, gap: phone ? 28 : 40 }}>
      <Reveal>
        <SectionHead
          eyebrow="Embed"
          title="Ship it inside your game."
          lead="JSUI is a static library with a C API. The same runtime that draws this page runs in a Godot node, in raylib, SDL3 or sokol, in your own engine's loop, or on a canvas in the browser. Pick a target."
        />
      </Reveal>
      <Reveal delay={60}>
        <View style={styles.badges}>
          <Pill color={colors.gold}>C API · include/jsui/jsui.h</Pill>
          <Pill color={colors.cyan}>Godot 4.3+ GDExtension</Pill>
          <Pill color={colors.orange}>raylib · SDL3 · sokol examples</Pill>
          <Pill color={colors.purple}>WebAssembly · CPU or WebGL</Pill>
          <Pill color={colors.green}>Live reload over HTTP</Pill>
        </View>
      </Reveal>
      <Reveal delay={100} style={styles.tabsWrap}>
        <Segmented items={TARGETS} value={tab} onChange={setTab} scrollable={phone} />
      </Reveal>
      <Card style={{ padding: phone ? 18 : 36, gap: 0 }}>
        <Reveal key={tab} distance={10}>
          <TargetView target={target} />
        </Reveal>
      </Card>
    </Column>
  );
}

const styles = StyleSheet.create({
  badges: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  tabsWrap: { marginBottom: -12 },
  targetHead: { flexDirection: 'row', gap: 28, alignItems: 'flex-end' },
  facts: { width: 280, gap: 1, borderRadius: 12, overflow: 'hidden', borderWidth: 1, borderColor: site.stroke },
  fact: { flexDirection: 'row', justifyContent: 'space-between', gap: 12, paddingHorizontal: 14, paddingVertical: 10, backgroundColor: 'rgba(10,15,29,0.7)' },
  factK: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textMute },
  factV: { fontFamily: mono, fontSize: 12, color: colors.text, flexShrink: 1, textAlign: 'right' },
  pair: { flexDirection: 'row', gap: 18, alignItems: 'flex-start' },
  blockTitle: { fontFamily: font, fontWeight: '700', fontSize: 12, letterSpacing: 2, textTransform: 'uppercase', color: colors.textMute },
  table: { borderRadius: 12, borderWidth: 1, borderColor: site.stroke, overflow: 'hidden', backgroundColor: 'rgba(10,15,29,0.6)' },
  tr: { flexDirection: 'row', alignItems: 'center', gap: 14, paddingHorizontal: 16, paddingVertical: 11, borderBottomWidth: 1, borderBottomColor: site.stroke },
  thRow: { backgroundColor: 'rgba(255,255,255,0.02)', paddingVertical: 9 },
  th: { fontFamily: font, fontWeight: '700', fontSize: 10, letterSpacing: 1.6, color: colors.textMute },
  c0: { width: 210 },
  c1: { width: 110 },
  tdName: { fontFamily: mono, fontSize: 12.5, color: colors.gold },
  tdType: { fontFamily: mono, fontSize: 12, color: colors.cyan },
  tdDesc: { fontFamily: font, fontSize: 14, lineHeight: 21, color: colors.textDim },
  steps: { flexDirection: 'row', gap: 16 },
  step: { flex: 1, gap: 8, padding: 18, borderRadius: 14, backgroundColor: 'rgba(10,15,29,0.6)', borderWidth: 1, borderColor: site.stroke },
  stepNum: { fontFamily: mono, fontWeight: '700', fontSize: 12, color: colors.gold },
  groups: { flexDirection: 'row', flexWrap: 'wrap', gap: 12 },
  group: { width: '48.8%', flexGrow: 1, gap: 10, padding: 16, borderRadius: 12, backgroundColor: 'rgba(10,15,29,0.6)', borderWidth: 1, borderColor: site.stroke },
  groupName: { fontFamily: font, fontWeight: '700', fontSize: 13, color: colors.text },
  fnWrap: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  fn: { paddingHorizontal: 8, paddingVertical: 4, borderRadius: 6, backgroundColor: 'rgba(148,170,255,0.08)' },
  fnText: { fontFamily: mono, fontSize: 11.5, color: '#c3cdf0' },
  note: { flexDirection: 'row', gap: 14, padding: 16, borderRadius: 12, borderWidth: 1, backgroundColor: 'rgba(10,15,29,0.6)', overflow: 'hidden' },
  noteBar: { width: 3, borderRadius: 2, alignSelf: 'stretch' },
});
