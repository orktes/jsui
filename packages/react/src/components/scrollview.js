// ScrollView: the host 'ScrollView' node (native scrolling, flings, snapping,
// indicators; see src/input/scroll.cpp) wrapped like React Native /
// react-native-web: a content container View, RN props and events.
//
// Native handles: horizontal, scrollEnabled, pagingEnabled, snapToInterval,
// snapToOffsets, snapToAlignment, snapToStart, snapToEnd,
// disableIntervalMomentum, decelerationRate, shows{Vertical,Horizontal}
// ScrollIndicator, indicatorStyle (all passed through as host props).
// Here: contentOffset (initial), scrollEventThrottle, stickyHeaderIndices,
// onContentSizeChange, the drag / momentum events and the ref methods.

import { createElement as h, Children, useLayoutEffect, useRef } from 'react';
import { native } from '../native.js';
import { omit } from './usePress.js';

const scrollBase = { flexGrow: 1, flexShrink: 1, flexDirection: 'column', overflow: 'scroll' };
const scrollBaseH = { flexGrow: 1, flexShrink: 1, flexDirection: 'row', overflow: 'scroll' };
const contentH = { flexDirection: 'row' };
const centerStyle = { flexGrow: 1, justifyContent: 'center' };
// Like react-native-web's `position: sticky; top: 0; zIndex: 10` wrapper.
const stickyStyle = { zIndex: 10 };

const SCROLL_IGNORED = [
  'contentContainerStyle',
  'onContentSizeChange',
  'refreshControl',
  'keyboardShouldPersistTaps',
  'keyboardDismissMode',
  'stickyHeaderIndices',
  'StickyHeaderComponent',
  'invertStickyHeaders',
  'stickyHeaderHiddenOnScroll',
  'children',
  'centerContent',
  'contentOffset',
  'onScroll',
  'scrollEventThrottle',
  'ref',
];

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref && typeof ref === 'object') ref.current = value;
}

export function ScrollView(props) {
  const { children, contentContainerStyle, horizontal, onContentSizeChange, refreshControl, centerContent, stickyHeaderIndices, pagingEnabled } = props;
  const latest = useRef(props);
  latest.current = props;
  const state = useRef(null);
  if (state.current === null) {
    state.current = {
      host: null,
      userRef: undefined,
      // Throttle (scrollEventThrottle) bookkeeping.
      lastEmit: -Infinity,
      pending: null,
      timer: null,
      // Sticky headers: index -> {inst, y, height, offset}; stable wrapper refs.
      sticky: new Map(),
      stickyRefs: new Map(),
      scrollY: 0,
    };
  }
  const st = state.current;

  // Stable ref callback: the host instance carries the ScrollView methods
  // (scrollTo, scrollToEnd, flashScrollIndicators, ...).
  const refCb = useRef(null);
  if (refCb.current === null) {
    refCb.current = (inst) => {
      st.host = inst;
      assignRef(latest.current.ref, inst);
      st.userRef = latest.current.ref;
    };
  }
  if (st.userRef !== props.ref && st.host) {
    // The user's ref object / callback changed between renders.
    assignRef(st.userRef, null);
    assignRef(props.ref, st.host);
    st.userRef = props.ref;
  }

  // Initial contentOffset (RN iOS / Android): applied once, before paint.
  useLayoutEffect(() => {
    const off = latest.current.contentOffset;
    if (off && st.host && typeof native.scrollTo === 'function') {
      native.scrollTo(st.host.id, +off.x || 0, +off.y || 0, false);
    }
    return () => {
      if (st.timer !== null) clearTimeout(st.timer);
      st.timer = null;
    };
  }, []);

  const updateSticky = () => {
    if (st.sticky.size === 0) return;
    const list = Array.from(st.sticky.values()).filter((s) => s.inst && s.y != null).sort((a, b) => a.y - b.y);
    for (let i = 0; i < list.length; i++) {
      const s = list[i];
      const next = list[i + 1];
      let t = Math.max(0, st.scrollY - s.y);
      // The next header pushes this one out (RN), instead of overlapping it.
      if (next) t = Math.min(t, Math.max(0, next.y - s.height - s.y));
      if (t !== s.offset) {
        s.offset = t;
        s.inst.patchStyle({ transform: t ? [{ translateY: t }] : null });
      }
    }
  };
  // React re-renders replace the wrappers' props: restore their offsets.
  useLayoutEffect(() => {
    for (const s of st.sticky.values()) s.offset = -1;
    updateSticky();
  });

  const emitScroll = (e) => {
    const p = latest.current;
    if (p.onScroll) p.onScroll(e);
  };
  const rest = omit(props, SCROLL_IGNORED);
  rest.ref = refCb.current;
  rest.horizontal = !!horizontal;
  rest.style = [horizontal ? scrollBaseH : scrollBase, props.style];
  const sticky = !horizontal && Array.isArray(stickyHeaderIndices) && stickyHeaderIndices.length > 0;
  if (props.onScroll || sticky) {
    rest.onScroll = (e) => {
      if (sticky) {
        st.scrollY = e.nativeEvent.contentOffset.y;
        updateSticky();
      }
      // scrollEventThrottle: at most one onScroll per interval, plus a
      // trailing one so the final offset is always delivered. 0 / unset:
      // every scroll event (React Native's current semantics).
      const throttle = +latest.current.scrollEventThrottle || 0;
      if (throttle <= 16) {
        emitScroll(e);
        return;
      }
      const t = e.timeStamp;
      if (t - st.lastEmit >= throttle) {
        st.lastEmit = t;
        st.pending = null;
        emitScroll(e);
        return;
      }
      st.pending = e;
      if (st.timer === null) {
        st.timer = setTimeout(() => {
          st.timer = null;
          const pe = st.pending;
          st.pending = null;
          if (pe) {
            st.lastEmit = pe.timeStamp;
            emitScroll(pe);
          }
        }, Math.max(0, throttle - (t - st.lastEmit)));
      }
    };
  }

  const stickyRef = (i) => {
    let fn = st.stickyRefs.get(i);
    if (!fn) {
      fn = (inst) => {
        let s = st.sticky.get(i);
        if (!inst) {
          if (s) s.inst = null;
          return;
        }
        if (!s) st.sticky.set(i, (s = { inst, y: null, height: 0, offset: 0 }));
        s.inst = inst;
        s.offset = -1;
      };
      st.stickyRefs.set(i, fn);
    }
    return fn;
  };

  // Children: sticky headers (and, like react-native-web, every child of a
  // paging ScrollView) are wrapped in a View.
  let kids = children;
  if (sticky || pagingEnabled) {
    kids = Children.map(children, (child, i) => {
      if (child == null) return child;
      const isSticky = sticky && stickyHeaderIndices.indexOf(i) >= 0;
      if (!isSticky && !pagingEnabled) return child;
      if (!isSticky) return h('View', { collapsable: false }, child);
      return h(
        'View',
        {
          style: stickyStyle,
          collapsable: false,
          ref: stickyRef(i),
          onLayout: (e) => {
            const s = st.sticky.get(i);
            if (!s) return;
            s.y = e.nativeEvent.layout.y;
            s.height = e.nativeEvent.layout.height;
            s.offset = -1;
            updateSticky();
          },
        },
        child,
      );
    });
  }

  const content = {
    style: [horizontal && contentH, centerContent && centerStyle, contentContainerStyle],
    collapsable: false,
  };
  if (onContentSizeChange) {
    content.onLayout = (e) => {
      const f = latest.current.onContentSizeChange;
      if (f) f(e.nativeEvent.layout.width, e.nativeEvent.layout.height);
    };
  }
  return h('ScrollView', rest, refreshControl || null, h('View', content, kids));
}
ScrollView.displayName = 'ScrollView';
