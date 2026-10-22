// Code editor: a multiline TextInput with transparent text laid exactly over
// syntax-highlighted rows (one memoized <Text> per source line, so a
// keystroke re-renders one row). Both use the same font, size, line height
// and width, so the native caret / selection line up with the coloured text
// and wrapped lines wrap identically. Tab indents, Enter keeps indentation,
// and the surrounding ScrollView follows the caret.
import React, { forwardRef, memo, useCallback, useEffect, useImperativeHandle, useMemo, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, TextInput, View } from 'react-native';
import { mono } from '../../theme.js';
import { PALETTE, highlightLines, runsKey } from './highlight.js';

const FONT_SIZE = 13;
const LINE = 21;
const GUTTER = 46;
const PAD_Y = 14;
const PAD_R = 18;

const Row = memo(
  function Row({ n, runs, active, onRowLayout }) {
    return (
      <View style={[styles.row, active && styles.rowActive]} onLayout={(e) => onRowLayout(n, e.nativeEvent.layout)}>
        <Text style={[styles.num, active && styles.numActive]}>{n + 1}</Text>
        <Text style={styles.code}>
          {runs.length ? runs.map(([k, s], i) => (
            <Text key={i} style={{ color: PALETTE[k] }}>
              {s}
            </Text>
          )) : ' '}
        </Text>
      </View>
    );
  },
  (a, b) => a.n === b.n && a.active === b.active && a.k === b.k,
);

const lineOf = (code, index) => {
  let n = 0;
  for (let i = 0; i < index && i < code.length; i++) if (code.charCodeAt(i) === 10) n++;
  return n;
};

export const Editor = forwardRef(function Editor({ code, onChange, height }, ref) {
  const input = useRef(null);
  const scroll = useRef(null);
  const sel = useRef({ start: 0, end: 0 });
  const pendingSel = useRef(null);
  const rowsY = useRef([]);
  const scrollY = useRef(0);
  const viewH = useRef(height);
  const [caretLine, setCaretLine] = useState(-1);
  const [focused, setFocused] = useState(false);

  useImperativeHandle(ref, () => ({
    focus: () => input.current && input.current.focus(),
    isFocused: () => !!(input.current && input.current.isFocused && input.current.isFocused()),
  }));

  const lines = useMemo(() => highlightLines(code), [code]);

  const onRowLayout = useCallback((n, l) => {
    rowsY.current[n] = { y: l.y, h: l.height };
  }, []);

  const follow = (line) => {
    const r = rowsY.current[line];
    if (!r || !scroll.current) return;
    const top = scrollY.current;
    const vh = viewH.current;
    if (r.y - LINE < top) scroll.current.scrollTo({ y: Math.max(0, r.y - LINE * 2), animated: false });
    else if (r.y + r.h + LINE > top + vh) scroll.current.scrollTo({ y: r.y + r.h + LINE * 2 - vh, animated: false });
  };

  // Apply a selection computed by Tab / Enter once the new value is native.
  useEffect(() => {
    const p = pendingSel.current;
    if (p != null && input.current) {
      pendingSel.current = null;
      input.current.setSelection(p, p);
      sel.current = { start: p, end: p };
      const l = lineOf(code, p);
      setCaretLine(l);
      setTimeout(() => follow(l), 0);
    }
  }, [code]);

  const replace = (text, start, end, caret) => {
    const next = code.slice(0, start) + text + code.slice(end);
    pendingSel.current = caret;
    onChange(next);
  };

  const onKeyDown = (e) => {
    const { start, end } = sel.current;
    if (e.key === 'Tab' && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      if (e.shiftKey) {
        // Outdent the caret's line by up to two spaces.
        const ls = code.lastIndexOf('\n', start - 1) + 1;
        const m = /^ {1,2}/.exec(code.slice(ls));
        if (m) replace('', ls, ls + m[0].length, Math.max(ls, start - m[0].length));
        return;
      }
      replace('  ', start, end, start + 2);
    } else if (e.key === 'Enter' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
      e.preventDefault();
      const ls = code.lastIndexOf('\n', start - 1) + 1;
      const indent = /^ */.exec(code.slice(ls))[0];
      const before = code.slice(ls, start).trimEnd();
      const after = code.slice(end).replace(/^ +/, '');
      const opens = /[{([]$/.test(before);
      const closes = /^[})\]]/.test(after);
      if (opens && closes) {
        const inner = '\n' + indent + '  ';
        replace(inner + '\n' + indent, start, end, start + inner.length);
      } else {
        const ins = '\n' + indent + (opens ? '  ' : '');
        replace(ins, start, end, start + ins.length);
      }
    }
  };

  return (
    <ScrollView
      ref={scroll}
      style={{ height }}
      onLayout={(e) => (viewH.current = e.nativeEvent.layout.height)}
      onScroll={(e) => (scrollY.current = e.nativeEvent.contentOffset.y)}
      scrollEventThrottle={16}
      contentContainerStyle={styles.content}
    >
      <View style={styles.rows}>
        {lines.map((runs, i) => (
          <Row key={i} n={i} runs={runs} k={runsKey(runs)} active={focused && i === caretLine} onRowLayout={onRowLayout} />
        ))}
      </View>
      <TextInput
        ref={input}
        value={code}
        multiline
        onChangeText={onChange}
        onKeyDown={onKeyDown}
        onFocus={() => setFocused(true)}
        onBlur={() => setFocused(false)}
        onSelectionChange={(e) => {
          const s = e.nativeEvent.selection;
          sel.current = s;
          const l = lineOf(code, s.end);
          if (l !== caretLine) setCaretLine(l);
          follow(l);
        }}
        autoCapitalize="none"
        autoCorrect={false}
        spellCheck={false}
        cursorColor="#ffcb6b"
        selectionColor="rgba(79, 209, 255, 0.28)"
        style={styles.input}
      />
    </ScrollView>
  );
});

const styles = StyleSheet.create({
  content: { minHeight: '100%' },
  rows: { paddingVertical: PAD_Y },
  row: { flexDirection: 'row', minHeight: LINE },
  rowActive: { backgroundColor: 'rgba(148, 170, 255, 0.06)' },
  num: { width: GUTTER, paddingRight: 14, textAlign: 'right', fontFamily: mono, fontSize: 12, lineHeight: LINE, color: '#3a4566' },
  numActive: { color: '#8a96b8' },
  code: { flex: 1, paddingRight: PAD_R, fontFamily: mono, fontSize: FONT_SIZE, lineHeight: LINE, color: PALETTE.plain },
  input: {
    position: 'absolute',
    left: GUTTER,
    right: 0,
    top: 0,
    bottom: 0,
    paddingTop: PAD_Y,
    paddingBottom: PAD_Y,
    paddingLeft: 0,
    paddingRight: PAD_R,
    fontFamily: mono,
    fontSize: FONT_SIZE,
    lineHeight: LINE,
    color: 'transparent',
    textAlignVertical: 'top',
    borderWidth: 0,
    backgroundColor: 'transparent',
  },
});
