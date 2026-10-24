// Syntax-highlighted code, rendered as nested <Text> runs.
import React, { memo, useMemo, useState } from 'react';
import { Clipboard, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { colors, font, mono, site } from '../theme.js';

const KEYWORDS = {
  js: 'import export from default function return const let var if else for while of in new class extends async await try catch finally throw typeof instanceof null undefined true false this switch case break continue',
  c: 'int float double char void const static struct typedef enum return if else for while unsigned size_t uint32_t uint8_t bool true false NULL sizeof include define',
  gd: 'extends func var const if elif else for while return signal class_name and or not in true false null self await pass match',
  sh: 'npx npm node cmake git cd export',
  cs: 'var void int float double bool string new return if else for foreach while using public private static class struct readonly true false null this switch case break continue out ref',
};

const PALETTE = {
  kw: '#c792ea',
  str: '#c3e88d',
  num: '#f78c6c',
  com: '#5c6a8f',
  fn: '#82aaff',
  tag: '#ffcb6b',
  attr: '#89ddff',
  punct: '#8a96b8',
  plain: '#d6def5',
};

const TOKEN = /(\/\/[^\n]*|#(?!include|define)[^\n]*|\/\*[\s\S]*?\*\/)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|`(?:[^`\\]|\\.)*`)|(\b\d+(?:\.\d+)?\b)|(<\/?[A-Za-z][\w.]*|\/?>)|([A-Za-z_$][\w$]*)(?=\s*\()|([A-Za-z_$#][\w$]*)|([{}()[\];,.=+\-*/<>!&|?:%^~@])/g;

function tokenize(code, lang) {
  const kw = new Set((KEYWORDS[lang] || KEYWORDS.js).split(' '));
  const out = [];
  let last = 0;
  let m;
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(code))) {
    if (m.index > last) out.push(['plain', code.slice(last, m.index)]);
    const [s, com, str, num, tag, fn, word] = m;
    let kind = 'punct';
    if (com) kind = lang === 'js' && com[0] === '#' ? 'plain' : lang === 'c' && com[0] === '#' ? 'kw' : 'com';
    else if (str) kind = 'str';
    else if (num) kind = 'num';
    else if (tag) kind = lang === 'js' ? 'tag' : 'punct';
    else if (fn) kind = kw.has(fn) ? 'kw' : 'fn';
    else if (word) kind = kw.has(word) ? 'kw' : /^[A-Z]/.test(word) ? 'tag' : 'plain';
    out.push([kind, s]);
    last = m.index + s.length;
  }
  if (last < code.length) out.push(['plain', code.slice(last)]);
  return out;
}

/** Highlighted runs for `code` (exported for the playground editor overlay). */
export function Highlighted({ code, lang = 'js', style }) {
  const runs = useMemo(() => tokenize(code, lang), [code, lang]);
  return (
    <Text style={[styles.code, style]}>
      {runs.map(([k, s], i) => (
        <Text key={i} style={{ color: PALETTE[k] }}>
          {s}
        </Text>
      ))}
    </Text>
  );
}

export const CodeBlock = memo(function CodeBlock({ code, lang = 'js', title, style, maxHeight }) {
  const [copied, setCopied] = useState(false);
  const body = code.replace(/^\n+|\s+$/g, '');
  return (
    <View style={[styles.wrap, style]}>
      <View style={styles.bar}>
        <View style={{ flexDirection: 'row', gap: 6 }}>
          <View style={[styles.dot, { backgroundColor: '#ff5f57' }]} />
          <View style={[styles.dot, { backgroundColor: '#febc2e' }]} />
          <View style={[styles.dot, { backgroundColor: '#28c840' }]} />
        </View>
        <Text style={styles.title} numberOfLines={1}>
          {title || ''}
        </Text>
        <Pressable
          onPress={() => {
            Clipboard.setString(body);
            setCopied(true);
            setTimeout(() => setCopied(false), 1400);
          }}
          style={({ hovered }) => [styles.copy, hovered && { backgroundColor: 'rgba(255,255,255,0.08)' }]}
        >
          <Text style={styles.copyText}>{copied ? 'Copied' : 'Copy'}</Text>
        </Pressable>
      </View>
      <ScrollView horizontal style={maxHeight ? { maxHeight } : null} contentContainerStyle={styles.body} showsHorizontalScrollIndicator>
        <Highlighted code={body} lang={lang} />
      </ScrollView>
    </View>
  );
});

const styles = StyleSheet.create({
  wrap: { backgroundColor: site.code, borderRadius: 14, borderWidth: 1, borderColor: site.stroke, overflow: 'hidden' },
  bar: {
    height: 38,
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
    paddingHorizontal: 14,
    borderBottomWidth: 1,
    borderBottomColor: site.stroke,
    backgroundColor: 'rgba(255,255,255,0.02)',
  },
  dot: { width: 10, height: 10, borderRadius: 5, opacity: 0.85 },
  title: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  copy: { paddingHorizontal: 10, height: 24, borderRadius: 6, justifyContent: 'center' },
  copyText: { fontFamily: font, fontWeight: '600', fontSize: 12, color: colors.textDim },
  body: { padding: 16, paddingRight: 28 },
  code: { fontFamily: mono, fontSize: 13, lineHeight: 21, color: PALETTE.plain },
});
