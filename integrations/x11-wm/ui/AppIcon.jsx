// An app's icon: the window's own _NET_WM_ICON (host texture), else the
// desktop file's PNG, else a generated glyph tile.
import React, { memo } from 'react';
import { HostTexture, Image, View } from 'react-native';
import { AppGlyph, glyphFor } from './icons.jsx';
import { useTheme } from './theme.js';

// A theme PNG sits on a plate of the same squircle as the glyph tiles, so
// every icon in a row has the same shape and weight.
function Plate({ size, children }) {
  const t = useTheme();
  return (
    <View style={{
      width: size, height: size, borderRadius: size * 0.28, alignItems: 'center', justifyContent: 'center',
      backgroundColor: t.name === 'dark' ? '#f4f5fb' : '#ffffff',
      boxShadow: `0px ${size * 0.06}px ${size * 0.18}px rgba(0,0,0,0.28)`,
    }}>
      {children}
    </View>
  );
}

export const AppIcon = memo(function AppIcon({ app, win, size = 56, preferGlyph = false }) {
  const id = (app && app.id) || (win && (win.appId || win.wmClass || win.instance)) || 'app';
  const glyph = glyphFor(`${id} ${(app && app.exec) || ''} ${(win && win.instance) || ''}`);
  if (!preferGlyph && glyph === 'app') {
    if (app && app.icon) {
      return (
        <Plate size={size}>
          <Image source={{ uri: app.icon }} style={{ width: size * 0.7, height: size * 0.7 }} resizeMode="contain" />
        </Plate>
      );
    }
    if (win && win.icon) {
      return (
        <Plate size={size}>
          <HostTexture name={`icon:${win.id}`} style={{ width: size * 0.7, height: size * 0.7 }} resizeMode="contain" />
        </Plate>
      );
    }
  }
  return <AppGlyph id={id} glyph={glyph} size={size} />;
});
