// An app's icon: the desktop file's icon (Ubuntu Touch's full-bleed Suru
// squares clipped to the tile shape, theme icons on a plate), else the
// window's own _NET_WM_ICON (host texture), else a generated glyph tile.
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
  if (!preferGlyph && app && app.icon && app.iconShape === 'square') {
    return (
      // The host's PNG has the tile shape cut out already (jsui-icons), and
      // no shadow: a rounded clip or a blurred shadow per icon made the
      // launcher's grid 40-55 ms to paint on the Tab A7.
      <View style={{ width: size, height: size }}>
        <Image source={{ uri: app.icon }} style={{ width: size, height: size }} resizeMode="stretch" />
      </View>
    );
  }
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
  const name = (app && app.name) || '';
  return <AppGlyph id={id} glyph={glyph} size={size} letter={name ? name.trim().charAt(0).toUpperCase() : undefined} />;
});
