// `jsui run`: executes a list of steps (JSON) against a headless app and
// reports one result per step. See STEP_HELP for the format.

import { elementSummary, textsOf } from './headless.js';

export const STEP_HELP = `jsui run <entry> <script.json|script.mjs>, or the steps as words:
  jsui run <entry> press Down click '#play' type Bob key Enter screenshot out.png
  jsui run --attach <url> expect '#gold' text='99 gold' state gold value=99 press A count=2
(an action word takes the next word as its value; name=value words add options; a
JSON word is a step or an array of steps)

steps (a JSON array, or {"steps": [...]}); targets are selectors ("#testID",
"text=Exact", "Contains", "label=...", "type=TextInput", "... >> 1") or [x, y]:
  {"wait": 500}                         advance the UI clock (ms)
  {"idle": 3000}                        run until nothing loads or animates (max ms)
  {"screenshot": "shots/menu.png"}      PNG of the current frame
  {"click": "#play", "hold": 50}        mouse click (also "tap", "hover")
  {"drag": "#knob", "to": [400, 300], "duration": 300, "touch": false}
  {"wheel": "#list", "dy": 240}         mouse wheel (dy > 0 scrolls down)
  {"key": "ArrowDown", "count": 2, "mods": ["shift"]}
  {"type": "Valkyrie"}                  text into the focused TextInput ("\\n" = Enter)
  {"press": "A", "count": 1}            gamepad button (A B X Y LB RB LT RT Start Back Up Down Left Right)
  {"stick": "lx", "value": 1, "duration": 300}
  {"state": "gold", "value": 1250}      jsui_set_state (no "value": remove)
  {"patch": "inventory", "value": {...}} merge patch
  {"buffer": "enemies", "values": [...], "type": "f32"}
  {"message": "hp", "data": 42}         jsui_post_message
  {"callUi": "ui.flash", "args": {...}} Host.handle handler; result in the output
  {"run": "Settings", "props": {...}}   switch the main surface's component
  {"resize": [1920, 1080, 1]}
  {"reload": true}                      reload the bundle (resets component state; shared state stays)
  {"find": "#play"}                     the element (or null) in the output
  {"expect": "#row-vsync", "count": 1, "text": "On", "focused": true}
                                        fails the run unless it matches; "text" / "textContains"
                                        check the element's text, or one of the Texts inside it
  {"expectMissing": "#spinner"}
  {"expectState": "settings", "value": {...}}  exact (deep-equal) check of a state key
  {"expectState": "settings", "match": {"volume": 50}}  only the keys given
  {"answer": "settings.apply", "value": {...}}  answer the UI's Host.call(name) from now on
  {"answer": "settings.apply", "error": "Disk full"}  ... or reject it
  {"expectMessage": "audio.play"}       the UI posted this Host.postMessage name
  {"tree": true}                        the whole view tree in the output
  {"getState": "settings"}
  {"hitTest": [640, 360]}               {pointer, scroll}: would the UI take input there?
  {"activity": 0}                       (--attach only) what the person, agents and UI did after
                                        that seq number: clicks, keys, messages, calls, logs
Unanswered Host.calls are rejected; answer them with "answer" steps or a top-level
{"calls": {"name": {"value": ...} | {"error": "..."}}, "steps": [...]}. Async UI work (calls,
fetches) settles a frame or two later: follow it with {"wait": 100} or {"idle": 1000}.
The action is the step's first key. Every step may carry "label" (echoed in the output).`;

const ACTIONS = [
  'wait', 'idle', 'screenshot', 'click', 'tap', 'hover', 'drag', 'wheel', 'key', 'type', 'press', 'stick',
  'state', 'patch', 'buffer', 'message', 'callUi', 'run', 'resize', 'find', 'expect', 'expectMissing',
  'expectState', 'expectMessage', 'tree', 'getState', 'hitTest', 'answer', 'activity', 'reload',
];

// The step's first key that names an action ({"buffer": k, "type": "f32"} is a buffer step).
function actionOf(s) {
  return s && typeof s === 'object' ? Object.keys(s).find((k) => ACTIONS.includes(k)) || null : null;
}

// `expected` with only some keys: every key it has must match (recursively).
function isSubset(actual, expected) {
  if (!expected || typeof expected !== 'object' || Array.isArray(expected)) return deepEqual(actual, expected);
  if (!actual || typeof actual !== 'object') return false;
  return Object.keys(expected).every((k) => isSubset(actual[k], expected[k]));
}

/** A Host.call answer from a step / script: {"value": v} resolves, {"error": "msg"} rejects. */
export function answerOf(s) {
  if (s && typeof s === 'object' && 'error' in s) {
    const message = String(s.error);
    return () => {
      throw new Error(message);
    };
  }
  return s && typeof s === 'object' && 'value' in s ? s.value : null;
}

function deepEqual(a, b) {
  if (a === b) return true;
  if (!a || !b || typeof a !== 'object' || typeof b !== 'object' || Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a);
  if (ka.length !== Object.keys(b).length) return false;
  return ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && deepEqual(a[k], b[k]));
}

async function runStep(app, s) {
  const action = actionOf(s);
  if (!action) throw new Error(`unknown step ${JSON.stringify(s)}`);
  const v = s[action];
  switch (action) {
    case 'wait': await app.wait(+v || 0); return { action };
    case 'idle': await app.idle(typeof v === 'number' ? v : 5000); return { action };
    case 'screenshot': await app.screenshot(v); return { action, file: v };
    case 'click': return { action, consumed: await app.click(v, s) };
    case 'tap': return { action, consumed: await app.tap(v, s) };
    case 'hover': await app.hover(v); return { action };
    case 'drag': await app.drag(v, s.to, s); return { action };
    case 'wheel': return { action, consumed: await app.wheel(v, s.dy ?? 120, s.dx ?? 0) };
    case 'key': return { action, consumed: await app.key(v, s) };
    case 'type': await app.type(v, s); return { action };
    case 'press': return { action, consumed: await app.press(v, s) };
    case 'stick': await app.stick(v, s.value ?? 1, s); return { action };
    case 'state': app.setState(v, s.value); await app.frame(); return { action };
    case 'patch': app.patchState(v, s.value); await app.frame(); return { action };
    case 'buffer': app.setBuffer(v, s.values || [], s.type || 'f32'); await app.frame(); return { action };
    case 'message': app.postMessage(v, s.data); await app.frame(); return { action };
    case 'callUi': return { action, result: await app.callUi(v, s.args) };
    case 'run': app.run(v, s.props || {}); await app.frame(); return { action };
    case 'resize': app.resize(+v[0], +v[1], v[2] != null ? +v[2] : undefined); return { action };
    case 'find': return { action, element: await app.find(v) };
    case 'tree': return { action, tree: await app.tree() };
    case 'getState': return { action, value: app.getState(v) ?? null };
    case 'hitTest': return { action, ...(await app.hitTest(v)) };
    case 'reload': await app.reload(); return { action };
    case 'activity':
      if (typeof app.activity !== 'function') throw new Error('"activity" needs the shared view (jsui run --attach)');
      return { action, events: await app.activity(+v || 0) };
    case 'expect': {
      const els = await app.findAll(v);
      const el = elementSummary(els[0]) || null;
      const texts = els[0] ? textsOf(els[0]).map((t) => t.trim()) : [];
      if (s.count != null ? els.length !== s.count : !els.length) {
        throw Object.assign(new Error(`expected ${s.count ?? '≥1'} match(es) for ${JSON.stringify(v)}, found ${els.length}`), { element: el });
      }
      if (s.text != null && !texts.includes(String(s.text).trim())) {
        throw Object.assign(new Error(`expected text ${JSON.stringify(s.text)}, got ${JSON.stringify(texts)}`), { element: el });
      }
      if (s.textContains != null && !texts.some((t) => t.toLowerCase().includes(String(s.textContains).toLowerCase()))) {
        throw Object.assign(new Error(`expected text containing ${JSON.stringify(s.textContains)}, got ${JSON.stringify(texts)}`), { element: el });
      }
      if (s.focused != null && !!(el && el.focused) !== !!s.focused) {
        throw Object.assign(new Error(`expected ${JSON.stringify(v)} ${s.focused ? '' : 'not '}to be focused`), { element: el });
      }
      return { action, element: el, count: els.length };
    }
    case 'expectMissing': {
      const el = await app.find(v);
      if (el) throw Object.assign(new Error(`expected no match for ${JSON.stringify(v)}`), { element: el });
      return { action };
    }
    case 'expectState': {
      const value = app.getState(v);
      const ok = 'match' in s ? isSubset(value, s.match) : deepEqual(value, s.value);
      if (!ok) throw Object.assign(new Error(`state ${v} is ${JSON.stringify(value)}`), { value });
      return { action, value };
    }
    case 'answer':
      app.handle(v, answerOf(s));
      return { action };
    case 'expectMessage': {
      const m = app.messages.filter((x) => x.name === v);
      if (!m.length) throw new Error(`no Host.postMessage("${v}") so far (got: ${[...new Set(app.messages.map((x) => x.name))].join(', ') || 'none'})`);
      return { action, data: m[m.length - 1].data };
    }
  }
  return { action };
}

// Actions without a value (`tree`, `reload`) or with an optional number (`idle 3000`, `activity 12`).
const NO_VALUE = new Set(['tree', 'reload']);
const OPTIONAL_NUMBER = { idle: 5000, activity: 0 };
// Actions whose value is a name or text, never JSON (`type 42` types "42").
const STRING_VALUE = new Set(['type', 'key', 'screenshot', 'state', 'patch', 'buffer', 'message', 'callUi', 'run', 'answer',
  'expectState', 'expectMessage', 'getState']);

// A token's value: JSON when it parses (numbers, true, [x, y], {...}), else the string.
function tokenValue(t) {
  try {
    return JSON.parse(t);
  } catch {
    return t;
  }
}

/**
 * Steps from command-line words, so `jsui run` needs no file:
 *   press Down  click '#play'  type Bob  key Enter  wait 200  screenshot out.png
 *   expect '#gold' text='99 gold'   state gold value=99   press A count=3   tree
 * An action word takes the next word as its value (JSON if it parses, except
 * for names and text); `name=value` words add options to the step before
 * them. A word that is a JSON object or array is a step (or steps), as in
 * script files.
 */
export function parseInlineSteps(words) {
  const steps = [];
  let cur = null;
  for (let i = 0; i < words.length; i++) {
    const w = String(words[i]);
    if (/^\s*[[{]/.test(w)) {
      const v = tokenValue(w);
      if (typeof v !== 'object' || v === null) throw new Error(`not a JSON step: ${w}`);
      for (const s of [].concat(v)) steps.push(s);
      cur = null;
      continue;
    }
    if (ACTIONS.includes(w)) {
      const next = words[i + 1];
      if (NO_VALUE.has(w)) {
        cur = { [w]: true };
      } else if (w in OPTIONAL_NUMBER) {
        const n = next !== undefined && next !== '' ? Number(next) : NaN;
        cur = { [w]: Number.isFinite(n) ? n : OPTIONAL_NUMBER[w] };
        if (Number.isFinite(n)) i++;
      } else {
        if (next === undefined) throw new Error(`"${w}" needs a value`);
        cur = { [w]: STRING_VALUE.has(w) ? String(next) : tokenValue(next) };
        i++;
      }
      steps.push(cur);
      continue;
    }
    const m = /^([a-zA-Z]\w*)=(.*)$/s.exec(w);
    if (m && cur) {
      cur[m[1]] = tokenValue(m[2]);
      continue;
    }
    throw new Error(`unknown step word ${JSON.stringify(w)} (actions: ${ACTIONS.join(', ')})`);
  }
  return steps;
}

/**
 * Runs `steps` in order. Calls onResult({step, action, ok, time, ...}) per
 * step; stops at the first failure unless keepGoing. Returns {ok, failed}.
 */
export async function runSteps(app, steps, { onResult = () => {}, keepGoing = false } = {}) {
  let failed = 0;
  for (let i = 0; i < steps.length; i++) {
    const s = steps[i];
    const errorsBefore = app.errors.length;
    let out;
    try {
      out = { step: i, ...(await runStep(app, s)), ok: true };
    } catch (e) {
      out = { step: i, action: actionOf(s), ok: false, error: e.message };
      if (e.element !== undefined) out.element = e.element;
      if (e.value !== undefined) out.value = e.value;
    }
    if (s && s.label) out.label = s.label;
    out.time = Math.round(app.time);
    if (app.errors.length > errorsBefore) out.jsErrors = app.errors.slice(errorsBefore);
    onResult(out);
    if (!out.ok) {
      failed++;
      if (!keepGoing) break;
    }
  }
  return { ok: failed === 0, failed };
}
