// Tabbed code sample: the same UI from the app side and from three hosts.
import React, { useState } from 'react';
import { StyleSheet, View } from 'react-native';
import { colors } from '../../theme.js';
import { CodeBlock } from '../../components/CodeBlock.jsx';
import { Segmented } from '../motion/controls.jsx';

const TABS = [
  {
    id: 'app',
    label: 'App.jsx',
    lang: 'js',
    code: `
import { useState } from 'react';
import { AppRegistry, Pressable, Text, View } from 'react-native';

function PauseMenu() {
  const [volume, setVolume] = useState(7);
  return (
    <View style={{ padding: 32, gap: 12, backgroundColor: '#0d1322' }}>
      <Text style={{ color: '#f2c35b', fontSize: 28, fontWeight: '700' }}>
        Paused
      </Text>
      {['Resume', 'Settings', 'Quit'].map((label, i) => (
        <Pressable key={label} autoFocus={i === 0}
          style={({ focused, hovered }) => ({
            padding: 14, borderRadius: 10,
            backgroundColor: focused || hovered ? '#1a2440' : 'transparent',
          })}>
          <Text style={{ color: '#eef2ff', fontSize: 18 }}>{label}</Text>
        </Pressable>
      ))}
    </View>
  );
}

AppRegistry.registerComponent('game', () => PauseMenu);`,
  },
  {
    id: 'c',
    label: 'host.c',
    lang: 'c',
    code: `
#include <jsui/jsui.h>

JsuiConfig cfg;
jsui_config_init(&cfg);
JsuiRuntime* ui = jsui_create(&cfg);
jsui_load_bundle_url(ui, "ui/bundle.js");
jsui_set_viewport(ui, 1920, 1080, 1.0f);

// every frame
jsui_send_gamepad_button(ui, 0, 0 /* A */, a_down, 1.0f);
jsui_tick(ui, now_ms);
if (jsui_needs_redraw(ui))
  jsui_render_rgba(ui, pixels, 1920, 1080, 1920 * 4);
upload_texture(pixels);  // or jsui_render_gl() into your FBO`,
  },
  {
    id: 'gd',
    label: 'main.gd',
    lang: 'gd',
    code: `
extends Control

# JsuiView is a Control from the JSUI GDExtension.
@onready var ui = $JsuiView

func _ready():
    ui.bundle_url = "res://ui/dist/bundle.js"
    ui.message_received.connect(_on_ui_message)

func _on_ui_message(name, data, _json):
    if name == "ui:ping":
        ui.post_message("godot:pong", {"time": Time.get_ticks_msec()})`,
  },
  {
    id: 'web',
    label: 'index.html',
    lang: 'js',
    code: `
import { createJsui } from './jsui-web.js';

// This very page: one canvas, ~9 MB of WebAssembly.
const jsui = await createJsui({
  canvas: document.getElementById('ui'),
  bundleUrl: './app/bundle.js',
});
jsui.on('message', (name, data) => console.log(name, data));
jsui.postMessage('route', location.hash.slice(1));`,
  },
];

export function CodeTabs() {
  const [tab, setTab] = useState('app');
  const t = TABS.find((x) => x.id === tab);
  return (
    <View style={styles.wrap}>
      <Segmented options={TABS} value={tab} onChange={setTab} color={colors.gold} small />
      <CodeBlock key={t.id} title={t.label} code={t.code} lang={t.lang} />
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { gap: 12 },
});
