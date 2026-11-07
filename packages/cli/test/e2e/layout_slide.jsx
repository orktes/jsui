// End-to-end fixture for tests/native/react_layout_slide_e2e_test.cpp: the
// jsui-wm switcher's strip in small. Cards in a horizontal ScrollView, each
// an Animated.View with a native translateX; "remove" swipes the first card
// away (native timing) and drops it under LayoutAnimation's update, so the
// next card slides into its place. 400 x 120.
import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { AppRegistry, Animated, Easing, LayoutAnimation, ScrollView, View } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';

const COLORS = ['#ff0000', '#00ff00', '#0000ff'];

// `slide` ({dx, gen}): the card starts dx px from its new place and slides
// there (a native value set in the commit that moved it).
function Card({ id, w, color, leaving, slide }) {
  const lift = useRef(new Animated.Value(0)).current;
  const shift = useRef(new Animated.Value(0)).current;
  const gen = slide ? slide.gen : 0;
  useLayoutEffect(() => {
    if (!slide) return;
    shift.setValue(slide.dx);
    Animated.timing(shift, { toValue: 0, duration: slide.duration, easing: Easing.linear, useNativeDriver: true }).start();
  }, [gen]); // eslint-disable-line react-hooks/exhaustive-deps
  const enter = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    if (leaving) Animated.timing(lift, { toValue: -200, duration: 100, easing: Easing.linear, useNativeDriver: true }).start();
  }, [leaving, lift]);
  const tx = enter.interpolate({ inputRange: [0, 1], outputRange: [100, 0] });
  const x = React.useMemo(() => Animated.add(tx, shift), []); // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <Animated.View style={{ width: w, marginRight: 20, transform: [{ translateX: x }, { translateY: lift }] }}>
      <View style={{ width: w, height: 80, backgroundColor: color }} />
    </Animated.View>
  );
}

function App() {
  const [cards, setCards] = useState([0, 1, 2]);
  const [leaving, setLeaving] = useState(null);
  const [slide, setSlide] = useState(null);
  // remove: LayoutAnimation's update moves the rest. removeNative: the rest
  // get a native translateX from where they were, set in the same commit
  // (useLayoutEffect): it must land on the frame of that commit's layout.
  useHostMessage('remove', (m) => {
    setLeaving(0);
    setTimeout(() => {
      LayoutAnimation.configureNext({ duration: (m && m.duration) || 200, update: { type: 'linear' } });
      setCards((c) => c.slice(1));
    }, 120);
  });
  useHostMessage('removeNative', (m) => {
    setLeaving(0);
    setTimeout(() => {
      setSlide({ dx: 120, gen: 1, duration: (m && m.duration) || 200 });
      setCards((c) => c.slice(1));
    }, 120);
  });
  useEffect(() => Host.postMessage('ready', true), []);
  const first = cards.length ? 100 : 0;
  const pad = (400 - first) / 2;
  return (
    <View style={{ width: 400, height: 120, backgroundColor: '#000000' }}>
      <ScrollView horizontal style={{ position: 'absolute', left: 0, right: 0, top: 20, height: 80 }}
        contentContainerStyle={{ paddingLeft: pad, paddingRight: pad, alignItems: 'flex-start' }}>
        {cards.map((c) => <Card key={c} id={c} w={100} color={COLORS[c]} leaving={leaving === c} slide={slide} />)}
      </ScrollView>
    </View>
  );
}

AppRegistry.registerComponent('main', () => App);
