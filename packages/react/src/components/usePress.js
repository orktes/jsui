// Press state machine shared by Pressable, the Touchables and Text.
//
//   pointer: pointerdown -> pressIn (+ long-press timer) -> pointerup -> pressOut, press
//            pointerleave / pointercancel while pressed -> pressOut (cancel)
//   keyboard (focused): Enter / Space keydown -> pressIn, keyup -> pressOut, press
//   gamepad A / native activation: `click` without a preceding pointer or key
//            press -> pressIn, pressOut, press (pressed state flashes briefly)
//
// Handlers are created once per component and read the latest props through
// a ref, so re-renders do not change host props (no bridge traffic).

import { useEffect, useRef, useState } from 'react';
import { now } from '../native.js';

const DEFAULT_LONG_PRESS_MS = 500;
const CLICK_DEDUPE_MS = 400;
const FLASH_MS = 90;

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

class PressController {
  constructor() {
    this.props = null;
    this.bits = 0;
    this.source = null;  // 'pointer' | 'key' while pressed
    this.longTimer = null;
    this.flashTimer = null;
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

  pressIn(e, source) {
    this.source = source;
    this.longFired = false;
    this.set(PRESSED, true);
    const p = this.props;
    if (p.onPressIn) p.onPressIn(e);
    if (p.onLongPress) {
      const ms = p.delayLongPress != null ? +p.delayLongPress : DEFAULT_LONG_PRESS_MS;
      clearTimeout(this.longTimer);
      this.longTimer = setTimeout(() => {
        this.longTimer = null;
        if (!(this.bits & PRESSED) || !this.mounted) return;
        this.longFired = true;
        if (this.props.onLongPress) this.props.onLongPress(e);
      }, ms);
    }
  }

  pressOut(e, activate) {
    clearTimeout(this.longTimer);
    this.longTimer = null;
    this.source = null;
    this.set(PRESSED, false);
    this.lastEnd = now();
    const p = this.props;
    if (p.onPressOut) p.onPressOut(e);
    if (activate && !this.longFired && p.onPress) p.onPress(e);
  }

  pointerDown(e) {
    const p = this.props;
    if (p.onPointerDown) p.onPointerDown(e);
    if (this.disabled() || (e.button !== undefined && e.button !== 0)) return;
    e.stopPropagation();
    if (this.bits & PRESSED) return;
    this.pressIn(e, 'pointer');
  }

  pointerUp(e) {
    const p = this.props;
    if (p.onPointerUp) p.onPointerUp(e);
    if (!(this.bits & PRESSED) || this.source !== 'pointer') return;
    e.stopPropagation();
    this.pressOut(e, true);
  }

  cancel(e) {
    if (this.bits & PRESSED) this.pressOut(e, false);
  }

  hover(on, e) {
    const p = this.props;
    if (!on && this.source === 'pointer') this.cancel(e);
    if (this.disabled()) return;
    if (!!(this.bits & HOVERED) === on) return;
    this.set(HOVERED, on);
    const f = on ? p.onHoverIn || p.onMouseEnter : p.onHoverOut || p.onMouseLeave;
    if (f) f(e);
  }

  click(e) {
    const p = this.props;
    if (p.onClick) p.onClick(e);
    if (this.disabled()) return;
    e.stopPropagation();
    // Native may synthesize a click after pointerup or Enter; those presses
    // already fired. A click with no press in flight is an activation from
    // the gamepad (A button) or the host.
    if (this.bits & PRESSED || now() - this.lastEnd < CLICK_DEDUPE_MS) return;
    this.pressIn(e, 'click');
    this.pressOut(e, true);
    if (this.setBits && this.mounted) {
      // Brief pressed flash so gamepad activation gets visual feedback.
      this.bits |= PRESSED;
      this.setBits(this.bits);
      clearTimeout(this.flashTimer);
      this.flashTimer = setTimeout(() => {
        this.flashTimer = null;
        if (!this.mounted) return;
        this.bits &= ~PRESSED;
        this.setBits(this.bits);
      }, FLASH_MS);
    }
  }

  keyDown(e) {
    const p = this.props;
    if (p.onKeyDown) p.onKeyDown(e);
    if (e.isDefaultPrevented() || this.disabled() || !isActivationKey(e)) return;
    e.preventDefault();  // we handle activation; native must not click as well
    e.stopPropagation();
    if (e.repeat || this.bits & PRESSED) return;
    this.pressIn(e, 'key');
  }

  keyUp(e) {
    const p = this.props;
    if (p.onKeyUp) p.onKeyUp(e);
    if (this.source !== 'key' || !isActivationKey(e)) return;
    e.preventDefault();
    e.stopPropagation();
    this.pressOut(e, true);
  }

  focus(on, e) {
    const p = this.props;
    this.set(FOCUSED, on);
    if (!on && this.source === 'key') this.pressOut(e, false);
    const f = on ? p.onFocus : p.onBlur;
    if (f) f(e);
  }

  dispose() {
    this.mounted = false;
    clearTimeout(this.longTimer);
    clearTimeout(this.flashTimer);
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
