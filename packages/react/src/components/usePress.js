// Press state machine shared by Pressable, the Touchables and Text, following
// React Native's Pressability:
//
//   pointer: pointerdown -> pressIn (after delayPressIn; + long-press timer)
//            pointerup -> pressOut, press
//            pointerleave / pointercancel while pressed -> pressOut (cancel)
//   keyboard (focused): Enter / Space keydown -> pressIn, keyup -> pressOut, press
//   gamepad A / native activation: `click` without a preceding pointer or key
//            press -> pressIn, press, pressOut
//
// Like RN, onPressOut (and the pressed state) is held until the press has
// lasted minPressDuration (130 ms) or delayPressOut, so a quick tap or a
// gamepad activation still shows its pressed feedback; onPress then fires
// before the delayed onPressOut. onHoverIn / onHoverOut honor delayHoverIn /
// delayHoverOut (the `hovered` state itself changes immediately, like
// react-native-web).
//
// Handlers are created once per component and read the latest props through
// a ref, so re-renders do not change host props (no bridge traffic).

import { useEffect, useRef, useState } from 'react';
import { now } from '../native.js';

const DEFAULT_LONG_PRESS_MS = 500;
const DEFAULT_MIN_PRESS_MS = 130;
const CLICK_DEDUPE_MS = 400;

// Interaction states as bit flags -> shared frozen objects (no allocations).
const PRESSED = 1;
const HOVERED = 2;
const FOCUSED = 4;
const STATES = [];
for (let i = 0; i < 8; i++) STATES.push(Object.freeze({ pressed: !!(i & PRESSED), hovered: !!(i & HOVERED), focused: !!(i & FOCUSED) }));
export const IDLE_STATE = STATES[0];

function isActivationKey(e) {
  const k = e.key;
  return k === 'Enter' || k === ' ' || k === 'Spacebar' || k === 'NumpadEnter';
}

function delayOf(v, fallback = 0) {
  const n = v == null ? fallback : +v;
  return n > 0 ? n : 0;
}

class PressController {
  constructor() {
    this.props = null;
    this.bits = 0;
    this.source = null;  // 'pointer' | 'key' | 'click' while a press is in flight
    this.active = false;  // onPressIn delivered (delayPressIn elapsed)
    this.activatedAt = 0;
    this.longTimer = null;
    this.pressInTimer = null;
    this.pressOutTimer = null;
    this.pressOutFire = null;
    this.hoverTimer = null;
    this.hoverPending = null;
    this.longFired = false;
    this.lastEnd = -1e9;
    this.setBits = null;  // React state setter, or null when not tracked
    this.mounted = true;
    const self = this;
    this.handlers = {
      onPointerDown(e) {
        self.pointerDown(e);
      },
      onPointerUp(e) {
        self.pointerUp(e);
      },
      onPointerCancel(e) {
        self.cancel(e);
      },
      onPointerEnter(e) {
        self.hover(true, e);
      },
      onPointerLeave(e) {
        self.hover(false, e);
      },
      onClick(e) {
        self.click(e);
      },
      onKeyDown(e) {
        self.keyDown(e);
      },
      onKeyUp(e) {
        self.keyUp(e);
      },
      onFocus(e) {
        self.focus(true, e);
      },
      onBlur(e) {
        self.focus(false, e);
      },
    };
  }

  set(bit, on) {
    const next = on ? this.bits | bit : this.bits & ~bit;
    if (next === this.bits) return;
    this.bits = next;
    if (this.setBits && this.mounted) this.setBits(next);
  }

  disabled() {
    return !!this.props.disabled;
  }

  // Delivers a pending (delayed) onPressOut right away.
  flushPressOut() {
    if (!this.pressOutTimer) return;
    clearTimeout(this.pressOutTimer);
    const fire = this.pressOutFire;
    this.pressOutTimer = null;
    this.pressOutFire = null;
    if (fire) fire();
  }

  // A press starts (pointer down, activation key, click): onPressIn now or
  // after delayPressIn, and the long-press timer.
  begin(e, source) {
    this.flushPressOut();
    this.source = source;
    this.longFired = false;
    this.active = false;
    const p = this.props;
    const delayIn = source === 'pointer' ? delayOf(p.delayPressIn != null ? p.delayPressIn : p.unstable_pressDelay) : 0;
    if (delayIn > 0) {
      clearTimeout(this.pressInTimer);
      this.pressInTimer = setTimeout(() => {
        this.pressInTimer = null;
        if (this.source === source && this.mounted) this.activate(e);
      }, delayIn);
    } else {
      this.activate(e);
    }
    if (p.onLongPress && source !== 'click') {
      const ms = delayOf(p.delayLongPress, DEFAULT_LONG_PRESS_MS) + delayIn;
      clearTimeout(this.longTimer);
      this.longTimer = setTimeout(() => {
        this.longTimer = null;
        if (!this.source || !this.mounted) return;
        if (!this.active) this.activate(e);
        this.longFired = true;
        if (this.props.onLongPress) this.props.onLongPress(e);
      }, ms);
    }
  }

  activate(e) {
    this.active = true;
    this.activatedAt = now();
    this.set(PRESSED, true);
    if (this.props.onPressIn) this.props.onPressIn(e);
  }

  // The press ends; `activate`: released inside (fires onPress).
  end(e, activate) {
    clearTimeout(this.longTimer);
    clearTimeout(this.pressInTimer);
    this.longTimer = null;
    this.pressInTimer = null;
    const wasSource = this.source;
    this.source = null;
    if (!wasSource) return;
    if (!this.active) {
      if (!activate) return;  // cancelled before onPressIn: nothing to undo
      this.activate(e);       // released before delayPressIn: press in, then out
    }
    this.active = false;
    this.lastEnd = now();
    const p = this.props;
    const held = now() - this.activatedAt;
    const delayOut = Math.max(delayOf(p.minPressDuration, DEFAULT_MIN_PRESS_MS) - held, delayOf(p.delayPressOut));
    const out = () => {
      this.set(PRESSED, false);
      if (this.props.onPressOut) this.props.onPressOut(e);
    };
    if (delayOut > 0 && activate) {
      this.pressOutFire = out;
      this.pressOutTimer = setTimeout(() => {
        this.pressOutTimer = null;
        this.pressOutFire = null;
        if (this.mounted) out();
      }, delayOut);
    } else {
      out();
    }
    if (activate && !this.longFired && p.onPress) p.onPress(e);
  }

  pointerDown(e) {
    const p = this.props;
    if (p.onPointerDown) p.onPointerDown(e);
    if (this.disabled() || (e.button !== undefined && e.button !== 0)) return;
    e.stopPropagation();
    if (this.source) return;
    this.begin(e, 'pointer');
  }

  pointerUp(e) {
    const p = this.props;
    if (p.onPointerUp) p.onPointerUp(e);
    if (this.source !== 'pointer') return;
    e.stopPropagation();
    this.end(e, true);
  }

  cancel(e) {
    if (this.source) this.end(e, false);
  }

  hover(on, e) {
    const p = this.props;
    if (!on && this.source === 'pointer') this.cancel(e);
    if (this.disabled()) return;
    if (!!(this.bits & HOVERED) === on) return;
    this.set(HOVERED, on);
    const pending = this.hoverPending;
    clearTimeout(this.hoverTimer);
    this.hoverTimer = null;
    this.hoverPending = null;
    // Leaving before a delayed onHoverIn fired cancels both (and vice versa),
    // like RN's Pressability.
    if (pending !== null && pending !== on) return;
    const f = on ? p.onHoverIn || p.onMouseEnter : p.onHoverOut || p.onMouseLeave;
    if (!f) return;
    const delay = delayOf(on ? p.delayHoverIn : p.delayHoverOut);
    if (delay > 0) {
      this.hoverPending = on;
      this.hoverTimer = setTimeout(() => {
        this.hoverTimer = null;
        this.hoverPending = null;
        if (this.mounted) f(e);
      }, delay);
    } else {
      f(e);
    }
  }

  click(e) {
    const p = this.props;
    if (p.onClick) p.onClick(e);
    if (this.disabled()) return;
    e.stopPropagation();
    // Native may synthesize a click after pointerup or Enter; those presses
    // already fired. A click with no press in flight is an activation from
    // the gamepad (A button) or the host.
    if (this.source) return;
    const pt = e.nativeEvent && e.nativeEvent.pointerType;
    const synthesized = pt === 'gamepad' || pt === 'keyboard';
    if (!synthesized && now() - this.lastEnd < CLICK_DEDUPE_MS) return;
    // pressIn, press, then pressOut after minPressDuration: the pressed
    // state stays visible briefly, like a tap.
    this.begin(e, 'click');
    this.end(e, true);
  }

  keyDown(e) {
    const p = this.props;
    if (p.onKeyDown) p.onKeyDown(e);
    if (e.isDefaultPrevented() || this.disabled() || !isActivationKey(e)) return;
    e.preventDefault();  // we handle activation; native must not click as well
    e.stopPropagation();
    if (e.repeat || this.source) return;
    this.begin(e, 'key');
  }

  keyUp(e) {
    const p = this.props;
    if (p.onKeyUp) p.onKeyUp(e);
    if (this.source !== 'key' || !isActivationKey(e)) return;
    e.preventDefault();
    e.stopPropagation();
    this.end(e, true);
  }

  focus(on, e) {
    const p = this.props;
    this.set(FOCUSED, on);
    if (!on && this.source === 'key') this.end(e, false);
    const f = on ? p.onFocus : p.onBlur;
    if (f) f(e);
  }

  dispose() {
    this.mounted = false;
    clearTimeout(this.longTimer);
    clearTimeout(this.pressInTimer);
    clearTimeout(this.pressOutTimer);
    clearTimeout(this.hoverTimer);
  }
}

/**
 * @param {object} props  press props (onPress, onPressIn, onPressOut,
 *   onLongPress, delayLongPress, onHoverIn/Out, onFocus/onBlur, disabled, ...)
 * @param {boolean} trackState  re-render on pressed/hovered/focused changes
 * @returns {[object, {pressed: boolean, hovered: boolean, focused: boolean}]}
 *   host event handlers (stable identity) and the interaction state
 */
export function usePress(props, trackState) {
  const ref = useRef(null);
  if (ref.current === null) ref.current = new PressController();
  const c = ref.current;
  c.props = props;
  const [bits, setBits] = useState(0);
  c.setBits = trackState ? setBits : null;
  useEffect(() => {
    c.mounted = true;
    return () => c.dispose();
  }, [c]);
  return [c.handlers, STATES[trackState ? bits : c.bits & 7] || IDLE_STATE, c];
}

// Props consumed by usePress that must not reach the host element.
export const PRESS_PROPS = [
  'onPress',
  'onPressIn',
  'onPressOut',
  'onLongPress',
  'delayLongPress',
  'onHoverIn',
  'onHoverOut',
  'onMouseEnter',
  'onMouseLeave',
  'onPointerDown',
  'onPointerUp',
  'onClick',
  'onKeyDown',
  'onKeyUp',
  'onFocus',
  'onBlur',
  'disabled',
  'delayPressIn',
  'delayPressOut',
  'minPressDuration',
  'delayHoverIn',
  'delayHoverOut',
  'unstable_pressDelay',
  'pressRetentionOffset',
  'android_ripple',
  'android_disableSound',
  'cancelable',
  'testOnly_pressed',
  'touchSoundDisabled',
  'rejectResponderTermination',
];

/** Copies `props` minus `omit` keys (array) into a new object. */
export function omit(props, omitList, extra) {
  const out = {};
  for (const k in props) {
    if (omitList.indexOf(k) >= 0 || (extra && extra.indexOf(k) >= 0)) continue;
    out[k] = props[k];
  }
  return out;
}
