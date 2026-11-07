// A finger's travel, followed natively: no JS on any move.
//
// Spread `handlers` on the Animated.View that takes the gesture. JSUI maps
// pointer-down and pointer-move events of the view and its children onto
// native values (Animated.event with the native driver), so `dy` (pageY
// minus the pageY at the press; dx likewise) follows the finger on the
// frame it moves.
// `dy` reads 0 until the PanResponder takes the gesture: its grant sets
// `on` (dy then catches up to where the finger is), its release / termination
// clears it, when JS hands the position over to its own animation. The
// PanResponder still sees every move (for gestureState), but nothing on
// screen waits for it.
import { useRef } from 'react';
import { Animated } from 'react-native';

// The returned object is the same on every render, so it can sit in hook
// deps (a PanResponder rebuilt mid-gesture loses its gesture state).
function useNativeDrag(key, axis) {
  const ref = useRef(null);
  if (!ref.current) {
    const p = new Animated.Value(0);
    const p0 = new Animated.Value(0);
    const on = new Animated.Value(0);
    ref.current = {
      on,
      [axis]: Animated.multiply(Animated.subtract(p, p0), on),
      handlers: {
        onPointerDown: Animated.event([{ nativeEvent: { [key]: p0 } }], { useNativeDriver: true }),
        onPointerMove: Animated.event([{ nativeEvent: { [key]: p } }], { useNativeDriver: true }),
      },
    };
  }
  return ref.current;
}

/** Vertical travel: { dy, on, handlers }. */
export function useNativeDragY() {
  return useNativeDrag('pageY', 'dy');
}

/**
 * Both axes: { dx, dy, x, y, on, handlers }, x / y the finger's page
 * position itself (for drop targets drawn natively).
 */
export function useNativeDragXY() {
  const ref = useRef(null);
  if (!ref.current) {
    const x = new Animated.Value(0);
    const y = new Animated.Value(0);
    const x0 = new Animated.Value(0);
    const y0 = new Animated.Value(0);
    const on = new Animated.Value(0);
    ref.current = {
      on,
      x,
      y,
      dx: Animated.multiply(Animated.subtract(x, x0), on),
      dy: Animated.multiply(Animated.subtract(y, y0), on),
      handlers: {
        onPointerDown: Animated.event([{ nativeEvent: { pageX: x0, pageY: y0 } }], { useNativeDriver: true }),
        onPointerMove: Animated.event([{ nativeEvent: { pageX: x, pageY: y } }], { useNativeDriver: true }),
      },
    };
  }
  return ref.current;
}

/** Horizontal travel: { dx, on, handlers }. */
export function useNativeDragX() {
  return useNativeDrag('pageX', 'dx');
}
