// A finger's vertical travel, followed natively: no JS on any move.
//
// Spread `handlers` on the Animated.View that takes the gesture. JSUI maps
// pointer-down and pointer-move events of the view and its children onto
// native values (Animated.event with the native driver), so `dy` (pageY
// minus the pageY at the press) follows the finger on the frame it moves.
// `dy` reads 0 until the PanResponder takes the gesture: its grant sets
// `on` (dy then catches up to where the finger is), its release / termination
// clears it, when JS hands the position over to its own animation. The
// PanResponder still sees every move (for gestureState), but nothing on
// screen waits for it.
import { useRef } from 'react';
import { Animated } from 'react-native';

export function useNativeDragY() {
  const ref = useRef(null);
  if (!ref.current) {
    const y = new Animated.Value(0);
    const y0 = new Animated.Value(0);
    const on = new Animated.Value(0);
    ref.current = {
      on,
      dy: Animated.multiply(Animated.subtract(y, y0), on),
      handlers: {
        onPointerDown: Animated.event([{ nativeEvent: { pageY: y0 } }], { useNativeDriver: true }),
        onPointerMove: Animated.event([{ nativeEvent: { pageY: y } }], { useNativeDriver: true }),
      },
    };
  }
  return ref.current;
}
