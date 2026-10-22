// FlatList / SectionList / VirtualizedList. Not virtualized: every item is
// rendered (game menus are small; Yoga + the painter cull off-screen work),
// but items are memoized so re-rendering the list only re-renders changed rows.

import { createElement as h, Fragment, isValidElement, memo, useImperativeHandle, useRef, useCallback } from 'react';
import { ScrollView } from './basic.js';
import { measureNode } from '../instances.js';

function renderComponent(C, props) {
  if (C == null || C === false) return null;
  if (isValidElement(C)) return C;
  return h(C, props || null);
}

function defaultKeyExtractor(item, index) {
  if (item && typeof item === 'object') {
    if (item.key != null) return String(item.key);
    if (item.id != null) return String(item.id);
  }
  return String(index);
}

// One cell (like RN's CellRenderer: a View holding the item and the
// separator after it). Memoized on item identity + extraData so re-rendering
// the list only re-renders rows that changed.
function sameRow(a, b) {
  if (a === b) return true;
  if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const rowStyle = { flexDirection: 'row' };

const Cell = memo(
  function Cell({ item, index, renderItem, cellStyle, separators, cellRef, Separator, leadingItem, numColumns, columnWrapperStyle }) {
    let content;
    if (numColumns > 1) {
      content = h(
        'View',
        { style: [rowStyle, columnWrapperStyle] },
        item.map((it, k) => h(Fragment, { key: k }, renderItem({ item: it, index: index * numColumns + k, separators }))),
      );
    } else {
      content = renderItem({ item, index, separators });
    }
    return h('View', { style: cellStyle, ref: cellRef, collapsable: false }, content, Separator ? renderComponent(Separator, { highlighted: false, leadingItem }) : null);
  },
  (a, b) =>
    sameRow(a.item, b.item) &&
    a.index === b.index &&
    a.renderItem === b.renderItem &&
    a.extraData === b.extraData &&
    a.Separator === b.Separator &&
    a.cellStyle === b.cellStyle &&
    a.cellRef === b.cellRef &&
    a.columnWrapperStyle === b.columnWrapperStyle,
);

const flipX = { transform: [{ scaleX: -1 }] };
const flipY = { transform: [{ scaleY: -1 }] };
const noopSeparators = { highlight() {}, unhighlight() {}, updateProps() {} };

function useListHandle(ref, scrollRef, cellRefs, horizontal, getCount) {
  useImperativeHandle(ref, () => ({
    scrollToOffset({ offset = 0, animated = true } = {}) {
      const s = scrollRef.current;
      if (s) s.scrollTo(horizontal ? { x: offset, y: 0, animated } : { x: 0, y: offset, animated });
    },
    scrollToEnd(opts) {
      const s = scrollRef.current;
      if (s) s.scrollToEnd(opts);
    },
    scrollToIndex({ index, animated = true, viewOffset = 0, viewPosition = 0 }) {
      const s = scrollRef.current;
      const cell = cellRefs.current.get(index);
      if (!s || !cell) return;
      const c = measureNode(cell.id);
      const sv = measureNode(s.id);
      const content = s._content ? measureNode(s._content.id) : sv;
      const pos = horizontal ? c.pageX - content.pageX : c.pageY - content.pageY;
      const size = horizontal ? c.width : c.height;
      const view = horizontal ? sv.width : sv.height;
      const offset = Math.max(0, pos - viewPosition * (view - size) - viewOffset);
      s.scrollTo(horizontal ? { x: offset, y: 0, animated } : { x: 0, y: offset, animated });
    },
    scrollToItem({ item, animated, viewPosition }) {
      const i = getCount().data.indexOf(item);
      if (i >= 0) this.scrollToIndex({ index: i, animated, viewPosition });
    },
    flashScrollIndicators() {},
    recordInteraction() {},
    getScrollResponder: () => scrollRef.current,
    getNativeScrollRef: () => scrollRef.current,
    getScrollableNode: () => scrollRef.current && scrollRef.current.id,
  }));
}

function endReachedHandler(props, horizontal) {
  const { onEndReached, onEndReachedThreshold = 0.5, onScroll } = props;
  if (!onEndReached) return onScroll;
  let firedAt = -1;
  return (e) => {
    if (onScroll) onScroll(e);
    const ne = e.nativeEvent || {};
    const off = ne.contentOffset || { x: 0, y: 0 };
    const size = ne.contentSize || { width: 0, height: 0 };
    const view = ne.layoutMeasurement || { width: 0, height: 0 };
    const len = horizontal ? size.width : size.height;
    const dist = len - (horizontal ? off.x + view.width : off.y + view.height);
    if (dist < onEndReachedThreshold * (horizontal ? view.width : view.height)) {
      if (firedAt !== len) {
        firedAt = len;
        onEndReached({ distanceFromEnd: dist });
      }
    } else firedAt = -1;
  };
}

const LIST_OMIT = {
  data: 1, renderItem: 1, keyExtractor: 1, ItemSeparatorComponent: 1, ListHeaderComponent: 1, ListFooterComponent: 1,
  ListEmptyComponent: 1, ListHeaderComponentStyle: 1, ListFooterComponentStyle: 1, numColumns: 1, columnWrapperStyle: 1,
  extraData: 1, onEndReached: 1, onEndReachedThreshold: 1, initialNumToRender: 1, maxToRenderPerBatch: 1, windowSize: 1,
  getItemLayout: 1, removeClippedSubviews: 1, onViewableItemsChanged: 1, viewabilityConfig: 1, inverted: 1,
  initialScrollIndex: 1, refreshing: 1, onRefresh: 1, progressViewOffset: 1, getItem: 1, getItemCount: 1,
  updateCellsBatchingPeriod: 1, CellRendererComponent: 1, debug: 1, disableVirtualization: 1, sections: 1,
  renderSectionHeader: 1, renderSectionFooter: 1, SectionSeparatorComponent: 1, stickySectionHeadersEnabled: 1,
  ref: 1,
};

function scrollProps(props, horizontal, scrollRef) {
  const out = {};
  for (const k in props) if (!LIST_OMIT[k]) out[k] = props[k];
  out.horizontal = horizontal;
  out.ref = scrollRef;
  out.onScroll = endReachedHandler(props, horizontal);
  if (props.inverted) out.style = [props.style, horizontal ? flipX : flipY];
  return out;
}

export function FlatList(props) {
  const {
    data,
    renderItem,
    keyExtractor = defaultKeyExtractor,
    ItemSeparatorComponent,
    ListHeaderComponent,
    ListFooterComponent,
    ListEmptyComponent,
    ListHeaderComponentStyle,
    ListFooterComponentStyle,
    numColumns = 1,
    columnWrapperStyle,
    extraData,
    inverted,
    getItem,
    getItemCount,
  } = props;
  const horizontal = !!props.horizontal;
  const scrollRef = useRef(null);
  const cellRefs = useRef(new Map());
  const count = getItemCount ? getItemCount(data) : data ? data.length : 0;
  const itemAt = getItem ? (i) => getItem(data, i) : (i) => data[i];
  const latest = useRef(null);
  latest.current = { data: data || [] };
  useListHandle(props.ref, scrollRef, cellRefs, horizontal, () => latest.current);
  // Stable per-index ref callbacks so memoized cells stay memoized.
  const refCache = useRef([]);
  const cellRef = useCallback((i) => {
    let f = refCache.current[i];
    if (!f) {
      f = (inst) => {
        if (inst) cellRefs.current.set(i, inst);
        else cellRefs.current.delete(i);
      };
      refCache.current[i] = f;
    }
    return f;
  }, []);

  const flip = inverted ? (horizontal ? flipX : flipY) : null;
  const children = [];
  if (ListHeaderComponent) children.push(h('View', { key: '__header', style: [ListHeaderComponentStyle, flip] }, renderComponent(ListHeaderComponent)));
  if (count === 0 && ListEmptyComponent) children.push(h(Fragment, { key: '__empty' }, renderComponent(ListEmptyComponent)));
  if (renderItem) {
    const cols = numColumns > 1 ? numColumns : 1;
    const rows = Math.ceil(count / cols);
    for (let r = 0; r < rows; r++) {
      let item;
      let key;
      if (cols > 1) {
        item = [];
        key = '';
        for (let c = 0; c < cols && r * cols + c < count; c++) {
          const it = itemAt(r * cols + c);
          item.push(it);
          key += (c ? ':' : '') + keyExtractor(it, r * cols + c);
        }
      } else {
        item = itemAt(r);
        key = keyExtractor(item, r);
      }
      children.push(
        h(Cell, {
          key,
          item,
          index: r,
          renderItem,
          extraData,
          cellStyle: flip,
          separators: noopSeparators,
          cellRef: cellRef(r),
          Separator: r < rows - 1 ? ItemSeparatorComponent : null,
          leadingItem: item,
          numColumns: cols,
          columnWrapperStyle,
        }),
      );
    }
  }
  if (ListFooterComponent) children.push(h('View', { key: '__footer', style: [ListFooterComponentStyle, flip] }, renderComponent(ListFooterComponent)));
  return h(ScrollView, scrollProps(props, horizontal, scrollRef), children);
}
FlatList.displayName = 'FlatList';

export function VirtualizedList(props) {
  return h(FlatList, props);
}
VirtualizedList.displayName = 'VirtualizedList';

export function SectionList(props) {
  const {
    sections = [],
    renderItem,
    renderSectionHeader,
    renderSectionFooter,
    keyExtractor = defaultKeyExtractor,
    ItemSeparatorComponent,
    SectionSeparatorComponent,
    ListHeaderComponent,
    ListFooterComponent,
    ListEmptyComponent,
    extraData,
  } = props;
  const horizontal = !!props.horizontal;
  const scrollRef = useRef(null);
  const cellRefs = useRef(new Map());
  const latest = useRef(null);
  latest.current = { data: [] };
  const handle = useRef(null);
  useListHandle(handle, scrollRef, cellRefs, horizontal, () => latest.current);
  useImperativeHandle(props.ref, () => ({
    scrollToLocation({ sectionIndex = 0, itemIndex = 0, animated, viewOffset, viewPosition }) {
      let flat = 0;
      for (let s = 0; s < sectionIndex && s < sections.length; s++) flat += (sections[s].data || []).length;
      if (handle.current) handle.current.scrollToIndex({ index: flat + itemIndex, animated, viewOffset, viewPosition });
    },
    flashScrollIndicators() {},
    recordInteraction() {},
    getScrollResponder: () => scrollRef.current,
    getScrollableNode: () => scrollRef.current && scrollRef.current.id,
  }));
  // Per-section renderItem wrappers and per-index refs, cached so memoized
  // cells are not invalidated on every list render.
  const cache = useRef(null);
  if (!cache.current) cache.current = { renderers: new WeakMap(), refs: [] };
  const sectionRenderer = (section, render) => {
    let entry = cache.current.renderers.get(section);
    if (!entry || entry.render !== render) {
      entry = { render, fn: (info) => render(Object.assign(info, { section })) };
      cache.current.renderers.set(section, entry);
    }
    return entry.fn;
  };
  const sectionCellRef = (idx) =>
    cache.current.refs[idx] || (cache.current.refs[idx] = (inst) => (inst ? cellRefs.current.set(idx, inst) : cellRefs.current.delete(idx)));
  const children = [];
  if (ListHeaderComponent) children.push(h(Fragment, { key: '__header' }, renderComponent(ListHeaderComponent)));
  let total = 0;
  let flatIndex = 0;
  sections.forEach((section, si) => {
    const items = section.data || [];
    total += items.length;
    const sKey = section.key != null ? String(section.key) : 's' + si;
    if (si > 0 && SectionSeparatorComponent) children.push(h(Fragment, { key: sKey + '-ssep' }, renderComponent(SectionSeparatorComponent)));
    if (renderSectionHeader) children.push(h(Fragment, { key: sKey + '-header' }, renderSectionHeader({ section })));
    const Sep = section.ItemSeparatorComponent !== undefined ? section.ItemSeparatorComponent : ItemSeparatorComponent;
    const renderWithSection = sectionRenderer(section, section.renderItem || renderItem);
    items.forEach((item, i) => {
      const key = sKey + '-' + (section.keyExtractor || keyExtractor)(item, i);
      const idx = flatIndex++;
      children.push(
        h(Cell, {
          key,
          item,
          index: i,
          extraData,
          renderItem: renderWithSection,
          separators: noopSeparators,
          cellRef: sectionCellRef(idx),
          Separator: i < items.length - 1 ? Sep : null,
          leadingItem: item,
          numColumns: 1,
        }),
      );
    });
    if (renderSectionFooter) children.push(h(Fragment, { key: sKey + '-footer' }, renderSectionFooter({ section })));
  });
  if (total === 0 && ListEmptyComponent) children.push(h(Fragment, { key: '__empty' }, renderComponent(ListEmptyComponent)));
  if (ListFooterComponent) children.push(h(Fragment, { key: '__footer' }, renderComponent(ListFooterComponent)));
  return h(ScrollView, scrollProps(props, horizontal, scrollRef), children);
}
SectionList.displayName = 'SectionList';
