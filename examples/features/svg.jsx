// Feature: react-native-svg (JSUI: VectorView; Chrome: its own SVG renderer).
// Row 1: stroked icon (polyline, circle, round caps / joins), filled path
//        with arcs, linear gradient on a rounded rect, radial gradient with a
//        focal point, nested transforms (string form), evenodd + dashes.
// Row 2: clipPath, group opacity + polygon with fill-opacity, use / symbol
//        with preserveAspectRatio, luminance mask, currentColor + joins,
//        userSpaceOnUse gradient with reflect.
// Row 3: text with text-anchor and a tspan, nested svg + miter, inherited
//        group paint with stroke-opacity, square caps.
import React from 'react';
import { StyleSheet, View } from 'react-native';
import Svg, {
  Circle, ClipPath, Defs, Ellipse, G, Line, LinearGradient, Mask, Path, Polygon, Polyline, RadialGradient, Rect,
  Stop, Symbol, Text, TSpan, Use,
} from 'react-native-svg';

export const meta = { width: 640, height: 380, maxMismatch: 0.5, maxTileMismatch: 3, title: 'SVG (react-native-svg)' };

const S = 100;

export default function App() {
  return (
    <View style={styles.root}>
      <View style={styles.row}>
        <Svg width={S} height={S} viewBox="0 0 24 24" fill="none" stroke="#123" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round">
          <Polyline points="22 12 18 12 15 21 9 3 6 12 2 12" />
          <Circle cx="12" cy="12" r="3" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 24 24">
          <Path fill="#c22" d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Defs>
            <LinearGradient id="a" x1="0%" y1="0%" x2="100%" y2="100%">
              <Stop offset="0%" stopColor="#ff9900" />
              <Stop offset="100%" stopColor="#3300ff" stopOpacity="0.5" />
            </LinearGradient>
          </Defs>
          <Rect x="10" y="10" width="80" height="80" rx="15" fill="url(#a)" stroke="#000" strokeWidth="4" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Defs>
            <RadialGradient id="r" cx="50%" cy="50%" r="50%" fx="30%" fy="30%">
              <Stop offset="0" stopColor="white" />
              <Stop offset="1" stopColor="#06c" />
            </RadialGradient>
          </Defs>
          <Circle cx="50" cy="50" r="45" fill="url(#r)" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <G transform="translate(50 50) rotate(30) scale(1.2 0.8)">
            <Rect x="-30" y="-20" width="60" height="40" fill="#2a2" />
            <Circle r="10" fill="#fff" transform="translate(15,0)" />
          </G>
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Path fillRule="evenodd" fill="#f80" d="M10 10h80v80h-80z M30 30h40v40h-40z" />
          <Line x1="10" y1="95" x2="90" y2="95" stroke="#000" strokeWidth="3" strokeDasharray="8 4" strokeDashoffset="2" />
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Defs>
            <ClipPath id="c">
              <Circle cx="50" cy="50" r="35" />
            </ClipPath>
          </Defs>
          <G clipPath="url(#c)">
            <Rect width="50" height="100" fill="#e33" />
            <Rect x="50" width="50" height="100" fill="#33e" />
          </G>
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <G opacity="0.5">
            <Rect x="10" y="10" width="50" height="50" fill="#f00" />
            <Rect x="40" y="40" width="50" height="50" fill="#00f" />
          </G>
          <Polygon points="50,5 61,35 95,35 68,57 79,91 50,70 21,91 32,57 5,35 39,35" fill="#fc0" fillOpacity="0.7" stroke="#000" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 200 100">
          <Defs>
            <Symbol id="s" viewBox="0 0 10 10">
              <Circle cx="5" cy="5" r="4" fill="#0a8" />
            </Symbol>
            <Rect id="r1" width="20" height="20" fill="#333" />
          </Defs>
          <Rect width="200" height="100" fill="#eee" />
          <Use href="#s" x="10" y="10" width="80" height="80" />
          <Use href="#r1" x="120" y="40" />
          <Use href="#r1" x="150" y="40" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Defs>
            <Mask id="m">
              <Rect width="100" height="100" fill="white" />
              <Circle cx="50" cy="50" r="25" fill="black" />
            </Mask>
          </Defs>
          <Rect x="10" y="10" width="80" height="80" fill="#639" mask="url(#m)" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100" color="#d40">
          <Ellipse cx="50" cy="30" rx="40" ry="20" fill="currentColor" />
          <Polyline points="10,90 30,60 50,90 70,60 90,90" fill="none" stroke="#000" strokeWidth="8" strokeLinejoin="bevel" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 50" preserveAspectRatio="xMinYMin slice">
          <Defs>
            <LinearGradient id="u" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="25" y2="0" spreadMethod="reflect">
              <Stop offset="0" stopColor="#000" />
              <Stop offset="1" stopColor="#fff" />
            </LinearGradient>
          </Defs>
          <Rect width="100" height="50" fill="url(#u)" />
        </Svg>
      </View>
      <View style={styles.row}>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Text x="50" y="40" fontSize="22" textAnchor="middle" fill="#333" fontFamily="Inter" fontWeight="bold">Abc</Text>
          <Text x="10" y="80" fontSize="16" fill="#c00" fontFamily="Inter">
            x<TSpan fill="#00c" dy="-5">2</TSpan>
          </Text>
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <Polyline points="20,90 50,20 80,90" fill="none" stroke="#000" strokeWidth="6" strokeMiterlimit="10" />
        </Svg>
        <Svg width={S} height={S} viewBox="0 0 100 100">
          <G fill="#08f" stroke="#000" strokeOpacity="0.4" strokeWidth="10">
            <Circle cx="30" cy="30" r="20" />
            <Rect x="55" y="10" width="35" height="40" />
          </G>
          <Line x1="15" y1="75" x2="85" y2="75" stroke="#090" strokeWidth="14" strokeLinecap="square" />
        </Svg>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { width: 640, height: 380, backgroundColor: '#ffffff', padding: 10 },
  row: { flexDirection: 'row', gap: 4, marginBottom: 20 },
});
