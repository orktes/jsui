// Paint bench: a static settings-like screen (panels with shadows, gradients,
// borders and plenty of text) whose only motion is the blinking caret of a
// focused TextInput. 1280 x 720.
import React from 'react';
import { AppRegistry, StyleSheet, Text, TextInput, View } from 'react-native';

const ROWS = ['Master volume', 'Music', 'Effects', 'Voice chat', 'Subtitles', 'Colour-blind mode', 'Motion blur', 'Field of view'];

function Panel({ title, children, style }) {
  return (
    <View style={[styles.panel, style]}>
      <Text style={styles.panelTitle}>{title}</Text>
      {children}
    </View>
  );
}

function App() {
  return (
    <View style={styles.root}>
      <View style={styles.header}>
        <Text style={styles.h1}>Settings</Text>
        <View style={styles.tabs}>
          {['Audio', 'Video', 'Controls', 'Account'].map((t, i) => (
            <View key={t} style={[styles.tab, i === 0 && styles.tabOn]}>
              <Text style={styles.tabText}>{t}</Text>
            </View>
          ))}
        </View>
      </View>
      <View style={styles.body}>
        <Panel title="Audio" style={{ flex: 1 }}>
          {ROWS.map((r, i) => (
            <View key={r} style={styles.row}>
              <Text style={styles.label}>{r}</Text>
              <View style={styles.track}>
                <View style={[styles.trackFill, { width: `${30 + (i * 17) % 70}%` }]} />
              </View>
            </View>
          ))}
        </Panel>
        <View style={{ flex: 1, gap: 20 }}>
          <Panel title="Profile">
            <Text style={styles.label}>Display name</Text>
            <TextInput autoFocus style={styles.input} defaultValue="Seren Ashvale" />
            <Text style={styles.hint}>Shown to other players in lobbies, chat and the scoreboard.</Text>
          </Panel>
          <Panel title="About" style={{ flex: 1 }}>
            <Text style={styles.para}>
              Emberfall keeps your settings in the cloud. Changes apply at once and sync to every device you sign in on.
              Audio levels are per output device; video settings are per display.
            </Text>
          </Panel>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0b1020', padding: 28, gap: 20 },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  h1: { color: '#fff', fontSize: 30, fontWeight: '800' },
  tabs: { flexDirection: 'row', gap: 8 },
  tab: { paddingHorizontal: 16, paddingVertical: 8, borderRadius: 18, borderWidth: 1, borderColor: 'rgba(255,255,255,0.15)' },
  tabOn: { experimental_backgroundImage: 'linear-gradient(90deg, #5b6cff, #a35bff)', borderColor: 'transparent' },
  tabText: { color: '#fff', fontSize: 14, fontWeight: '600' },
  body: { flex: 1, flexDirection: 'row', gap: 20 },
  panel: {
    padding: 20,
    gap: 12,
    borderRadius: 16,
    backgroundColor: 'rgba(24,32,65,0.92)',
    borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.1)',
    boxShadow: '0 10px 30px rgba(0,0,0,0.5)',
  },
  panelTitle: { color: '#f2c35b', fontSize: 16, fontWeight: '700', letterSpacing: 1 },
  row: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', height: 36 },
  label: { color: '#cfd6ff', fontSize: 15 },
  track: { width: 240, height: 8, borderRadius: 4, backgroundColor: 'rgba(255,255,255,0.1)', overflow: 'hidden' },
  trackFill: { height: '100%', experimental_backgroundImage: 'linear-gradient(90deg, #4fd1ff, #4f7aff)' },
  input: { height: 44, paddingHorizontal: 12, borderRadius: 10, borderWidth: 1, borderColor: '#5b6cff', color: '#fff', fontSize: 16, backgroundColor: 'rgba(5,7,16,0.6)' },
  hint: { color: '#7d86a8', fontSize: 13 },
  para: { color: '#aab3d6', fontSize: 15, lineHeight: 22 },
});

AppRegistry.registerComponent('Caret', () => App);
export default App;
