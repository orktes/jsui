// The JSUI showcase site. Every pixel of the page is drawn by the JSUI
// runtime compiled to WebAssembly; this file is ordinary React Native code.
import React, { useEffect, useRef } from 'react';
import { Animated, AppRegistry, Easing, Fonts, ScrollView, StyleSheet, Text, View, precompileShaders } from 'react-native';
import { RouterProvider, segments, useRouterState } from './router.js';
import { mono, site } from './theme.js';
import { NavBar, NAV_HEIGHT } from './components/NavBar.jsx';
import { Footer } from './components/Footer.jsx';
import { HomePage } from './pages/Home.jsx';
import { ScreensPage } from './pages/Screens.jsx';
import { InteractionsPage } from './pages/Interactions.jsx';
import { InputPage } from './pages/Input.jsx';
import { MotionPage } from './pages/Motion.jsx';
import { PlaygroundPage } from './pages/Playground.jsx';
import { EmbedPage } from './pages/Embed.jsx';
import { SCREEN_SHADERS } from './screens/index.js';

Fonts.load(mono, require('./assets/fonts/JetBrainsMonoNL-Regular.ttf'), { weight: 400 });
Fonts.load(mono, require('./assets/fonts/JetBrainsMonoNL-Bold.ttf'), { weight: 700 });

export const PAGES = {
  '': HomePage,
  screens: ScreensPage,
  interactions: InteractionsPage,
  input: InputPage,
  motion: MotionPage,
  playground: PlaygroundPage,
  embed: EmbedPage,
};

function Page({ path }) {
  const [head, ...rest] = segments(path);
  const Component = PAGES[head || ''] || NotFound;
  const fade = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    fade.setValue(0);
    Animated.timing(fade, { toValue: 1, duration: 360, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
  }, [head]);
  return (
    <Animated.View style={{ flex: 1, opacity: fade }}>
      <ScrollView key={head || 'home'} style={{ flex: 1 }} onScroll={noteScroll} scrollEventThrottle={200} contentContainerStyle={{ paddingTop: NAV_HEIGHT, minHeight: '100%' }}>
        <Component params={rest} />
        <Footer />
      </ScrollView>
    </Animated.View>
  );
}

function NotFound() {
  return (
    <View style={{ padding: 80, alignItems: 'center', gap: 12 }}>
      <Text style={{ fontSize: 48, fontWeight: '700', color: '#eef2ff' }}>404</Text>
      <Text style={{ fontSize: 16, color: '#a3aecb' }}>This page wandered off the map.</Text>
    </View>
  );
}

// When the page last scrolled (Date.now()).
let lastScroll = 0;
const noteScroll = () => {
  lastScroll = Date.now();
};

// The screens' shaders, compiled and warmed on the GPU once the first page
// has loaded, one at a time and only while the page is not scrolling, so
// opening a screen later does not stall on the GPU building their pipelines
// (each warm-up frame may itself take a moment on a cold GPU cache).
function usePreloadedShaders() {
  useEffect(() => {
    let stopped = false;
    let timer = 0;
    const next = (i) => {
      if (stopped || i >= SCREEN_SHADERS.length) return;
      if (Date.now() - lastScroll < 1500) {
        timer = setTimeout(() => next(i), 500);
        return;
      }
      precompileShaders(SCREEN_SHADERS[i]).then(() => {
        timer = setTimeout(() => next(i + 1), 150);
      });
    };
    timer = setTimeout(() => next(0), 5000);
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, []);
}

function App() {
  const router = useRouterState();
  usePreloadedShaders();
  return (
    <RouterProvider value={router}>
      <View style={styles.root}>
        <View pointerEvents="none" style={styles.glow} />
        <Page path={router.path} />
        <NavBar />
      </View>
    </RouterProvider>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: site.bg },
  glow: {
    ...StyleSheet.absoluteFillObject,
    experimental_backgroundImage:
      'radial-gradient(ellipse at 20% 0%, rgba(79, 140, 255, 0.16) 0%, rgba(5, 7, 15, 0) 55%), radial-gradient(ellipse at 90% 10%, rgba(242, 195, 91, 0.10) 0%, rgba(5, 7, 15, 0) 50%)',
  },
});

AppRegistry.registerComponent('site', () => App);
