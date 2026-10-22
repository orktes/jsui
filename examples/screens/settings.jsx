// Settings: category rail, rows with dropdowns, segmented controls, toggles
// and sliders drawn with Views, an open dropdown and a help panel.
import React from 'react';
import { Image, StyleSheet, Text, View } from 'react-native';
import { colors, font } from '../theme.js';

export const meta = { width: 1280, height: 720, maxMismatch: 1.5, title: 'Settings' };

const CATEGORIES = ['Gameplay', 'Graphics', 'Audio', 'Controls', 'Accessibility', 'Account'];

function Row({ label, sub, focused, children }) {
  return (
    <View style={[styles.row, focused && styles.rowFocused]}>
      <View style={{ flex: 1 }}>
        <Text style={[styles.rowLabel, focused && { color: '#fff' }]}>{label}</Text>
        {sub ? <Text style={styles.rowSub}>{sub}</Text> : null}
      </View>
      <View style={styles.control}>{children}</View>
    </View>
  );
}

function Toggle({ on }) {
  return (
    <View style={[styles.toggle, on && styles.toggleOn]}>
      <View style={[styles.knob, on && styles.knobOn]} />
    </View>
  );
}

function Slider({ value, label, accent = colors.cyan }) {
  return (
    <View style={styles.sliderWrap}>
      <View style={styles.sliderTrack}>
        <View style={[styles.sliderFill, { width: `${value * 100}%`, backgroundColor: accent }]} />
        <View style={[styles.thumb, { left: `${value * 100}%`, borderColor: accent }]} />
      </View>
      <Text style={styles.sliderValue}>{label}</Text>
    </View>
  );
}

function Segmented({ options, active }) {
  return (
    <View style={styles.segmented}>
      {options.map((o, i) => (
        <View key={o} style={[styles.segment, i === active && styles.segmentActive]}>
          <Text style={[styles.segmentText, i === active && styles.segmentTextActive]}>{o}</Text>
        </View>
      ))}
    </View>
  );
}

function Dropdown({ value, open }) {
  return (
    <View style={[styles.dropdown, open && styles.dropdownOpen]}>
      <Text style={styles.dropdownText}>{value}</Text>
      <View style={[styles.caret, open && { transform: [{ rotate: '225deg' }], marginTop: 4 }]} />
    </View>
  );
}

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.rail}>
        <Text style={styles.railTitle}>Settings</Text>
        {CATEGORIES.map((c, i) => (
          <View key={c} style={[styles.cat, i === 1 && styles.catActive]}>
            <View style={[styles.catIcon, i === 1 && { backgroundColor: colors.cyan, transform: [{ rotate: '45deg' }] }]} />
            <Text style={[styles.catText, i === 1 && styles.catTextActive]}>{c}</Text>
            {i === 2 ? <View style={styles.dot} /> : null}
          </View>
        ))}
        <View style={{ flex: 1 }} />
        <Text style={styles.railFoot}>Profile: Valkyrie_07</Text>
      </View>

      <View style={styles.content}>
        <View style={styles.contentHeader}>
          <Text style={styles.h1}>Graphics</Text>
          <Segmented options={['Low', 'Medium', 'High', 'Ultra', 'Custom']} active={4} />
        </View>

        <Text style={styles.section}>Display</Text>
        <Row label="Resolution" sub="Native 2560 × 1440">
          <Dropdown value="2560 × 1440 (16:9)" />
        </Row>
        <Row label="Window Mode">
          <Segmented options={['Fullscreen', 'Borderless', 'Windowed']} active={1} />
        </Row>
        <Row label="Vertical Sync">
          <Toggle on />
        </Row>
        <Row label="Frame Rate Limit" focused>
          <Slider value={0.58} label="144 FPS" accent={colors.gold} />
        </Row>
        <Row label="Field of View">
          <Slider value={0.4} label="90°" />
        </Row>

        <Text style={styles.section}>Quality</Text>
        <Row label="Shadow Quality">
          <Dropdown value="High" open />
        </Row>
        <Row label="Motion Blur">
          <Toggle />
        </Row>
        <Row label="Ray-Traced Reflections" sub="Requires a compatible GPU">
          <Toggle on />
        </Row>

        {/* Open dropdown menu floats above the following rows. */}
        <View style={styles.menu}>
          {['Off', 'Low', 'Medium', 'High', 'Epic'].map((o) => (
            <View key={o} style={[styles.menuItem, o === 'High' && styles.menuItemActive]}>
              <Text style={[styles.menuText, o === 'High' && { color: '#fff' }]}>{o}</Text>
              {o === 'High' ? <View style={styles.checkMark} /> : null}
            </View>
          ))}
        </View>
      </View>

      <View style={styles.help}>
        <Image source={require('../assets/card-tide.png')} style={styles.preview} resizeMode="cover" />
        <View style={styles.helpBody}>
          <Text style={styles.helpTitle}>Frame Rate Limit</Text>
          <Text style={styles.helpText}>
            Caps the maximum frames rendered per second. Matching your display's refresh rate reduces power draw and keeps frame pacing smooth.
          </Text>
          <Text style={styles.impactLabel}>Performance impact</Text>
          <View style={styles.impact}>
            {[0, 1, 2, 3, 4].map((i) => (
              <View key={i} style={[styles.impactSeg, i < 2 && { backgroundColor: colors.green }]} />
            ))}
          </View>
          <View style={styles.statLine}>
            <Text style={styles.statKey}>GPU estimate</Text>
            <Text style={styles.statVal}>6.2 ms</Text>
          </View>
          <View style={styles.statLine}>
            <Text style={styles.statKey}>VRAM</Text>
            <Text style={styles.statVal}>5.1 / 12 GB</Text>
          </View>
        </View>
      </View>

      <View style={styles.footer}>
        <View style={[styles.btn, styles.btnGhost]}>
          <Text style={styles.btnGhostText}>Reset to Defaults</Text>
        </View>
        <View style={[styles.btn, styles.btnPrimary]}>
          <Text style={styles.btnPrimaryText}>Apply Changes</Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, flexDirection: 'row', backgroundColor: '#080c18', experimental_backgroundImage: 'linear-gradient(135deg, #0c1428 0%, #070a14 60%)' },

  rail: { width: 240, paddingTop: 36, paddingBottom: 28, paddingHorizontal: 18, backgroundColor: 'rgba(0,0,0,0.25)', borderRightWidth: 1, borderRightColor: colors.line },
  railTitle: { fontFamily: font, fontWeight: '700', fontSize: 13, letterSpacing: 3, textTransform: 'uppercase', color: colors.textMute, marginLeft: 14, marginBottom: 18 },
  cat: { flexDirection: 'row', alignItems: 'center', gap: 12, height: 44, paddingHorizontal: 14, borderRadius: 10, marginBottom: 4 },
  catActive: {
    backgroundColor: 'rgba(79,209,255,0.10)',
    borderWidth: 1,
    borderColor: 'rgba(79,209,255,0.35)',
    boxShadow: 'inset 3px 0 0 #4fd1ff',
  },
  catIcon: { width: 10, height: 10, borderRadius: 2, backgroundColor: colors.textMute },
  catText: { flex: 1, fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.textDim },
  catTextActive: { color: '#fff', fontWeight: '600' },
  dot: { width: 7, height: 7, borderRadius: 4, backgroundColor: colors.orange },
  railFoot: { fontFamily: font, fontSize: 12, color: colors.textMute, marginLeft: 14 },

  content: { width: 640, paddingTop: 30, paddingHorizontal: 32 },
  contentHeader: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 },
  h1: { fontFamily: font, fontWeight: '700', fontSize: 30, color: colors.text },
  section: { fontFamily: font, fontWeight: '700', fontSize: 11, letterSpacing: 2, textTransform: 'uppercase', color: colors.cyan, marginTop: 18, marginBottom: 6 },
  row: { flexDirection: 'row', alignItems: 'center', minHeight: 50, paddingHorizontal: 14, borderRadius: 8, borderBottomWidth: 1, borderBottomColor: 'rgba(148,170,255,0.07)' },
  rowFocused: {
    backgroundColor: 'rgba(242,195,91,0.08)',
    borderWidth: 1,
    borderColor: 'rgba(242,195,91,0.5)',
    boxShadow: '0 0 20px rgba(242,195,91,0.15)',
  },
  rowLabel: { fontFamily: font, fontWeight: '500', fontSize: 15, color: colors.text },
  rowSub: { fontFamily: font, fontSize: 12, color: colors.textMute, marginTop: 2 },
  control: { width: 290, alignItems: 'flex-end' },

  toggle: { width: 46, height: 26, borderRadius: 13, padding: 3, backgroundColor: 'rgba(255,255,255,0.10)', borderWidth: 1, borderColor: 'rgba(255,255,255,0.12)' },
  toggleOn: { backgroundColor: colors.cyanDeep, borderColor: colors.cyan, boxShadow: '0 0 12px rgba(79,209,255,0.45)' },
  knob: { width: 18, height: 18, borderRadius: 9, backgroundColor: '#8a93ad', boxShadow: '0 1px 3px rgba(0,0,0,0.5)' },
  knobOn: { backgroundColor: '#fff', transform: [{ translateX: 20 }] },

  sliderWrap: { flexDirection: 'row', alignItems: 'center', gap: 14, width: 290 },
  sliderTrack: { flex: 1, height: 6, borderRadius: 3, backgroundColor: 'rgba(255,255,255,0.10)' },
  sliderFill: { position: 'absolute', left: 0, top: 0, bottom: 0, borderRadius: 3 },
  thumb: {
    position: 'absolute',
    top: -6,
    width: 18,
    height: 18,
    marginLeft: -9,
    borderRadius: 9,
    backgroundColor: '#fff',
    borderWidth: 3,
    boxShadow: '0 2px 6px rgba(0,0,0,0.5)',
  },
  sliderValue: { width: 64, fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text, textAlign: 'right' },

  segmented: { flexDirection: 'row', padding: 3, borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.35)', borderWidth: 1, borderColor: colors.line },
  segment: { paddingHorizontal: 11, paddingVertical: 6, borderRadius: 6 },
  segmentActive: { backgroundColor: '#24314f', boxShadow: '0 1px 4px rgba(0,0,0,0.5), inset 0 1px 0 rgba(255,255,255,0.08)' },
  segmentText: { fontFamily: font, fontWeight: '500', fontSize: 12, color: colors.textMute },
  segmentTextActive: { color: '#fff', fontWeight: '600' },

  dropdown: {
    width: 220,
    height: 34,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 12,
    borderRadius: 7,
    backgroundColor: 'rgba(0,0,0,0.35)',
    borderWidth: 1,
    borderColor: colors.lineStrong,
  },
  dropdownOpen: { borderColor: colors.cyan, borderBottomLeftRadius: 0, borderBottomRightRadius: 0, backgroundColor: '#111a30' },
  dropdownText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.text },
  caret: { width: 7, height: 7, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.textDim, transform: [{ rotate: '45deg' }], marginTop: -3 },
  menu: {
    position: 'absolute',
    left: 374,
    top: 441,
    width: 220,
    paddingVertical: 4,
    backgroundColor: '#111a30',
    borderWidth: 1,
    borderTopWidth: 0,
    borderColor: colors.cyan,
    borderBottomLeftRadius: 8,
    borderBottomRightRadius: 8,
    boxShadow: '0 18px 30px rgba(0,0,0,0.7)',
    zIndex: 10,
  },
  menuItem: { height: 32, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: 12 },
  menuItemActive: { backgroundColor: 'rgba(79,209,255,0.16)' },
  menuText: { fontFamily: font, fontWeight: '500', fontSize: 13, color: colors.textDim },
  checkMark: { width: 6, height: 11, borderRightWidth: 2, borderBottomWidth: 2, borderColor: colors.cyan, transform: [{ rotate: '45deg' }], marginTop: -3 },

  help: {
    position: 'absolute',
    right: 32,
    top: 92,
    width: 320,
    borderRadius: 14,
    overflow: 'hidden',
    backgroundColor: colors.panelSolid,
    borderWidth: 1,
    borderColor: colors.line,
    boxShadow: '0 20px 40px rgba(0,0,0,0.5)',
  },
  preview: { width: '100%', height: 150 },
  helpBody: { padding: 18 },
  helpTitle: { fontFamily: font, fontWeight: '700', fontSize: 18, color: colors.text },
  helpText: { fontFamily: font, fontSize: 13, lineHeight: 20, color: colors.textDim, marginTop: 8 },
  impactLabel: { fontFamily: font, fontWeight: '600', fontSize: 11, letterSpacing: 1.4, textTransform: 'uppercase', color: colors.textMute, marginTop: 16 },
  impact: { flexDirection: 'row', gap: 4, marginTop: 8, marginBottom: 12 },
  impactSeg: { flex: 1, height: 6, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.10)' },
  statLine: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 4 },
  statKey: { fontFamily: font, fontSize: 13, color: colors.textDim },
  statVal: { fontFamily: font, fontWeight: '600', fontSize: 13, color: colors.text },

  footer: { position: 'absolute', right: 32, bottom: 28, flexDirection: 'row', gap: 12 },
  btn: { height: 44, paddingHorizontal: 22, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  btnGhost: { borderWidth: 1, borderColor: colors.lineStrong },
  btnGhostText: { fontFamily: font, fontWeight: '600', fontSize: 14, color: colors.textDim },
  btnPrimary: {
    experimental_backgroundImage: 'linear-gradient(180deg, #ffe29a 0%, #f2c35b 50%, #c9902d 100%)',
    boxShadow: '0 6px 18px rgba(242,195,91,0.35), inset 0 1px 0 rgba(255,255,255,0.6)',
  },
  btnPrimaryText: { fontFamily: font, fontWeight: '700', fontSize: 14, color: '#2a1a03', letterSpacing: 0.5 },
});
