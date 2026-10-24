// jsui-bench "mount": the showcase screen (examples/showcase/interactions.jsx)
// mounted from scratch every frame. A rAF counter changes the screen's key,
// so each frame's commit unmounts the previous copy and mounts a new one:
// the cost of a real app's largest commit (every component, hook, host node
// and bridge call of the screen), measured frame after frame.
import React, { useEffect, useState } from 'react';
import { AppRegistry } from 'react-native';
import Showcase from '../../../examples/showcase/interactions.jsx';

function Remount() {
  const [n, setN] = useState(0);
  useEffect(() => {
    let raf = 0;
    const step = () => {
      setN((v) => v + 1);
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, []);
  return <Showcase key={n} />;
}

AppRegistry.registerComponent('mount', () => Remount);
// interactions.jsx registers 'Showcase' first: start this one explicitly.
AppRegistry.runApplication('mount', {});
