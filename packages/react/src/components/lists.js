// VirtualizedList / FlatList / SectionList, virtualized like React Native.
//
// Ported from RN's VirtualizedList design: only a window of cells around the
// viewport is mounted (`windowSize` viewports, grown by `maxToRenderPerBatch`
// cells every `updateCellsBatchingPeriod` ms), unmounted ranges are replaced
// by spacer Views sized from `getItemLayout` or from measured cell layouts
// (`onLayout` per cell; unmeasured cells use the average measured length).
// The first `initialNumToRender` cells stay mounted (fast scroll-to-top), as
// do the last focused cell and its neighbours (gamepad / keyboard focus
// navigation keeps working while the list scrolls under it), and the closest
// sticky header before the window.
//
// Game focus navigation: native spatial navigation only sees mounted cells,
// and the window always extends past the viewport, so moving focus onto the
// next off-screen cell works; native `scrollIntoView` scrolls it in, the
// scroll event moves the window and the following cells mount.

import { createElement as h, Component, Fragment, isValidElement, cloneElement, memo, useState, useMemo, useCallback } from 'react';
import { ScrollView } from './basic.js';
import { instances } from '../instances.js';
import { addGlobalListener } from '../events.js';
import { warnOnce } from '../native.js';

// --- helpers -------------------------------------------------------------------

function renderComponent(C, props) {
  if (C == null || C === false) return null;
  if (isValidElement(C)) return C;
  return h(C, props || null);
}

/** RN's default keyExtractor: item.key, then item.id, then the index. */
export function defaultKeyExtractor(item, index) {
  if (typeof item === 'object' && item != null) {
    if (item.key != null) return String(item.key);
    if (item.id != null) return String(item.id);
  }
  return String(index);
}

const num = (v, d) => (typeof v === 'number' && v === v ? v : d);

const styles = {
  row: { flexDirection: 'row' },
  rowReverse: { flexDirection: 'row-reverse' },
  columnReverse: { flexDirection: 'column-reverse' },
  verticallyInverted: { transform: [{ scaleY: -1 }] },
  horizontallyInverted: { transform: [{ scaleX: -1 }] },
};

// Props consumed by the list that must not reach the ScrollView host node
// (they would be serialized over the bridge).
const LIST_OMIT = {
  data: 1, renderItem: 1, keyExtractor: 1, ItemSeparatorComponent: 1, ListHeaderComponent: 1, ListFooterComponent: 1,
  ListEmptyComponent: 1, ListHeaderComponentStyle: 1, ListFooterComponentStyle: 1, ListItemComponent: 1, numColumns: 1,
  columnWrapperStyle: 1, extraData: 1, onEndReached: 1, onEndReachedThreshold: 1, onStartReached: 1,
  onStartReachedThreshold: 1, initialNumToRender: 1, maxToRenderPerBatch: 1, windowSize: 1, getItemLayout: 1,
  removeClippedSubviews: 1, onViewableItemsChanged: 1, viewabilityConfig: 1, viewabilityConfigCallbackPairs: 1,
  inverted: 1, initialScrollIndex: 1, refreshing: 1, onRefresh: 1, progressViewOffset: 1, getItem: 1, getItemCount: 1,
  updateCellsBatchingPeriod: 1, CellRendererComponent: 1, debug: 1, disableVirtualization: 1, sections: 1,
  renderSectionHeader: 1, renderSectionFooter: 1, SectionSeparatorComponent: 1, stickySectionHeadersEnabled: 1,
  stickyHeaderIndices: 1, onScrollToIndexFailed: 1, renderScrollComponent: 1, persistentScrollbar: 1,
  fadingEdgeLength: 1, strictMode: 1, ref: 1, children: 1, listKey: 1, maintainVisibleContentPosition: 1,
  onLayout: 1, onScroll: 1, onContentSizeChange: 1, onScrollBeginDrag: 1, onScrollEndDrag: 1, onMomentumScrollEnd: 1,
  style: 1, horizontal: 1,
};

// --- window computation (RN VirtualizeUtils) ------------------------------------

function newRangeCount(prev, next) {
  return next.last - next.first + 1 - Math.max(0, 1 + Math.min(next.last, prev.last) - Math.max(next.first, prev.first));
}

/**
 * Indices of the cells containing each offset (sorted ascending), by binary
 * search over approximate frames. Missing entries stay undefined.
 */
function elementsThatOverlapOffsets(offsets, count, frameOf) {
  const out = [];
  for (let k = 0; k < offsets.length; k++) {
    const target = offsets[k];
    let lo = 0;
    let hi = count - 1;
    while (lo <= hi) {
      const mid = lo + ((hi - lo) >>> 1);
      const f = frameOf(mid);
      if (target < f.offset) hi = mid - 1;
      else if (target >= f.offset + f.length) lo = mid + 1;
      else {
        out[k] = mid;
        break;
      }
    }
    if (out[k] === undefined && lo < count && lo > 0) {
      // In a gap between frames (zero length cells, approximations): use
      // the cell that starts after the offset.
      out[k] = Math.min(lo, count - 1);
    }
  }
  return out;
}

/** RN computeWindowedRenderLimits: the cells to keep mounted around the viewport. */
export function computeWindowedRenderLimits(count, maxToRenderPerBatch, windowSize, prev, frameOf, metrics) {
  if (count === 0) return { first: 0, last: -1 };
  const { offset, visibleLength, velocity } = metrics;
  const visibleBegin = Math.max(0, offset);
  const visibleEnd = visibleBegin + visibleLength;
  const overscanLength = (windowSize - 1) * visibleLength;
  const leadFactor = 0.5;
  const fillPreference = velocity > 1 ? 'after' : velocity < -1 ? 'before' : 'none';
  const overscanBegin = Math.max(0, visibleBegin - (1 - leadFactor) * overscanLength);
  const overscanEnd = Math.max(0, visibleEnd + leadFactor * overscanLength);
  const lastFrame = frameOf(count - 1);
  if (lastFrame.offset < overscanBegin) {
    // Entire list is before the window (scrolled past the content).
    return { first: Math.max(0, count - 1 - maxToRenderPerBatch), last: count - 1 };
  }
  let [overscanFirst, first, last, overscanLast] = elementsThatOverlapOffsets([overscanBegin, visibleBegin, visibleEnd, overscanEnd], count, frameOf);
  overscanFirst = overscanFirst == null ? 0 : overscanFirst;
  first = first == null ? Math.max(0, overscanFirst) : first;
  overscanLast = overscanLast == null ? count - 1 : overscanLast;
  last = last == null ? Math.min(overscanLast, first + maxToRenderPerBatch - 1) : last;
  const visible = { first, last };
  let newCellCount = newRangeCount(prev, visible);
  for (;;) {
    if (first <= overscanFirst && last >= overscanLast) break;
    const maxNewCells = newCellCount >= maxToRenderPerBatch;
    const firstWillAddMore = first <= prev.first || first > prev.last;
    const firstShouldIncrement = first > overscanFirst && (!maxNewCells || !firstWillAddMore);
    const lastWillAddMore = last >= prev.last || last < prev.first;
    const lastShouldIncrement = last < overscanLast && (!maxNewCells || !lastWillAddMore);
    if (maxNewCells && !firstShouldIncrement && !lastShouldIncrement) break;
    if (firstShouldIncrement && !(fillPreference === 'after' && lastShouldIncrement && lastWillAddMore)) {
      if (firstWillAddMore) newCellCount++;
      first--;
    }
    if (lastShouldIncrement && !(fillPreference === 'before' && firstShouldIncrement && firstWillAddMore)) {
      if (lastWillAddMore) newCellCount++;
      last++;
    }
  }
  if (!(last >= first && first >= 0 && last < count && first >= overscanFirst && last <= overscanLast && first <= visible.first && last >= visible.last)) {
    // Defensive: never return an invalid range.
    first = Math.max(0, Math.min(first, count - 1));
    last = Math.max(first, Math.min(last, count - 1));
  }
  return { first, last };
}

// --- viewability (RN ViewabilityHelper) -------------------------------------------

function isEntirelyVisible(top, bottom, viewport) {
  return top >= 0 && bottom <= viewport && bottom > top;
}

function pixelsVisible(top, bottom, viewport) {
  return Math.max(0, Math.min(bottom, viewport) - Math.max(0, top));
}

export class ViewabilityHelper {
  constructor(config) {
    this._config = config || {};
    this._hasInteracted = false;
    this._timers = new Set();
    this._viewableIndices = [];
    this._viewableItems = new Map();
  }

  dispose() {
    for (const t of this._timers) clearTimeout(t);
    this._timers.clear();
  }

  /** Indices (within renderRange) that pass the configured threshold. */
  computeViewableItems(count, scrollOffset, viewport, frameOf, renderRange) {
    const { itemVisiblePercentThreshold, viewAreaCoveragePercentThreshold } = this._config;
    const viewAreaMode = viewAreaCoveragePercentThreshold != null || itemVisiblePercentThreshold == null;
    const threshold = viewAreaMode ? viewAreaCoveragePercentThreshold || 0 : itemVisiblePercentThreshold;
    const out = [];
    if (count === 0) return out;
    let firstVisible = -1;
    const first = renderRange ? Math.max(0, renderRange.first) : 0;
    const last = renderRange ? Math.min(count - 1, renderRange.last) : count - 1;
    for (let i = first; i <= last; i++) {
      const m = frameOf(i);
      if (!m) continue;
      const top = Math.floor(m.offset - scrollOffset);
      const bottom = Math.floor(top + m.length);
      if (top < viewport && bottom > 0) {
        firstVisible = i;
        if (isEntirelyVisible(top, bottom, viewport)) out.push(i);
        else {
          const px = pixelsVisible(top, bottom, viewport);
          const pct = 100 * (viewAreaMode ? px / viewport : m.length > 0 ? px / m.length : 0);
          if (pct >= threshold) out.push(i);
        }
      } else if (firstVisible >= 0) break;
    }
    return out;
  }

  onUpdate(count, scrollOffset, viewport, frameOf, createViewToken, onChanged, renderRange) {
    if ((this._config.waitForInteraction && !this._hasInteracted) || count === 0 || !(viewport > 0)) return;
    const indices = this.computeViewableItems(count, scrollOffset, viewport, frameOf, renderRange);
    const prev = this._viewableIndices;
    if (prev.length === indices.length && prev.every((v, i) => v === indices[i])) return;
    this._viewableIndices = indices;
    if (this._config.minimumViewTime) {
      const t = setTimeout(() => {
        this._timers.delete(t);
        this._onUpdateSync(indices.filter((i) => this._viewableIndices.includes(i)), onChanged, createViewToken);
      }, this._config.minimumViewTime);
      this._timers.add(t);
    } else {
      this._onUpdateSync(indices, onChanged, createViewToken);
    }
  }

  resetViewableIndices() {
    this._viewableIndices = [];
  }

  recordInteraction() {
    this._hasInteracted = true;
  }

  _onUpdateSync(indices, onChanged, createViewToken) {
    const prevItems = this._viewableItems;
    const nextItems = new Map();
    for (const i of indices) {
      const t = createViewToken(i, true);
      nextItems.set(t.key, t);
    }
    const changed = [];
    for (const [key, t] of nextItems) if (!prevItems.has(key)) changed.push(t);
    for (const [key, t] of prevItems) if (!nextItems.has(key)) changed.push(Object.assign({}, t, { isViewable: false }));
    if (changed.length > 0) {
      this._viewableItems = nextItems;
      onChanged({ viewableItems: Array.from(nextItems.values()), changed, viewabilityConfig: this._config });
    }
  }
}

// --- cells ---------------------------------------------------------------------

function sameItem(a, b) {
  if (a === b) return true;
  // FlatList numColumns rows are fresh arrays of the same items.
  if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function cellPropsEqual(a, b) {
  for (const k in a) {
    if (k === 'item') {
      if (!sameItem(a.item, b.item)) return false;
    } else if (a[k] !== b[k]) return false;
  }
  for (const k in b) if (!(k in a)) return false;
  return true;
}

/**
 * One mounted cell (RN CellRenderer): a View holding the item and the
 * separator after it, reporting its layout to the list.
 */
const CellRenderer = memo(function CellRenderer(props) {
  const { item, index, cellKey, renderItem, ListItemComponent, Separator, cellStyle, onLayout, CellRendererComponent, list } = props;
  const [sepProps, setSepProps] = useState(null);
  const separators = useMemo(
    () => ({
      highlight: () => setSepProps((p) => Object.assign({}, p, { highlighted: true })),
      unhighlight: () => setSepProps((p) => Object.assign({}, p, { highlighted: false })),
      updateProps: (_select, next) => setSepProps((p) => Object.assign({}, p, next)),
    }),
    [],
  );
  const setRef = useCallback(
    (inst) => {
      if (inst) {
        inst.__vlList = list;
        inst.__vlCellKey = cellKey;
        list._cellRefs.set(cellKey, inst);
      } else if (list._cellRefs.get(cellKey)) list._cellRefs.delete(cellKey);
    },
    [list, cellKey],
  );
  let element = null;
  if (ListItemComponent) element = h(ListItemComponent, { item, index, separators });
  else if (renderItem) element = renderItem({ item, index, separators });
  const sep = Separator ? renderComponent(Separator, Object.assign({ highlighted: false, leadingItem: item }, sepProps)) : null;
  if (CellRendererComponent) {
    return h(CellRendererComponent, { cellKey, index, item, style: cellStyle, onLayout, onFocusCapture: undefined }, element, sep);
  }
  return h('View', { style: cellStyle, onLayout, ref: setRef, collapsable: false }, element, sep);
}, cellPropsEqual);

// --- VirtualizedList -------------------------------------------------------------

const defaultGetItem = (data, i) => (data == null ? undefined : data[i]);
const defaultGetItemCount = (data) => (data == null ? 0 : typeof data.length === 'number' ? data.length : 0);
const EMPTY_RANGE = { first: 0, last: -1 };

export class VirtualizedList extends Component {
  constructor(props) {
    super(props);
    this._scrollRef = null;
    this._cellRefs = new Map();
    this._frames = new Map(); // cellKey -> {offset, length, index}
    this._measuredSum = 0;
    this._measuredCount = 0;
    this._highestMeasured = -1;
    this._headerLength = 0;
    this._footerLength = 0;
    this._metrics = { offset: 0, visibleLength: 0, contentLength: 0, velocity: 0, timestamp: 0, dt: 10 };
    this._sentEndForContentLength = 0;
    this._sentStartForContentLength = 0;
    this._hiPriInProgress = false;
    this._batchTimer = null;
    this._pendingScroll = null;
    this._lastFocusedKey = null;
    this._lastFocusedIndex = -1;
    this._layoutHandlers = new Map();
    this._scrollAnchor = null;
    this._indexOfKey = new Map(); // cells of the last render
    this._mountedKeys = new Map(); // cells of the last commit: key -> index
    this._anchors = null;
    this._unmounted = false;
    this._viewabilityTuples = [];
    this._initViewability(props);
    const count = this._count(props);
    const isi = num(props.initialScrollIndex, 0) > 0 ? Math.min(count - 1, props.initialScrollIndex | 0) : 0;
    const init = Math.max(0, num(props.initialNumToRender, 10) | 0);
    this.state = { cellsAroundViewport: count === 0 ? EMPTY_RANGE : { first: isi, last: Math.min(count, isi + init) - 1 } };
    this._captureScrollRef = (r) => {
      this._scrollRef = r;
    };
  }

  // --- props helpers

  _count(props = this.props) {
    return (props.getItemCount || defaultGetItemCount)(props.data);
  }
  _getItem(index, props = this.props) {
    return (props.getItem || defaultGetItem)(props.data, index);
  }
  _keyAt(index, props = this.props) {
    const item = this._getItem(index, props);
    return (props.keyExtractor || defaultKeyExtractor)(item, index);
  }
  _horizontal() {
    return !!this.props.horizontal;
  }
  _maxPerBatch() {
    return Math.max(1, num(this.props.maxToRenderPerBatch, 10));
  }
  _windowSize() {
    return Math.max(1, num(this.props.windowSize, 21));
  }

  _initViewability(props) {
    for (const t of this._viewabilityTuples) t.helper.dispose();
    this._viewabilityTuples = [];
    if (props.viewabilityConfigCallbackPairs) {
      for (const pair of props.viewabilityConfigCallbackPairs) {
        this._viewabilityTuples.push({ helper: new ViewabilityHelper(pair.viewabilityConfig), pair });
      }
    } else if (props.onViewableItemsChanged) {
      this._viewabilityTuples.push({ helper: new ViewabilityHelper(props.viewabilityConfig), pair: null });
    }
  }

  // --- frames

  get _averageCellLength() {
    return this._measuredCount ? this._measuredSum / this._measuredCount : 0;
  }

  /**
   * Exact frame: getItemLayout, or the measured layout of a cell that is
   * mounted (an unmounted cell's last offset may be stale). Null if unknown.
   */
  _frame(index) {
    const p = this.props;
    if (p.getItemLayout) {
      const f = p.getItemLayout(p.data, index);
      return f ? { offset: f.offset, length: f.length } : null;
    }
    const key = this._keyAt(index);
    if (this._mountedKeys.get(key) !== index) return null;
    const f = this._frames.get(key);
    return f && f.index === index ? f : null;
  }

  /** Mounted, measured cells sorted by index: the anchors for estimates. */
  _getAnchors() {
    if (this._anchors) return this._anchors;
    const out = [];
    for (const [key, index] of this._mountedKeys) {
      const f = this._frames.get(key);
      if (f && f.index === index) out.push(f);
    }
    out.sort((a, b) => a.index - b.index);
    this._anchors = out;
    return out;
  }

  /**
   * Frame of any cell. Unmeasured cells between two mounted cells share the
   * gap between them evenly (so a spacer keeps exactly its current size and
   * mounted cells never move when estimates change); outside the anchors
   * the average measured length is used.
   */
  _frameApprox = (index) => {
    const f = this._frame(index);
    if (f) return f;
    const avg = this._averageCellLength;
    const anchors = this.props.getItemLayout ? [] : this._getAnchors();
    if (!anchors.length) return { offset: this._headerLength + avg * index, length: avg };
    let lo = 0;
    let hi = anchors.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >>> 1;
      if (anchors[mid].index < index) lo = mid + 1;
      else hi = mid - 1;
    }
    const below = anchors[lo - 1];
    const above = anchors[lo];
    if (below && above) {
      const start = below.offset + below.length;
      const n = above.index - below.index - 1;
      const len = n > 0 ? Math.max(0, above.offset - start) / n : avg;
      return { offset: start + (index - below.index - 1) * len, length: len };
    }
    if (below) return { offset: below.offset + below.length + (index - below.index - 1) * avg, length: avg };
    return { offset: above.offset - (above.index - index) * avg, length: avg };
  };

  _onCellLayout(e, cellKey) {
    const index = this._indexOfKey.get(cellKey);
    if (index == null) return;
    const l = (e.nativeEvent && e.nativeEvent.layout) || {};
    const horizontal = this._horizontal();
    const offset = horizontal ? +l.x || 0 : +l.y || 0;
    const length = horizontal ? +l.width || 0 : +l.height || 0;
    const prev = this._frames.get(cellKey);
    if (prev && prev.offset === offset && prev.length === length && prev.index === index) return;
    if (prev) this._measuredSum -= prev.length;
    else this._measuredCount++;
    this._measuredSum += length;
    this._frames.set(cellKey, { offset, length, index });
    if (index > this._highestMeasured) this._highestMeasured = index;
    this._anchors = null;
    // Scroll anchoring (like Chrome's default overflow-anchor, which the
    // react-native-web reference gets for free): cells mounting or resizing
    // above the viewport must not move what is on screen.
    const sa = this._scrollAnchor;
    if (sa && sa.key === cellKey && sa.offset !== offset) {
      const delta = offset - sa.offset;
      sa.offset = offset;
      if (this._metrics.offset > 0 && !this.props.getItemLayout) this._scrollToRaw(this._metrics.offset + delta, false, true);
    }
    this._retryPendingScroll(index);
    this._scheduleCellsToRenderUpdate();
  }

  _layoutHandler(cellKey) {
    let f = this._layoutHandlers.get(cellKey);
    if (!f) {
      f = (e) => this._onCellLayout(e, cellKey);
      this._layoutHandlers.set(cellKey, f);
    }
    return f;
  }

  // --- scroll view callbacks

  _onLayout = (e) => {
    const l = (e.nativeEvent && e.nativeEvent.layout) || {};
    this._metrics.visibleLength = this._horizontal() ? +l.width || 0 : +l.height || 0;
    this._scheduleCellsToRenderUpdate();
    this._maybeCallOnEdgesReached();
    if (this.props.onLayout) this.props.onLayout(e);
  };

  _onContentSizeChange = (width, height) => {
    this._metrics.contentLength = this._horizontal() ? width : height;
    this._scheduleCellsToRenderUpdate();
    this._maybeCallOnEdgesReached();
    if (this._pendingScroll && this._pendingScroll.end) this._retryPendingScroll(-1);
    if (this.props.onContentSizeChange) this.props.onContentSizeChange(width, height);
  };

  _onScroll = (e) => {
    const ne = (e && e.nativeEvent) || {};
    const horizontal = this._horizontal();
    const o = ne.contentOffset || {};
    const offset = horizontal ? +o.x || 0 : +o.y || 0;
    const m = this._metrics;
    if (ne.layoutMeasurement) m.visibleLength = horizontal ? +ne.layoutMeasurement.width || 0 : +ne.layoutMeasurement.height || 0;
    if (ne.contentSize) m.contentLength = horizontal ? +ne.contentSize.width || 0 : +ne.contentSize.height || 0;
    const ts = e && e.timeStamp != null ? e.timeStamp : Date.now();
    let dt = ts - m.timestamp;
    if (!(dt > 0) || dt > 500) dt = 1e9; // first event or a pause: no velocity
    m.velocity = dt < 1e9 ? (offset - m.offset) / dt : 0;
    m.dt = dt;
    m.offset = offset;
    m.timestamp = ts;
    for (const t of this._viewabilityTuples) t.helper.recordInteraction();
    if (!this._anchoring) this._pickScrollAnchor();
    this._updateViewableItems();
    this._maybeCallOnEdgesReached();
    this._scheduleCellsToRenderUpdate();
    if (this.props.onScroll) this.props.onScroll(e);
  };

  _onHeaderLayout = (e) => {
    const l = e.nativeEvent.layout;
    this._headerLength = this._horizontal() ? l.width : l.height;
  };

  _onFooterLayout = (e) => {
    const l = e.nativeEvent.layout;
    this._footerLength = this._horizontal() ? l.width : l.height;
  };

  // Focus anywhere inside a cell keeps that cell (and its neighbours)
  // mounted, like RN's onFocusCapture on CellRenderer. jsui focus events do
  // not bubble, so a global listener walks up from the focused node.
  _onAnyFocus = (e) => {
    let inst = instances.get(e.target);
    while (inst) {
      if (inst.__vlList === this && inst.__vlCellKey != null) {
        const key = inst.__vlCellKey;
        const idx = this._indexOfKey.get(key);
        if (idx != null && key !== this._lastFocusedKey) {
          this._lastFocusedKey = key;
          this._lastFocusedIndex = idx;
          this._scheduleCellsToRenderUpdate();
        }
        return;
      }
      inst = inst.parent;
    }
  };

  // --- updates

  _maybeCallOnEdgesReached() {
    const { onEndReached, onStartReached } = this.props;
    const { contentLength, visibleLength, offset } = this._metrics;
    if (!(visibleLength > 0)) return;
    const count = this._count();
    const range = this.state.cellsAroundViewport;
    const distanceFromStart = offset;
    let distanceFromEnd = contentLength - visibleLength - offset;
    if (distanceFromEnd < 0.001) distanceFromEnd = 0;
    const startThreshold = num(this.props.onStartReachedThreshold, 2) * visibleLength;
    const endThreshold = num(this.props.onEndReachedThreshold, 2) * visibleLength;
    const withinStart = distanceFromStart <= startThreshold;
    const withinEnd = distanceFromEnd <= endThreshold;
    if (onEndReached && count > 0 && range.last === count - 1 && withinEnd && contentLength !== this._sentEndForContentLength) {
      this._sentEndForContentLength = contentLength;
      onEndReached({ distanceFromEnd });
    } else if (onStartReached && count > 0 && range.first === 0 && withinStart && contentLength !== this._sentStartForContentLength) {
      this._sentStartForContentLength = contentLength;
      onStartReached({ distanceFromStart });
    }
    if (!withinStart) this._sentStartForContentLength = 0;
    if (!withinEnd) this._sentEndForContentLength = 0;
  }

  _updateViewableItems() {
    if (!this._viewabilityTuples.length) return;
    const count = this._count();
    const { offset, visibleLength } = this._metrics;
    const frameOf = (i) => this._frame(i);
    const token = (i, isViewable) => {
      const item = this._getItem(i);
      return { item, key: this._keyAt(i), index: i, isViewable };
    };
    for (const t of this._viewabilityTuples) {
      const cb = t.pair ? t.pair.onViewableItemsChanged : this.props.onViewableItemsChanged;
      if (!cb) continue;
      t.helper.onUpdate(count, offset, visibleLength, frameOf, token, cb, this.state.cellsAroundViewport);
    }
  }

  _scheduleCellsToRenderUpdate() {
    if (this._unmounted) return;
    const { first, last } = this.state.cellsAroundViewport;
    const { offset, visibleLength, velocity } = this._metrics;
    const count = this._count();
    let hiPri = false;
    const startThreshold = num(this.props.onStartReachedThreshold, 2) * visibleLength / 2;
    const endThreshold = num(this.props.onEndReachedThreshold, 2) * visibleLength / 2;
    // Mount immediately when the viewport is (about to be) blank.
    if (first > 0) {
      const distTop = offset - this._frameApprox(first).offset;
      hiPri = distTop < 0 || (velocity < -2 && distTop < startThreshold);
    }
    if (!hiPri && last >= 0 && last < count - 1) {
      const f = this._frameApprox(last);
      const distBottom = f.offset + f.length - (offset + visibleLength);
      hiPri = distBottom < 0 || (velocity > 2 && distBottom < endThreshold);
    }
    if (hiPri && (this._averageCellLength || this.props.getItemLayout) && !this._hiPriInProgress) {
      this._hiPriInProgress = true;
      if (this._batchTimer) clearTimeout(this._batchTimer);
      this._batchTimer = null;
      this._updateCellsToRender();
      return;
    }
    if (this._batchTimer) return;
    this._batchTimer = setTimeout(() => {
      this._batchTimer = null;
      if (!this._unmounted) this._updateCellsToRender();
    }, Math.max(0, num(this.props.updateCellsBatchingPeriod, 50)));
  }

  _adjustCellsAroundViewport(props, range) {
    const count = this._count(props);
    const { contentLength, visibleLength, offset } = this._metrics;
    if (!(visibleLength > 0) || !(contentLength > 0)) return constrainToCount(range, count);
    if (props.initialScrollIndex > 0 && !offset && this._pendingScroll) return constrainToCount(range, count);
    return computeWindowedRenderLimits(count, this._maxPerBatch(), this._windowSize(), constrainToCount(range, count), this._frameApprox, this._metrics);
  }

  _updateCellsToRender() {
    this._updateViewableItems();
    this.setState((state, props) => {
      const next = this._adjustCellsAroundViewport(props, state.cellsAroundViewport);
      const cur = state.cellsAroundViewport;
      if (next.first === cur.first && next.last === cur.last) return null;
      return { cellsAroundViewport: next };
    });
  }

  // --- lifecycle

  _committed() {
    this._mountedKeys = this._indexOfKey;
    this._anchors = null;
    this._pickScrollAnchor();
  }

  /** The first mounted, measured cell reaching into the viewport. */
  _pickScrollAnchor() {
    this._scrollAnchor = null;
    if (this.props.getItemLayout || !(this._metrics.offset > 0)) return;
    const anchors = this._getAnchors();
    const top = this._metrics.offset;
    let lo = 0;
    let hi = anchors.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >>> 1;
      if (anchors[mid].offset + anchors[mid].length <= top) lo = mid + 1;
      else hi = mid - 1;
    }
    const a = anchors[lo];
    if (!a || a.offset > top + this._metrics.visibleLength) return;
    for (const [key, f] of this._frames) {
      if (f === a) {
        this._scrollAnchor = { key, offset: a.offset };
        return;
      }
    }
  }

  componentDidMount() {
    this._committed();
    this._unsubFocus = addGlobalListener('focus', this._onAnyFocus);
    const isi = this.props.initialScrollIndex;
    if (isi > 0 && isi < this._count()) this.scrollToIndex({ index: isi, animated: false });
  }

  componentWillUnmount() {
    this._unmounted = true;
    if (this._batchTimer) clearTimeout(this._batchTimer);
    this._batchTimer = null;
    if (this._unsubFocus) this._unsubFocus();
    for (const t of this._viewabilityTuples) t.helper.dispose();
  }

  componentDidUpdate(prevProps) {
    this._committed();
    const { data, extraData } = this.props;
    if (data !== prevProps.data || extraData !== prevProps.extraData) {
      for (const t of this._viewabilityTuples) t.helper.resetViewableIndices();
    }
    if (prevProps.viewabilityConfigCallbackPairs !== this.props.viewabilityConfigCallbackPairs ||
        !!prevProps.onViewableItemsChanged !== !!this.props.onViewableItemsChanged) {
      this._initViewability(this.props);
    }
    const hiPri = this._hiPriInProgress;
    this._scheduleCellsToRenderUpdate();
    if (hiPri) this._hiPriInProgress = false;
    this._updateViewableItems();
    this._maybeCallOnEdgesReached();
  }

  // --- imperative API (RN VirtualizedList / FlatList methods)

  _scrollToRaw(offset, animated, anchoring) {
    const s = this._scrollRef;
    if (!s || typeof s.scrollTo !== 'function') return;
    if (!anchoring) this._scrollAnchor = null;
    const horizontal = this._horizontal();
    const m = this._metrics;
    // Optimistic: the native scroll event follows synchronously when the
    // offset changes; until then compute the window for the target.
    const max = m.contentLength > m.visibleLength ? m.contentLength - m.visibleLength : offset;
    m.offset = Math.max(0, Math.min(offset, max));
    m.velocity = 0;
    this._anchoring = !!anchoring;
    try {
      s.scrollTo(horizontal ? { x: offset, y: 0, animated } : { x: 0, y: offset, animated });
    } finally {
      this._anchoring = false;
    }
    this._scheduleCellsToRenderUpdate();
  }

  scrollToOffset({ offset = 0, animated = true } = {}) {
    this._pendingScroll = null;
    this._scrollToRaw(offset, animated);
  }

  scrollToEnd({ animated = true } = {}) {
    const count = this._count();
    if (count === 0) {
      if (this._scrollRef && this._scrollRef.scrollToEnd) this._scrollRef.scrollToEnd({ animated });
      return;
    }
    const f = this._frameApprox(count - 1);
    const offset = Math.max(0, f.offset + f.length + this._footerLength - this._metrics.visibleLength);
    this._pendingScroll = this.props.getItemLayout ? null : { end: true, animated, tries: 4, until: Date.now() + 2000 };
    this._scrollToRaw(offset, animated);
  }

  scrollToIndex(params) {
    const { index, animated = true, viewOffset = 0, viewPosition = 0 } = params || {};
    const count = this._count();
    if (!(index >= 0 && index < count)) {
      warnOnce('scrollToIndex-range', `[jsui] scrollToIndex out of range: requested index ${index} but maximum is ${count - 1}`);
      return;
    }
    const p = this.props;
    if (!p.getItemLayout && index > this._highestMeasured && p.onScrollToIndexFailed) {
      p.onScrollToIndexFailed({ averageItemLength: this._averageCellLength, highestMeasuredFrameIndex: this._highestMeasured, index });
      return;
    }
    const exact = this._frame(index);
    const f = exact || this._frameApprox(index);
    const offset = Math.max(0, f.offset - viewPosition * (this._metrics.visibleLength - f.length)) - viewOffset;
    // Without getItemLayout an unmeasured target is estimated; re-scroll
    // precisely once the cell mounts and reports its layout.
    this._pendingScroll = p.getItemLayout ? null : { index, viewOffset, viewPosition, animated, tries: 4, until: Date.now() + 2000, last: exact ? offset : NaN };
    this._scrollToRaw(offset, animated);
  }

  _retryPendingScroll(index) {
    const ps = this._pendingScroll;
    if (!ps) return;
    if (Date.now() > ps.until || ps.tries <= 0) {
      this._pendingScroll = null;
      return;
    }
    if (ps.end) {
      if (index !== -1) return;
      const count = this._count();
      const f = this._frame(count - 1);
      if (!f) return;
      ps.tries--;
      const s = this._scrollRef;
      if (s && s.scrollToEnd) s.scrollToEnd({ animated: false });
      return;
    }
    if (index !== ps.index) return;
    const f = this._frame(ps.index);
    if (!f) return;
    const offset = Math.max(0, f.offset - ps.viewPosition * (this._metrics.visibleLength - f.length)) - ps.viewOffset;
    if (offset === ps.last) return;
    ps.last = offset;
    ps.tries--;
    // Defer: we are inside a layout callback.
    Promise.resolve().then(() => {
      if (this._pendingScroll === ps) this._scrollToRaw(offset, false);
    });
  }

  scrollToItem(params) {
    const { item } = params || {};
    const count = this._count();
    for (let i = 0; i < count; i++) {
      if (this._getItem(i) === item) {
        this.scrollToIndex(Object.assign({}, params, { index: i }));
        return;
      }
    }
  }

  recordInteraction() {
    for (const t of this._viewabilityTuples) t.helper.recordInteraction();
    if (!this._anchoring) this._pickScrollAnchor();
    this._updateViewableItems();
  }

  flashScrollIndicators() {}

  getScrollResponder() {
    return this._scrollRef;
  }
  getScrollableNode() {
    return this._scrollRef && this._scrollRef.id;
  }
  getNativeScrollRef() {
    return this._scrollRef;
  }
  getScrollRef() {
    return this._scrollRef;
  }
  setNativeProps(props) {
    if (this._scrollRef && this._scrollRef.setNativeProps) this._scrollRef.setNativeProps(props);
  }
  hasMore() {
    return false;
  }
  /** RN private helper used by SectionList.scrollToLocation. */
  __getFrameMetricsApprox(index) {
    return this._frameApprox(index);
  }

  // --- render

  _renderMask(count) {
    const p = this.props;
    if (count === 0) return [];
    if (p.disableVirtualization) return [[0, count - 1]];
    const ranges = [];
    const init = Math.max(0, num(p.initialNumToRender, 10) | 0);
    if (!(p.initialScrollIndex > 0) && init > 0) ranges.push([0, Math.min(count, init) - 1]);
    const r = constrainToCount(this.state.cellsAroundViewport, count);
    if (r.last >= r.first) ranges.push([r.first, r.last]);
    // Keep the focused cell mounted (and its neighbours, for spatial nav).
    if (this._lastFocusedKey != null) {
      let fi = this._lastFocusedIndex;
      if (!(fi >= 0 && fi < count && this._keyAt(fi) === this._lastFocusedKey)) fi = -1;
      if (fi >= 0) ranges.push([Math.max(0, fi - 1), Math.min(count - 1, fi + 1)]);
      else this._lastFocusedKey = null;
    }
    // The closest sticky header before the window.
    const sticky = p.stickyHeaderIndices;
    if (sticky && sticky.length && r.last >= r.first) {
      const off = p.ListHeaderComponent ? 1 : 0;
      let best = -1;
      for (const s of sticky) {
        const i = s - off;
        if (i >= 0 && i < r.first && i > best) best = i;
      }
      if (best >= 0) ranges.push([best, best]);
    }
    ranges.sort((a, b) => a[0] - b[0]);
    const merged = [];
    for (const [a, b] of ranges) {
      const lastR = merged[merged.length - 1];
      if (lastR && a <= lastR[1] + 1) lastR[1] = Math.max(lastR[1], b);
      else merged.push([a, b]);
    }
    return merged;
  }

  _spacer(key, a, b, horizontal) {
    const fa = this._frameApprox(a);
    const fb = this._frameApprox(b);
    const size = Math.max(0, fb.offset + fb.length - fa.offset);
    return h('View', { key, style: horizontal ? { width: size } : { height: size } });
  }

  render() {
    const p = this.props;
    const {
      ListHeaderComponent, ListFooterComponent, ListEmptyComponent, ListHeaderComponentStyle, ListFooterComponentStyle,
      ItemSeparatorComponent, ListItemComponent, CellRendererComponent, renderItem, extraData, inverted,
    } = p;
    const horizontal = this._horizontal();
    const count = this._count();
    const inversionStyle = inverted ? (horizontal ? styles.horizontallyInverted : styles.verticallyInverted) : null;
    const cellStyle = inversionStyle
      ? [horizontal ? styles.rowReverse : styles.columnReverse, inversionStyle]
      : horizontal ? styles.row : undefined;
    const stickyFromProps = p.stickyHeaderIndices && p.stickyHeaderIndices.length ? new Set(p.stickyHeaderIndices) : null;
    const stickyOffset = ListHeaderComponent ? 1 : 0;
    const stickyHeaderIndices = [];
    const cells = [];

    if (ListHeaderComponent) {
      if (stickyFromProps && stickyFromProps.has(0)) stickyHeaderIndices.push(0);
      cells.push(h('View', { key: '$header', onLayout: this._onHeaderLayout, style: [inversionStyle, ListHeaderComponentStyle] }, renderComponent(ListHeaderComponent)));
    }

    const indexOfKey = new Map();
    const mask = this._renderMask(count);
    const getAware = !p.getItemLayout;
    let prevEnd = -1;
    for (const [a, b] of mask) {
      if (a > prevEnd + 1) cells.push(this._spacer('$spacer-' + (prevEnd + 1), prevEnd + 1, a - 1, horizontal));
      for (let i = a; i <= b; i++) {
        const item = this._getItem(i);
        const key = (p.keyExtractor || defaultKeyExtractor)(item, i);
        indexOfKey.set(key, i);
        if (stickyFromProps && stickyFromProps.has(i + stickyOffset)) stickyHeaderIndices.push(cells.length);
        cells.push(
          h(CellRenderer, {
            key,
            cellKey: key,
            item,
            index: i,
            list: this,
            renderItem,
            ListItemComponent,
            extraData,
            Separator: i < count - 1 ? ItemSeparatorComponent : undefined,
            cellStyle,
            CellRendererComponent,
            onLayout: getAware ? this._layoutHandler(key) : undefined,
          }),
        );
      }
      prevEnd = b;
    }
    if (count > 0 && prevEnd < count - 1) cells.push(this._spacer('$spacer-' + (prevEnd + 1), prevEnd + 1, count - 1, horizontal));
    this._indexOfKey = indexOfKey;
    if (this._layoutHandlers.size > indexOfKey.size * 4 + 64) {
      for (const k of this._layoutHandlers.keys()) if (!indexOfKey.has(k)) this._layoutHandlers.delete(k);
    }

    if (count === 0 && ListEmptyComponent) {
      const el = isValidElement(ListEmptyComponent) ? ListEmptyComponent : h(ListEmptyComponent);
      cells.push(cloneElement(el, { key: '$empty', style: inversionStyle ? [inversionStyle, el.props.style] : el.props.style }));
    }
    if (ListFooterComponent) {
      cells.push(h('View', { key: '$footer', onLayout: this._onFooterLayout, style: [inversionStyle, ListFooterComponentStyle] }, renderComponent(ListFooterComponent)));
    }

    const scrollProps = {};
    for (const k in p) if (!LIST_OMIT[k]) scrollProps[k] = p[k];
    scrollProps.horizontal = horizontal;
    scrollProps.onLayout = this._onLayout;
    scrollProps.onScroll = this._onScroll;
    scrollProps.onContentSizeChange = this._onContentSizeChange;
    scrollProps.style = inversionStyle ? [inversionStyle, p.style] : p.style;
    if (stickyHeaderIndices.length) scrollProps.stickyHeaderIndices = stickyHeaderIndices;
    if (p.renderScrollComponent) {
      const el = p.renderScrollComponent(scrollProps);
      return cloneElement(el, { ref: this._captureScrollRef }, cells);
    }
    scrollProps.ref = this._captureScrollRef;
    return h(ScrollView, scrollProps, cells);
  }
}
VirtualizedList.displayName = 'VirtualizedList';

function constrainToCount(range, count) {
  if (count === 0) return EMPTY_RANGE;
  const last = Math.min(count - 1, range.last);
  const first = Math.max(0, Math.min(range.first, last));
  if (first === range.first && last === range.last) return range;
  return { first, last };
}

// --- FlatList ----------------------------------------------------------------------

// Methods forwarded from FlatList / SectionList refs to the VirtualizedList.
const FORWARDED = ['scrollToEnd', 'scrollToIndex', 'scrollToItem', 'scrollToOffset', 'recordInteraction', 'flashScrollIndicators',
  'getScrollResponder', 'getScrollableNode', 'getNativeScrollRef', 'getScrollRef', 'setNativeProps'];

export class FlatList extends Component {
  constructor(props) {
    super(props);
    this._listRef = null;
    this._captureRef = (r) => {
      this._listRef = r;
    };
    this._rendererCache = null;
    this._viewableCache = null;
  }

  _cols() {
    const n = num(this.props.numColumns, 1) | 0;
    return n > 1 ? n : 1;
  }

  _getItem = (data, index) => {
    const cols = this._cols();
    if (cols > 1) {
      const out = [];
      for (let k = 0; k < cols; k++) {
        const i = index * cols + k;
        if (i < data.length) out.push(data[i]);
      }
      return out;
    }
    return data[index];
  };

  _getItemCount = (data) => {
    if (data == null || typeof data.length !== 'number') return 0;
    const cols = this._cols();
    return cols > 1 ? Math.ceil(data.length / cols) : data.length;
  };

  _keyExtractor = (items, index) => {
    const cols = this._cols();
    const ke = this.props.keyExtractor || defaultKeyExtractor;
    if (cols > 1) return items.map((it, k) => ke(it, index * cols + k)).join(':');
    return ke(items, index);
  };

  _renderer() {
    const { renderItem, ListItemComponent, columnWrapperStyle } = this.props;
    const cols = this._cols();
    const c = this._rendererCache;
    if (c && c.renderItem === renderItem && c.ListItemComponent === ListItemComponent && c.cols === cols && c.columnWrapperStyle === columnWrapperStyle) return c.fn;
    const one = (info) => (ListItemComponent ? h(ListItemComponent, info) : renderItem ? renderItem(info) : null);
    const fn =
      cols > 1
        ? (info) =>
            h(
              'View',
              { style: [styles.row, columnWrapperStyle] },
              info.item.map((it, k) => {
                const el = one({ item: it, index: info.index * cols + k, separators: info.separators });
                return el != null ? h(Fragment, { key: k }, el) : null;
              }),
            )
        : one;
    this._rendererCache = { renderItem, ListItemComponent, cols, columnWrapperStyle, fn };
    return fn;
  }

  _expandViewable(cb) {
    const cols = this._cols();
    if (cols <= 1 || !cb) return cb;
    const c = this._viewableCache;
    if (c && c.cb === cb && c.cols === cols) return c.fn;
    const ke = () => this.props.keyExtractor || defaultKeyExtractor;
    const expand = (arr) => {
      const out = [];
      for (const v of arr) {
        v.item.forEach((item, k) => {
          const index = v.index * cols + k;
          out.push(Object.assign({}, v, { item, key: ke()(item, index), index }));
        });
      }
      return out;
    };
    const fn = (info) => cb(Object.assign({}, info, { viewableItems: expand(info.viewableItems), changed: expand(info.changed) }));
    this._viewableCache = { cb, cols, fn };
    return fn;
  }

  render() {
    const { numColumns, columnWrapperStyle, renderItem, ListItemComponent, keyExtractor, onViewableItemsChanged, viewabilityConfigCallbackPairs, ...rest } = this.props;
    rest.getItem = this._getItem;
    rest.getItemCount = this._getItemCount;
    rest.keyExtractor = this._keyExtractor;
    rest.renderItem = this._renderer();
    rest.ref = this._captureRef;
    if (onViewableItemsChanged) rest.onViewableItemsChanged = this._expandViewable(onViewableItemsChanged);
    if (viewabilityConfigCallbackPairs) {
      rest.viewabilityConfigCallbackPairs = viewabilityConfigCallbackPairs.map((pair) =>
        Object.assign({}, pair, { onViewableItemsChanged: this._expandViewable(pair.onViewableItemsChanged) }),
      );
    }
    return h(VirtualizedList, rest);
  }
}
FlatList.displayName = 'FlatList';
for (const m of FORWARDED) {
  FlatList.prototype[m] = function (...args) {
    const l = this._listRef;
    return l ? l[m](...args) : undefined;
  };
}

// --- SectionList -------------------------------------------------------------------

/**
 * SectionList (RN VirtualizedSectionList): sections are flattened into one
 * VirtualizedList of [header, ...items, footer] per section.
 */
export class SectionList extends Component {
  constructor(props) {
    super(props);
    this._listRef = null;
    this._captureRef = (r) => {
      this._listRef = r;
    };
    this._starts = null;
    this._startsFor = null;
    this._renderers = new WeakMap();
  }

  _index(sections) {
    if (this._startsFor === sections && this._starts) return this._starts;
    const starts = [];
    let n = 0;
    for (const s of sections || []) {
      starts.push(n);
      n += ((s && s.data && s.data.length) || 0) + 2;
    }
    this._starts = { starts, total: n };
    this._startsFor = sections;
    return this._starts;
  }

  /** RN _subExtractor: which section / item a flat index is. */
  _sub(index) {
    const sections = this.props.sections || [];
    const { starts } = this._index(sections);
    let lo = 0;
    let hi = starts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >>> 1;
      if (starts[mid] <= index) lo = mid;
      else hi = mid - 1;
    }
    const si = lo;
    const section = sections[si];
    if (!section) return null;
    const data = section.data || [];
    const sKey = section.key != null ? String(section.key) : String(si);
    const ii = index - starts[si] - 1;
    if (ii === -1) return { section, sectionIndex: si, key: sKey + ':header', index: null, header: true, trailingSection: sections[si + 1] };
    if (ii === data.length) return { section, sectionIndex: si, key: sKey + ':footer', index: null, header: false, trailingSection: sections[si + 1] };
    const ke = section.keyExtractor || this.props.keyExtractor || defaultKeyExtractor;
    return {
      section,
      sectionIndex: si,
      key: sKey + ':' + ke(data[ii], ii),
      index: ii,
      leadingItem: data[ii - 1],
      leadingSection: sections[si - 1],
      trailingItem: data[ii + 1],
      trailingSection: sections[si + 1],
    };
  }

  _getItem = (sections, index) => {
    const info = this._sub(index);
    if (!info) return null;
    return info.index == null ? info.section : info.section.data[info.index];
  };

  _getItemCount = (sections) => this._index(sections).total;

  _keyExtractor = (_item, index) => {
    const info = this._sub(index);
    return (info && info.key) || String(index);
  };

  _renderItem = ({ index, separators }) => {
    const info = this._sub(index);
    if (!info) return null;
    const p = this.props;
    if (info.index == null) {
      if (info.header) return p.renderSectionHeader ? p.renderSectionHeader({ section: info.section }) : null;
      return p.renderSectionFooter ? p.renderSectionFooter({ section: info.section }) : null;
    }
    const render = info.section.renderItem || p.renderItem;
    const item = info.section.data[info.index];
    const element = render ? render({ item, index: info.index, section: info.section, separators }) : null;
    const ItemSep = info.section.ItemSeparatorComponent || p.ItemSeparatorComponent;
    const SectionSep = p.SectionSeparatorComponent;
    const isLastInSection = info.index === info.section.data.length - 1;
    const isLastInList = index === this._getItemCount(p.sections) - 1;
    let Sep = null;
    if (SectionSep && isLastInSection) Sep = SectionSep;
    else if (ItemSep && !isLastInSection && !isLastInList) Sep = ItemSep;
    const Leading = info.index === 0 ? SectionSep : null;
    if (!Sep && !Leading) return element;
    const sepProps = {
      highlighted: false,
      leadingItem: item,
      leadingSection: info.leadingSection,
      section: info.section,
      trailingItem: info.trailingItem,
      trailingSection: info.trailingSection,
    };
    return h(
      Fragment,
      null,
      Leading ? renderComponent(Leading, Object.assign({}, sepProps, { leadingItem: info.leadingItem, trailingItem: item })) : null,
      element,
      Sep ? renderComponent(Sep, sepProps) : null,
    );
  };

  _convertViewable(v) {
    const info = this._sub(v.index);
    if (!info) return null;
    const ke = info.section.keyExtractor || this.props.keyExtractor || defaultKeyExtractor;
    return Object.assign({}, v, { index: info.index, key: ke(v.item, info.index == null ? 0 : info.index), section: info.section });
  }

  _onViewableItemsChanged = ({ viewableItems, changed }) => {
    const cb = this.props.onViewableItemsChanged;
    if (!cb) return;
    const conv = (arr) => arr.map((v) => this._convertViewable(v)).filter(Boolean);
    cb({ viewableItems: conv(viewableItems), changed: conv(changed) });
  };

  scrollToLocation(params) {
    const { sectionIndex = 0, itemIndex = 0 } = params || {};
    const sections = this.props.sections || [];
    let index = itemIndex;
    for (let i = 0; i < sectionIndex && i < sections.length; i++) index += ((sections[i].data && sections[i].data.length) || 0) + 2;
    let viewOffset = params.viewOffset || 0;
    const l = this._listRef;
    if (!l) return;
    if (itemIndex > 0 && this.props.stickySectionHeadersEnabled) viewOffset += l.__getFrameMetricsApprox(index - itemIndex).length;
    l.scrollToIndex(Object.assign({}, params, { viewOffset, index }));
  }

  render() {
    const {
      sections = [], renderItem, renderSectionHeader, renderSectionFooter, ItemSeparatorComponent, SectionSeparatorComponent,
      keyExtractor, stickySectionHeadersEnabled, onViewableItemsChanged, ...rest
    } = this.props;
    // Section list invalidation: a new sections array or extraData re-renders cells.
    this._index(sections);
    rest.data = sections;
    rest.getItem = this._getItem;
    rest.getItemCount = this._getItemCount;
    rest.keyExtractor = this._keyExtractor;
    rest.renderItem = this._renderItem;
    rest.ItemSeparatorComponent = undefined;
    rest.ref = this._captureRef;
    // Cells are memoized on renderItem: refresh it when anything it reads changes.
    rest.extraData = [this.props.extraData, sections, renderItem, renderSectionHeader, renderSectionFooter, ItemSeparatorComponent, SectionSeparatorComponent];
    rest.extraData = this._stableExtra(rest.extraData);
    if (onViewableItemsChanged) rest.onViewableItemsChanged = this._onViewableItemsChanged;
    if (stickySectionHeadersEnabled) {
      const off = this.props.ListHeaderComponent ? 1 : 0;
      const sticky = [];
      const { starts } = this._index(sections);
      for (const s of starts) sticky.push(s + off);
      rest.stickyHeaderIndices = sticky;
    }
    return h(VirtualizedList, rest);
  }

  _stableExtra(arr) {
    const prev = this._extra;
    if (prev && prev.length === arr.length && prev.every((v, i) => v === arr[i])) return prev;
    this._extra = arr;
    return arr;
  }
}
SectionList.displayName = 'SectionList';
for (const m of FORWARDED) {
  SectionList.prototype[m] = function (...args) {
    const l = this._listRef;
    return l ? l[m](...args) : undefined;
  };
}
