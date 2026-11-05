// An app's icon: the window's own _NET_WM_ICON (host texture), else the
// desktop file's PNG, else a generated glyph tile.
import React, { memo } from 'react';
import { HostTexture, Image, View } from 'react-native';
import { AppGlyph, glyphFor } from './icons.jsx';

export const AppIcon = memo(function AppIcon({ app, win, size = 56, preferGlyph = false }) {
  const id = (app && app.id) || (win && (win.appId || win.wmClass || win.instance)) || 'app';
  const glyph = glyphFor(`${id} ${(app && app.exec) || ''} ${(win && win.instance) || ''}`);
  if (!preferGlyph && glyph === 'app') {
    if (app && app.icon) {
      return (
        <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>
          <Image source={{ uri: app.icon }} style={{ width: size * 0.92, height: size * 0.92 }} resizeMode="contain" />
        </View>
      );
    }
    if (win && win.icon) {
      return <HostTexture name={`icon:${win.id}`} style={{ width: size, height: size }} resizeMode="contain" />;
    }
  }
  return <AppGlyph id={id} glyph={glyph} size={size} />;
});
