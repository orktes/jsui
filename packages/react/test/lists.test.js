// Virtualized FlatList / SectionList / VirtualizedList against the mock bridge.
// A tiny column layout engine stands in for Yoga: it positions the mock node
// tree, fires onLayout like native does, and scrollTo clamps + dispatches
// 'scroll' synchronously like InputManager::scrollTo.
import { mock } from './mock-native.js';
import { test, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { createElement as h, createRef } from 'react';
import { View, Text, Pressable, ScrollView, FlatList, SectionList, VirtualizedList, render } from '../src/index.js';

const VIEW_H = 600;
const VIEW_W = 800;
const native = globalThis.__jsui_native;
const lastRects = new Map(); // onLayout listeners: last reported rect
const rects = new Map(); // every node: last computed rect
const scrollState = new Map(); // scroll view id -> {x, y}

function styleOf(n) {
  return n.props.style || {};
}

/** Lays out the mock tree (vertical stacking, explicit heights) and fires onLayout. */
function layout() {
  const events = [];
  const visit = (id, x, y, w, horizontal) => {
    const n = mock.nodes.get(id);
    const st = styleOf(n);
    if (n.type === 'RawText' || st.display === 'none') return { w: 0, h: 0 };
    let size;
    if (n.type === 'ScrollView') {
      const hz = !!n.props.horizontal;
      let cx = 0;
      let cy = 0;
      for (const c of n.children) {
        const s = visit(c, cx, cy, hz ? 0 : w, hz);
        if (hz) cx += s.w;
        else cy += s.h;
      }
      size = { w: typeof st.width === 'number' ? st.width : VIEW_W, h: typeof st.height === 'number' ? st.height : VIEW_H };
    } else {
      const row = horizontal || st.flexDirection === 'row';
      let cx = 0;
      let cy = 0;
      let maxW = 0;
      let maxH = 0;
      for (const c of n.children) {
        const s = visit(c, cx, cy, w, false);
        if (row) cx += s.w;
        else cy += s.h;
        maxW = Math.max(maxW, s.w);
        maxH = Math.max(maxH, s.h);
      }
      size = {
        w: typeof st.width === 'number' ? st.width : row ? cx : maxW || w,
        h: typeof st.height === 'number' ? st.height : row ? maxH : cy,
      };
    }
    const rect = { x, y, width: size.w, height: size.h };
    rects.set(id, rect);
    if (n.props.__events && n.props.__events.includes('layout')) {
      const prev = lastRects.get(id);
      if (!prev || prev.x !== x || prev.y !== y || prev.width !== rect.width || prev.height !== rect.height) {
        lastRects.set(id, rect);
        events.push([id, rect]);
      }
    }
    return size;
  };
  for (const c of mock.root().children) visit(c, 0, 0, VIEW_W, false);
  for (const [id, rect] of events) if (mock.nodes.has(id)) globalThis.__jsui_onLayout(id, rect);
  return events.length;
}

function contentSize(sv) {
  const content = mock.nodes.get(sv.children[sv.children.length - 1]);
  const r = rects.get(content.id);
  if (r) return { width: r.width, height: r.height };
  return { width: VIEW_W, height: VIEW_H };
}

function dispatchScroll(sv) {
  const off = scrollState.get(sv.id) || { x: 0, y: 0 };
  mock.dispatch({ type: 'scroll', target: sv.id, contentOffset: off, contentSize: contentSize(sv), layoutMeasurement: { width: VIEW_W, height: VIEW_H } });
}

native.scrollTo = (id, x, y, animated) => {
  mock.calls.push(['scrollTo', id, x, y, animated]);
  const sv = mock.nodes.get(id);
  layout();
  const cs = contentSize(sv);
  const nx = Math.max(0, Math.min(x, cs.width - VIEW_W));
  const ny = Math.max(0, Math.min(y, cs.height - VIEW_H));
  const cur = scrollState.get(id) || { x: 0, y: 0 };
  if (cur.x === nx && cur.y === ny) return;
  scrollState.set(id, { x: nx, y: ny });
  dispatchScroll(sv);
};

function scrollView() {
  return mock.find((n) => n.type === 'ScrollView');
}

function scrollBy(y) {
  const sv = scrollView();
  native.scrollTo(sv.id, 0, y, false);
}

const tick = () => new Promise((r) => setTimeout(r, 3));

/** Runs layout + timers until the tree stops changing. */
async function settle(max = 60) {
  let quiet = 0;
  for (let i = 0; i < max && quiet < 3; i++) {
    const before = mock.calls.length;
    layout();
    await tick();
    quiet = mock.calls.length === before ? quiet + 1 : 0;
  }
}

function renderedIndices() {
  return mock
    .findAll((n) => typeof n.props.nativeID === 'string' && n.props.nativeID.startsWith('item-'))
    .map((n) => Number(n.props.nativeID.slice(5)))
    .sort((a, b) => a - b);
}

function reset() {
  render(null);
  lastRects.clear();
  rects.clear();
  scrollState.clear();
  mock.reset();
}

afterEach(reset);

const data10k = Array.from({ length: 10000 }, (_, i) => ({ id: 'k' + i, n: i }));
const Item = ({ item, height = 50 }) => h(View, { nativeID: 'item-' + item.n, style: { height } }, h(Text, null, 'Item ' + item.n));
const renderItem = ({ item }) => h(Item, { item });

test('FlatList with 10,000 items mounts only a window of cells (getItemLayout)', async () => {
  const getItemLayout = (_d, index) => ({ length: 50, offset: 50 * index, index });
  render(h(FlatList, { data: data10k, renderItem, getItemLayout, updateCellsBatchingPeriod: 1 }));
  assert.deepEqual(renderedIndices(), Array.from({ length: 10 }, (_, i) => i), 'initialNumToRender');
  await settle();
  const idx = renderedIndices();
  // windowSize 21 at the top: viewport + 10 viewports below = 6600px = 132 cells.
  assert.ok(idx.length >= 100 && idx.length <= 200, 'rendered ' + idx.length);
  assert.equal(idx[0], 0);
  // Spacers keep the full content size.
  assert.equal(contentSize(scrollView()).height, 500000);
  // Cells are not measured when getItemLayout is given.
  const cell = mock.nodes.get(mock.find((n) => n.props.nativeID === 'item-3').parent);
  assert.equal((cell.props.__events || []).includes('layout'), false);
  // Native node count stays small.
  assert.ok(mock.nodes.size < 1000, 'nodes ' + mock.nodes.size);

  // Scroll to the middle: the window moves, the initial cells stay mounted.
  scrollBy(250000);
  await settle();
  const mid = renderedIndices();
  assert.ok(mid.includes(5000) && mid.includes(5011), 'visible cells mounted');
  assert.ok(!mid.includes(200) && !mid.includes(9000));
  assert.ok(mid.includes(0) && mid.includes(9), 'initialNumToRender cells are kept');
  assert.ok(mid.length < 400, 'rendered ' + mid.length);
  assert.equal(contentSize(scrollView()).height, 500000);
  const item5000 = mock.find((n) => n.props.nativeID === 'item-5000');
  assert.equal(rects.get(item5000.parent).y, 250000, 'cell 5000 sits at its offset');
});

test('measured layouts: spacers use measured / average cell length', async () => {
  render(h(FlatList, { data: data10k, renderItem: ({ item }) => h(Item, { item, height: 40 }), updateCellsBatchingPeriod: 1 }));
  await settle();
  const idx = renderedIndices();
  assert.ok(idx.length > 15 && idx.length < 250, 'rendered ' + idx.length);
  assert.equal(contentSize(scrollView()).height, 400000);
  scrollBy(200000);
  await settle();
  const mid = renderedIndices();
  assert.ok(mid.includes(5000) && mid.includes(5014), 'visible cells mounted: ' + mid.slice(10, 20));
  const c = mock.find((n) => n.props.nativeID === 'item-5000');
  assert.equal(rects.get(c.parent).y, 200000);
  assert.equal(contentSize(scrollView()).height, 400000);
});

test('separators, header, footer and empty with virtualization', async () => {
  const data = Array.from({ length: 500 }, (_, i) => ({ id: 'x' + i, n: i }));
  render(
    h(FlatList, {
      data,
      renderItem,
      ItemSeparatorComponent: () => h(View, { nativeID: 'sep', style: { height: 2 } }),
      ListHeaderComponent: h(View, { nativeID: 'head', style: { height: 100 } }),
      ListFooterComponent: () => h(View, { nativeID: 'foot', style: { height: 30 } }),
      updateCellsBatchingPeriod: 1,
    }),
  );
  await settle();
  const n = renderedIndices().length;
  assert.equal(mock.findAll((x) => x.props.nativeID === 'sep').length, n, 'one separator per mounted cell (not after the last item)');
  assert.ok(mock.find((x) => x.props.nativeID === 'head'));
  assert.ok(mock.find((x) => x.props.nativeID === 'foot'));
  // Header + 500 * 52 - last separator + footer (the trailing spacer is an
  // estimate from the average cell length, which includes separators).
  assert.ok(Math.abs(contentSize(scrollView()).height - (100 + 500 * 52 - 2 + 30)) <= 2);
  render(h(FlatList, { data: [], renderItem, ListEmptyComponent: () => h(Text, null, 'empty') }));
  assert.equal(mock.text(scrollView().id), 'empty');
});

test('scrolling down renders ahead; onEndReached fires once per content length', async () => {
  const calls = [];
  let data = data10k.slice(0, 100);
  const props = () => ({ data, renderItem, onEndReached: (e) => calls.push(e.distanceFromEnd), onEndReachedThreshold: 0.5, updateCellsBatchingPeriod: 1, windowSize: 5 });
  render(h(FlatList, props()));
  await settle();
  assert.equal(calls.length, 0, 'not at the end yet');
  // 100 * 50 = 5000; end threshold 0.5 * 600 = 300.
  scrollBy(4000);
  await settle();
  assert.equal(calls.length, 0);
  scrollBy(4150);
  await settle();
  assert.equal(calls.length, 1);
  scrollBy(4200);
  await settle();
  assert.equal(calls.length, 1, 'same content length: no repeat');
  data = data10k.slice(0, 200);
  render(h(FlatList, props()));
  await settle();
  scrollBy(10000);
  await settle();
  assert.equal(calls.length, 2, 'fires again after more data');
});

test('scrollToIndex / scrollToOffset / scrollToEnd / scrollToItem via ref', async () => {
  const ref = createRef();
  const getItemLayout = (_d, index) => ({ length: 50, offset: 50 * index, index });
  render(h(FlatList, { ref, data: data10k, renderItem, getItemLayout, updateCellsBatchingPeriod: 1 }));
  await settle();
  const sv = scrollView();
  mock.reset();
  ref.current.scrollToIndex({ index: 3000, animated: false });
  assert.deepEqual(mock.calls.find((c) => c[0] === 'scrollTo'), ['scrollTo', sv.id, 0, 150000, false]);
  await settle();
  assert.ok(renderedIndices().includes(3000));
  mock.reset();
  ref.current.scrollToIndex({ index: 100, viewPosition: 0.5 });
  assert.deepEqual(mock.calls.find((c) => c[0] === 'scrollTo'), ['scrollTo', sv.id, 0, 5000 - 0.5 * (600 - 50), true]);
  mock.reset();
  ref.current.scrollToOffset({ offset: 1234, animated: false });
  assert.deepEqual(mock.calls.find((c) => c[0] === 'scrollTo'), ['scrollTo', sv.id, 0, 1234, false]);
  mock.reset();
  ref.current.scrollToItem({ item: data10k[42], animated: false });
  assert.deepEqual(mock.calls.find((c) => c[0] === 'scrollTo'), ['scrollTo', sv.id, 0, 2100, false]);
  ref.current.scrollToEnd({ animated: false });
  await settle();
  assert.equal(scrollState.get(sv.id).y, 500000 - 600);
  assert.ok(renderedIndices().includes(9999));
  assert.equal(typeof ref.current.getScrollableNode(), 'number');
});

test('scrollToIndex without getItemLayout estimates, then corrects once measured', async () => {
  const ref = createRef();
  // Variable heights: 30 for even, 70 for odd items (average 50).
  const varItem = ({ item }) => h(Item, { item, height: item.n % 2 ? 70 : 30 });
  render(h(FlatList, { ref, data: data10k, renderItem: varItem, updateCellsBatchingPeriod: 1, initialNumToRender: 20 }));
  await settle();
  ref.current.scrollToIndex({ index: 4001, animated: false });
  await settle();
  const sv = scrollView();
  const cell = mock.find((n) => n.props.nativeID === 'item-4001');
  assert.ok(cell, 'target mounted');
  assert.equal(scrollState.get(sv.id).y, rects.get(cell.parent).y, 'scrolled exactly to the measured cell');
});

test('onScrollToIndexFailed is called for unmeasured indices when provided', async () => {
  const ref = createRef();
  const failed = [];
  render(h(FlatList, { ref, data: data10k, renderItem, onScrollToIndexFailed: (i) => failed.push(i), updateCellsBatchingPeriod: 1 }));
  await settle();
  ref.current.scrollToIndex({ index: 8000 });
  assert.equal(failed.length, 1);
  assert.equal(failed[0].index, 8000);
  assert.equal(failed[0].averageItemLength, 50);
  assert.ok(failed[0].highestMeasuredFrameIndex > 10);
});

test('onViewableItemsChanged reports items entering and leaving the viewport', async () => {
  const events = [];
  const onViewableItemsChanged = ({ viewableItems, changed }) =>
    events.push({ viewable: viewableItems.map((v) => v.index), changed: changed.map((c) => [c.index, c.isViewable]) });
  render(h(FlatList, { data: data10k, renderItem, onViewableItemsChanged, viewabilityConfig: { itemVisiblePercentThreshold: 50 }, updateCellsBatchingPeriod: 1 }));
  await settle();
  assert.ok(events.length >= 1);
  assert.deepEqual(events[events.length - 1].viewable, Array.from({ length: 12 }, (_, i) => i));
  events.length = 0;
  scrollBy(110); // item 2 is 40% visible, item 13 is 60% visible
  await settle();
  const e = events[events.length - 1];
  assert.deepEqual(e.viewable, Array.from({ length: 12 }, (_, i) => i + 2));
  assert.deepEqual(e.changed.sort((a, b) => a[0] - b[0]), [[0, false], [1, false], [12, true], [13, true]]);
});

test('focus inside a cell keeps it mounted while the list scrolls away', async () => {
  const pressItem = ({ item }) => h(Pressable, { nativeID: 'item-' + item.n, style: { height: 50 }, onPress: () => {} }, h(Text, null, String(item.n)));
  render(h(FlatList, { data: data10k, renderItem: pressItem, updateCellsBatchingPeriod: 1, initialNumToRender: 5, windowSize: 3 }));
  await settle();
  const target = mock.find((n) => n.props.nativeID === 'item-20');
  mock.dispatch({ type: 'focus', target: target.id });
  scrollBy(100000);
  await settle();
  const idx = renderedIndices();
  assert.ok(idx.includes(19) && idx.includes(20) && idx.includes(21), 'focused cell and neighbours stay: ' + idx.slice(0, 10));
  assert.ok(!idx.includes(30));
  assert.ok(idx.includes(2000));
});

test('numColumns: rows are virtualized', async () => {
  render(h(FlatList, { data: data10k, numColumns: 4, renderItem, updateCellsBatchingPeriod: 1 }));
  await settle();
  const idx = renderedIndices();
  assert.equal(idx.length % 4, 0);
  assert.ok(idx.length < 800, 'rendered ' + idx.length);
  // 2500 rows of 50.
  assert.equal(contentSize(scrollView()).height, 125000);
});

test('horizontal and inverted lists', async () => {
  render(h(FlatList, { horizontal: true, data: data10k, renderItem, getItemLayout: (_d, i) => ({ length: 50, offset: 50 * i, index: i }) }));
  const sv = scrollView();
  assert.equal(sv.props.horizontal, true);
  const content = mock.nodes.get(sv.children[0]);
  const spacer = mock.nodes.get(content.children[content.children.length - 1]);
  assert.deepEqual(spacer.props.style, { width: 50 * (10000 - 10) });
  const cell = mock.nodes.get(content.children[0]);
  assert.equal(cell.props.style.flexDirection, 'row');
  render(h(FlatList, { inverted: true, data: data10k.slice(0, 5), renderItem }));
  const sv2 = scrollView();
  assert.deepEqual(sv2.props.style.transform, [{ scaleY: -1 }]);
  const c2 = mock.nodes.get(mock.nodes.get(sv2.children[0]).children[0]);
  assert.deepEqual(c2.props.style.transform, [{ scaleY: -1 }]);
});

test('VirtualizedList with getItem / getItemCount', async () => {
  const ref = createRef();
  render(
    h(VirtualizedList, {
      ref,
      data: null,
      getItemCount: () => 100000,
      getItem: (_d, i) => ({ id: 'v' + i, n: i }),
      renderItem,
      updateCellsBatchingPeriod: 1,
    }),
  );
  await settle();
  assert.ok(renderedIndices().length < 250);
  assert.equal(contentSize(scrollView()).height, 5000000);
  ref.current.scrollToIndex({ index: 50000, animated: false });
  await settle();
  assert.ok(renderedIndices().includes(50000));
});

test('SectionList: virtualized sections, separators, scrollToLocation', async () => {
  const ref = createRef();
  const sections = Array.from({ length: 100 }, (_, s) => ({
    key: 's' + s,
    title: 'S' + s,
    data: Array.from({ length: 100 }, (_, i) => ({ id: s + '-' + i, n: s * 100 + i })),
  }));
  render(
    h(SectionList, {
      ref,
      sections,
      renderItem: ({ item, index, section }) => {
        assert.equal(section.data[index], item);
        return h(Item, { item });
      },
      renderSectionHeader: ({ section }) => h(View, { nativeID: 'hdr-' + section.title, style: { height: 20 } }),
      ItemSeparatorComponent: () => h(View, { nativeID: 'isep', style: { height: 1 } }),
      SectionSeparatorComponent: () => h(View, { nativeID: 'ssep', style: { height: 3 } }),
      updateCellsBatchingPeriod: 1,
    }),
  );
  await settle();
  const idx = renderedIndices();
  assert.ok(idx.length > 10 && idx.length < 300, 'rendered ' + idx.length);
  assert.ok(mock.find((n) => n.props.nativeID === 'hdr-S0'));
  assert.ok(!mock.find((n) => n.props.nativeID === 'hdr-S50'));
  // Section separator above the first item (after the header).
  const first = mock.find((n) => n.props.nativeID === 'item-0');
  const cell = mock.nodes.get(first.parent);
  assert.equal(mock.nodes.get(cell.children[0]).props.nativeID, 'ssep');
  // Item separators between items, not after the last item of a section.
  assert.equal(mock.nodes.get(cell.children[2]).props.nativeID, 'isep');
  ref.current.scrollToLocation({ sectionIndex: 50, itemIndex: 0, animated: false });
  await settle();
  assert.ok(mock.find((n) => n.props.nativeID === 'hdr-S50'), 'section 50 header mounted');
  assert.ok(renderedIndices().includes(5000));
});

test('SectionList sticky headers pin and get pushed by the next header', async () => {
  const sections = Array.from({ length: 20 }, (_, s) => ({
    title: 'S' + s,
    data: Array.from({ length: 10 }, (_, i) => ({ id: s + '-' + i, n: s * 10 + i })),
  }));
  render(
    h(SectionList, {
      sections,
      renderItem,
      renderSectionHeader: ({ section }) => h(View, { nativeID: 'hdr-' + section.title, style: { height: 20 } }),
      stickySectionHeadersEnabled: true,
      getItemLayout: undefined,
      updateCellsBatchingPeriod: 1,
    }),
  );
  await settle();
  // Section = 20 + 10*50 + 0 (footer) = 520 px. Scroll into section 0.
  scrollBy(100);
  await settle();
  const hdr = (t) => mock.find((n) => n.props.nativeID === 'hdr-' + t);
  const wrapperOf = (n) => {
    let p = mock.nodes.get(n.parent);
    while (p && !(p.props.style && p.props.style.zIndex === 10)) p = mock.nodes.get(p.parent);
    return p;
  };
  assert.deepEqual(wrapperOf(hdr('S0')).props.style.transform, [{ translateY: 100 }]);
  // Next header at 520 pushes S0 up once the offset passes 500.
  scrollBy(510);
  await settle();
  assert.deepEqual(wrapperOf(hdr('S0')).props.style.transform, [{ translateY: 500 }]);
  // Far down (estimated region): the header of the section under the top
  // edge is mounted and pinned exactly at the viewport top.
  scrollBy(520 * 15 + 10);
  await settle();
  const y = scrollState.get(scrollView().id).y;
  const headers = mock.findAll((n) => typeof n.props.nativeID === 'string' && n.props.nativeID.startsWith('hdr-')).map(wrapperOf);
  const current = headers.filter((w) => rects.get(w.id).y <= y).sort((a, b) => rects.get(b.id).y - rects.get(a.id).y)[0];
  assert.ok(current, 'current section header mounted');
  const next = headers.filter((w) => rects.get(w.id).y > y).sort((a, b) => rects.get(a.id).y - rects.get(b.id).y)[0];
  const cy = rects.get(current.id).y;
  const expected = Math.min(y - cy, next ? rects.get(next.id).y - cy - 20 : Infinity);
  assert.deepEqual(current.props.style.transform, [{ translateY: expected }]);
  assert.ok(Math.abs(cy - 520 * 15) < 520, 'near section 15');
});

test('extraData re-renders mounted cells; unchanged rows are memoized', async () => {
  let renders = 0;
  const data = data10k.slice(0, 50);
  const ri = ({ item }) => {
    renders++;
    return h(Item, { item });
  };
  render(h(FlatList, { data, renderItem: ri, extraData: 1, updateCellsBatchingPeriod: 1 }));
  await settle();
  const mounted = renderedIndices().length;
  renders = 0;
  render(h(FlatList, { data, renderItem: ri, extraData: 1, updateCellsBatchingPeriod: 1 }));
  await settle();
  assert.equal(renders, 0, 'same props: no cell renders');
  render(h(FlatList, { data, renderItem: ri, extraData: 2, updateCellsBatchingPeriod: 1 }));
  await settle();
  assert.equal(renders, mounted);
});
