// Inspector for tools: answers jsui_call_ui('__jsui.inspect') with the host
// view tree (types, testIDs, text, layout), so headless drivers such as
// `jsui inspect` / `jsui run` (@orktes/jsui-cli) can find elements by testID or
// text and click them.

import { native } from './native.js';
import { instances, measureNode } from './instances.js';
import { rootContainers } from './renderer.js';
import { addGlobalListener } from './events.js';
import { handle } from './hostData.js';

let focusedId = null;
let tracking = false;
function trackFocus() {
  if (tracking) return;
  tracking = true;
  addGlobalListener('focus', (e) => {
    focusedId = e.target;
  });
  addGlobalListener('blur', (e) => {
    if (focusedId === e.target) focusedId = null;
  });
}

function textOf(inst, byParent) {
  if (inst.type === 'RawText') return inst.text;
  const kids = byParent.get(inst);
  if (!kids) return '';
  let s = '';
  for (const k of kids) s += textOf(k, byParent);
  return s;
}

const r1 = (v) => Math.round((+v || 0) * 10) / 10;

function describe(inst, byParent, all) {
  const p = inst.props || {};
  const np = inst.nativeProps || (typeof native.getProps === 'function' && native.getProps(inst.id)) || {};
  const m = measureNode(inst.id);
  const el = { id: inst.id, type: inst.type };
  if (p.testID != null) el.testID = String(p.testID);
  if (p.nativeID != null) el.nativeID = String(p.nativeID);
  const label = p.accessibilityLabel != null ? p.accessibilityLabel : p['aria-label'];
  if (label != null) el.label = String(label);
  const role = p.accessibilityRole || p.role;
  if (role) el.role = String(role);
  if (inst.type === 'Text') el.text = textOf(inst, byParent);
  if (inst.type === 'TextInput') {
    if (p.value != null) el.value = String(p.value);
    if (p.placeholder != null) el.placeholder = String(p.placeholder);
  }
  el.rect = { x: r1(m.pageX), y: r1(m.pageY), width: r1(m.width), height: r1(m.height) };
  const ev = np.__events;
  if (ev && (ev.indexOf('click') >= 0 || ev.indexOf('press') >= 0)) el.pressable = true;
  if (np.disabled) el.disabled = true;
  if (inst.hidden) el.hidden = true;
  if (focusedId === inst.id) el.focused = true;
  all.push(el);
  const kids = byParent.get(inst);
  if (kids) {
    const children = [];
    for (const k of kids) if (k.type !== 'RawText') children.push(describe(k, byParent, all));
    if (children.length) el.children = children;
  }
  return el;
}

/**
 * The mounted host tree of every surface: `{ roots: [{rootTag, viewport,
 * children}], focused }`. Siblings are in creation order (React does not
 * mirror host child order outside the root container).
 */
export function inspect() {
  const byParent = new Map();
  for (const inst of instances.values()) {
    if (!inst.parent) continue;
    let list = byParent.get(inst.parent);
    if (!list) byParent.set(inst.parent, (list = []));
    list.push(inst);
  }
  for (const list of byParent.values()) list.sort((a, b) => a.id - b.id);
  const roots = [];
  for (const c of rootContainers()) {
    const all = [];
    const children = c.children.filter((k) => k.type !== 'RawText').map((k) => describe(k, byParent, all));
    const vp = typeof native.viewport === 'function' ? native.viewport(c.id) : null;
    roots.push({ rootTag: c.id, viewport: vp ? { width: vp.width, height: vp.height, scale: vp.scale } : null, children });
  }
  return { roots, focused: focusedId };
}

export function installInspector() {
  trackFocus();
  handle('__jsui.inspect', () => inspect());
}
