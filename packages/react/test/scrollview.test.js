import { mock } from './mock-native.js';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h } from 'react';
import { View, Text, ScrollView, render } from '../src/index.js';

const wait = (ms = 0) => new Promise((r) => setTimeout(r, ms));
const scrollNode = () => mock.find((n) => n.type === 'ScrollView');
const scrollEvent = (target, y, timestamp, type = 'scroll', extra = {}) =>
  mock.dispatch(
    Object.assign(
      { type, target, timestamp, contentOffset: { x: 0, y }, contentSize: { width: 100, height: 1000 }, layoutMeasurement: { width: 100, height: 100 } },
      extra,
    ),
  );

test('ScrollView maps drag / momentum events to RN props in order', () => {
  const log = [];
  render(
    h(
      ScrollView,
      {
        onScrollBeginDrag: (e) => log.push('begin:' + e.nativeEvent.contentOffset.y),
        onScrollEndDrag: (e) => log.push('end:' + e.nativeEvent.velocity.y + '->' + e.nativeEvent.targetContentOffset.y),
        onMomentumScrollBegin: () => log.push('momentumBegin'),
        onMomentumScrollEnd: (e) => log.push('momentumEnd:' + e.nativeEvent.contentOffset.y),
        onScroll: (e) => log.push('scroll:' + e.nativeEvent.contentOffset.y),
      },
      h(View),
    ),
  );
  const sv = scrollNode();
  scrollEvent(sv.id, 0, 1, 'scrollbegindrag');
  scrollEvent(sv.id, 30, 2);
  scrollEvent(sv.id, 60, 3, 'scrollenddrag', { velocity: { x: 0, y: 1.25 }, targetContentOffset: { x: 0, y: 684 } });
  scrollEvent(sv.id, 60, 4, 'momentumscrollbegin');
  scrollEvent(sv.id, 90, 5);
  scrollEvent(sv.id, 684, 6, 'momentumscrollend');
  assert.deepEqual(log, ['begin:0', 'scroll:30', 'end:1.25->684', 'momentumBegin', 'scroll:90', 'momentumEnd:684']);
  render(null);
});

test('scrollEventThrottle limits onScroll and delivers the last offset', async () => {
  // The runtime throttles (src/input/scroll.cpp, tests/native/scroll_test.cpp):
  // the host node gets the interval, and what it sends reaches onScroll.
  const seen = [];
  render(h(ScrollView, { scrollEventThrottle: 100, onScroll: (e) => seen.push(e.nativeEvent.contentOffset.y) }, h(View)));
  const sv = scrollNode();
  assert.equal(sv.props.scrollEventThrottle, 100);
  assert.ok(sv.props.__events.includes('scroll'));
  scrollEvent(sv.id, 10, 1000);
  scrollEvent(sv.id, 20, 1020);
  assert.deepEqual(seen, [10, 20]);
  render(null);
  // Sticky headers follow every offset: the host sends all of them and JS
  // throttles onScroll, with a trailing event for the latest offset.
  const sticky = [];
  render(h(ScrollView, { stickyHeaderIndices: [0], scrollEventThrottle: 100, onScroll: (e) => sticky.push(e.nativeEvent.contentOffset.y) }, h(View), h(View)));
  const sv1 = scrollNode();
  assert.equal(sv1.props.scrollEventThrottle, undefined);
  scrollEvent(sv1.id, 10, 1000);
  scrollEvent(sv1.id, 20, 1020);
  scrollEvent(sv1.id, 30, 1040);
  assert.deepEqual(sticky, [10]);
  await wait(120);
  assert.deepEqual(sticky, [10, 30]); // trailing event with the latest offset
  scrollEvent(sv1.id, 40, 1200);
  assert.deepEqual(sticky, [10, 30, 40]);
  render(null);
  // Unset throttle: every event.
  const all = [];
  render(h(ScrollView, { onScroll: (e) => all.push(e.nativeEvent.contentOffset.y) }, h(View)));
  const sv2 = scrollNode();
  for (let i = 1; i <= 3; i++) scrollEvent(sv2.id, i, 2000 + i);
  assert.deepEqual(all, [1, 2, 3]);
  render(null);
});

test('contentOffset is applied on mount; ref methods reach native', () => {
  const ref = { current: null };
  mock.reset();
  render(h(ScrollView, { ref, contentOffset: { x: 0, y: 120 }, showsVerticalScrollIndicator: false, pagingEnabled: true, snapToInterval: 50, decelerationRate: 'fast' }, h(View)));
  const sv = scrollNode();
  assert.deepEqual(
    mock.calls.find((c) => c[0] === 'scrollTo'),
    ['scrollTo', sv.id, 0, 120, false],
  );
  // Native-handled props pass through; contentOffset does not.
  assert.equal(sv.props.showsVerticalScrollIndicator, false);
  assert.equal(sv.props.pagingEnabled, true);
  assert.equal(sv.props.snapToInterval, 50);
  assert.equal(sv.props.decelerationRate, 'fast');
  assert.equal(sv.props.contentOffset, undefined);
  mock.reset();
  ref.current.scrollTo({ y: 300, animated: true });
  ref.current.scrollTo({ x: 0, y: 10, animated: false });
  ref.current.flashScrollIndicators();
  assert.deepEqual(mock.calls, [
    ['scrollTo', sv.id, 0, 300, true],
    ['scrollTo', sv.id, 0, 10, false],
    ['flashScrollIndicators', sv.id],
  ]);
  // Re-renders keep the ref; a callback ref gets the instance too.
  let cbInst = null;
  render(h(ScrollView, { ref: (i) => (cbInst = i) }, h(View)));
  assert.equal(cbInst && cbInst.id, scrollNode().id);
  render(null);
});

test('pagingEnabled wraps children like react-native-web', () => {
  render(h(ScrollView, { pagingEnabled: true, horizontal: true }, h(View, { nativeID: 'a' }), h(View, { nativeID: 'b' })));
  const content = mock.nodes.get(scrollNode().children[0]);
  assert.equal(content.children.length, 2);
  for (const id of content.children) {
    const wrapper = mock.nodes.get(id);
    assert.equal(wrapper.type, 'View');
    assert.equal(wrapper.children.length, 1);
  }
  render(null);
});

test('stickyHeaderIndices pin headers and the next header pushes the previous one', () => {
  render(
    h(
      ScrollView,
      { stickyHeaderIndices: [0, 2] },
      h(Text, null, 'A'),
      h(View, { style: { height: 200 } }),
      h(Text, null, 'B'),
      h(View, { style: { height: 200 } }),
    ),
  );
  const sv = scrollNode();
  const content = mock.nodes.get(sv.children[0]);
  const [w0, , w2] = content.children.map((id) => mock.nodes.get(id));
  assert.equal(w0.props.style.zIndex, 10);
  assert.equal(w2.props.style.zIndex, 10);
  globalThis.__jsui_onLayout(w0.id, { x: 0, y: 0, width: 100, height: 20 });
  globalThis.__jsui_onLayout(w2.id, { x: 0, y: 220, width: 100, height: 20 });
  const ty = (n) => {
    const t = mock.nodes.get(n.id).props.style.transform;
    return t ? t[0].translateY : 0;
  };
  scrollEvent(sv.id, 50, 1);
  assert.equal(ty(w0), 50);
  assert.equal(ty(w2), 0);
  scrollEvent(sv.id, 210, 2); // B (220) is 10 px from the top: pushes A up
  assert.equal(ty(w0), 200);
  scrollEvent(sv.id, 300, 3);
  assert.equal(ty(w0), 200);
  assert.equal(ty(w2), 80);
  scrollEvent(sv.id, 0, 4);
  assert.equal(ty(w0), 0);
  assert.equal(ty(w2), 0);
  render(null);
});
