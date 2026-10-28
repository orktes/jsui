// A CPU profile from a native runtime (Profiler.stop) as a Chrome trace, so
// DevTools' Performance panel shows it as it shows a page's: the JS samples
// on Main under what called into JS (Timer fired, Event: press, ...), the
// runtime's frame phases (the profile's `jsuiSpans`) on a
// "JSUI render thread" track as Chrome's own Layout / Paint / ... events
// (colored Rendering, Painting), worklet runs on a JSUI > Worklets track,
// and GPU submits on the GPU track. The relay sends these to a DevTools
// that opened the runtime as a page (Tracing.dataCollected, inspect.js);
// opened Node-style, DevTools records a CPU profile, not a trace.
//
// The events carry the fields DevTools' trace engine reads (a Paint without
// args.data.layerId fails the whole trace). Thread names matter: DevTools
// hides "Compositor", takes "CrRendererMain" for the main thread and the
// "GPU Process" / "CrGpuMain" pair for the GPU track.

const SPECIAL = new Set(['(root)', '(program)', '(idle)', '(garbage collector)']);
const PHASES = /^\((commit|style|layout|animated|input|paint|gpu|worklets)\)$/;

/**
 * @param {object} profile  Profiler.Profile with `jsuiSpans` ([name, start, end] µs)
 * @param {{name?: string, url?: string}} [o]  process name and page URL to show
 * @returns {{traceEvents: object[], metadata: object}}
 */
export function profileTrace(profile, o = {}) {
  const pid = 1, tid = 1;
  const ev = [
    { ph: 'M', name: 'process_name', pid, tid, ts: 0, args: { name: o.name || 'jsui' } },
    // The thread DevTools takes for the page's main thread.
    { ph: 'M', name: 'thread_name', pid, tid, ts: 0, args: { name: 'CrRendererMain' } },
    {
      ph: 'I', s: 't', cat: 'disabled-by-default-devtools.timeline', name: 'TracingStartedInBrowser', pid, tid,
      ts: profile.startTime,
      args: { data: { frameTreeNodeId: 1, persistentIds: true, frames: [{ frame: 'F1', url: o.url || 'jsui://app', name: '', processId: pid }] } },
    },
  ];

  // The phases have tracks of their own: their samples count as idle here.
  // What called into JS ("(timer)", ...) becomes a trace event above the JS
  // (below), so its node goes and its children hang off the root.
  const label = (n) => /^\(.*\)$/.test(n.callFrame.functionName) && !SPECIAL.has(n.callFrame.functionName);
  const phase = new Set(profile.nodes.filter((n) => PHASES.test(n.callFrame.functionName)).map((n) => n.id));
  const entry = new Set(profile.nodes.filter((n) => label(n) && !phase.has(n.id)).map((n) => n.id));
  const idle = profile.nodes.find((n) => n.callFrame.functionName === '(idle)');
  const program = profile.nodes.find((n) => n.callFrame.functionName === '(program)');
  const lifted = profile.nodes.filter((n) => entry.has(n.id)).flatMap((n) => n.children || []);
  const nodes = profile.nodes
    .filter((n) => !phase.has(n.id) && !entry.has(n.id))
    .map((n) => ({
      ...n,
      children: [...(n.children || []).filter((c) => !phase.has(c) && !entry.has(c)), ...(n.callFrame.functionName === '(root)' ? lifted : [])],
    }));
  const samples = profile.samples.map((s) => (phase.has(s) && idle ? idle.id : entry.has(s) && program ? program.id : s));
  ev.push({ ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'Profile', id: '0x1', pid, tid, ts: profile.startTime, args: { data: { startTime: profile.startTime } } });
  ev.push({
    ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'ProfileChunk', id: '0x1', pid, tid, ts: profile.endTime,
    args: { data: { cpuProfile: { nodes, samples }, timeDeltas: profile.timeDeltas } },
  });

  // Worklets sampled: a thread of their own with its CPU profile (the
  // timing-only Worklets track then isn't needed).
  const worklets = workletProfile(profile.jsuiWorklets, profile.startTime, profile.endTime);
  if (worklets) {
    const wtid = 4;
    ev.push({ ph: 'M', name: 'thread_name', pid, tid: wtid, ts: 0, args: { name: 'JSUI worklets' } });
    ev.push({ ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'Profile', id: '0x2', pid, tid: wtid, ts: worklets.startTime, args: { data: { startTime: worklets.startTime } } });
    ev.push({
      ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'ProfileChunk', id: '0x2', pid, tid: wtid, ts: worklets.endTime,
      args: { data: { cpuProfile: { nodes: worklets.nodes, samples: worklets.samples }, timeDeltas: worklets.timeDeltas } },
    });
  }

  const render = 2, gpuPid = 2, gpuTid = 3;
  ev.push({ ph: 'M', name: 'thread_name', pid, tid: render, ts: 0, args: { name: 'JSUI render thread' } });
  ev.push({ ph: 'M', name: 'process_name', pid: gpuPid, tid: gpuTid, ts: 0, args: { name: 'GPU Process' } });
  ev.push({ ph: 'M', name: 'thread_name', pid: gpuPid, tid: gpuTid, ts: 0, args: { name: 'CrGpuMain' } });
  for (const [name, start, end] of profile.jsuiSpans || []) {
    const m = PHASES.exec(name);
    if (!m) {
      ev.push(...entryEvents(name, start, end, pid, tid));
      continue;
    }
    const at = { ph: 'X', pid, tid: render, ts: start, dur: Math.max(1, end - start) };
    switch (m[1]) {
      case 'layout':
        ev.push({ ...at, cat: 'devtools.timeline', name: 'Layout', args: { beginData: { frame: 'F1', dirtyObjects: 0, partialLayout: false, totalObjects: 0 }, endData: { layoutRoots: [] } } });
        break;
      case 'style':
        ev.push({ ...at, cat: 'devtools.timeline', name: 'UpdateLayoutTree', args: { elementCount: 0, beginData: { frame: 'F1' } } });
        break;
      case 'paint':
        ev.push({ ...at, cat: 'devtools.timeline,rail', name: 'Paint', args: { data: { frame: 'F1', layerId: 0, clip: [0, 0, 0, 0, 0, 0, 0, 0] } } });
        break;
      case 'input':
        ev.push({ ...at, cat: 'devtools.timeline', name: 'EventDispatch', args: { data: { type: 'input', frame: 'F1' } } });
        break;
      case 'worklets':  // a custom track (console.timeStamp's), so it carries its own name
        if (worklets) break;  // the sampled thread shows them
        ev.push({ ph: 'I', s: 't', pid, tid, ts: start, cat: 'devtools.timeline', name: 'TimeStamp', args: { data: { message: 'Worklets', name: 'Worklets', start, end, track: 'Worklets', trackGroup: 'JSUI', color: 'secondary' } } });
        break;
      case 'gpu':
        ev.push({ ...at, pid: gpuPid, tid: gpuTid, cat: 'disabled-by-default-devtools.timeline', name: 'GPUTask', args: { data: { renderer_pid: pid, used_bytes: 0 } } });
        break;
      default:  // commit, animated: tree and native Animated writes
        ev.push({ ...at, cat: 'devtools.timeline', name: 'Commit', args: { frame: 'F1' } });
    }
  }
  return { traceEvents: ev, metadata: { source: 'jsui dev' } };
}

// What called into JS, as Chrome's own events on Main inside a task, so the
// JS samples nest under them the way a page's do.
function entryEvents(name, start, end, pid, tid) {
  const at = { ph: 'X', pid, tid, ts: start, dur: Math.max(1, end - start) };
  const m = /^\(([^:)]+)(?:: (.*))?\)$/.exec(name) || [];
  const [kind, detail] = [m[1] || name, m[2] || ''];
  let ev;
  if (kind === 'timer') ev = { name: 'TimerFire', args: { data: { timerId: 0, frame: 'F1' } } };
  else if (kind === 'animation frame') ev = { name: 'FireAnimationFrame', args: { data: { id: 0, frame: 'F1' } } };
  else if (kind === 'event') ev = { name: 'EventDispatch', args: { data: { type: detail, frame: 'F1' } } };
  else if (kind === 'script') ev = { name: 'EvaluateScript', args: { data: { url: detail, lineNumber: 0, columnNumber: 0, frame: 'F1' } } };
  else if (kind === 'microtasks') ev = { name: 'RunMicrotasks', args: {} };
  else ev = { name: 'FunctionCall', args: { data: { functionName: name, url: '', scriptId: '0', lineNumber: 0, columnNumber: 0, frame: 'F1' } } };
  return [
    { ...at, cat: 'disabled-by-default-devtools.timeline', name: 'RunTask', args: {} },
    { ...at, cat: 'devtools.timeline', ...ev },
  ];
}

// profile.jsuiWorklets ({frames: [[name, url, line, col]], samples: [[time,
// frame indices outermost first | null when idle]]}) as a CPU profile.
function workletProfile(w, startTime, endTime) {
  if (!w || !Array.isArray(w.samples) || !w.samples.some(([, stack]) => stack && stack.length)) return null;
  const cf = (functionName, url = '', lineNumber = -1, columnNumber = -1) => ({ functionName, scriptId: '0', url, lineNumber, columnNumber });
  const nodes = [
    { id: 1, callFrame: cf('(root)'), children: [] },
    { id: 2, callFrame: cf('(program)'), children: [] },
    { id: 3, callFrame: cf('(idle)'), children: [] },
  ];
  nodes[0].children.push(2, 3);
  const child = new Map();  // parent id + frame index -> node id
  const samples = [], timeDeltas = [];
  let prev = startTime;
  for (const [time, stack] of w.samples) {
    let id = 3;
    if (stack) {
      id = 1;
      for (const f of stack) {
        const key = id + ':' + f;
        let next = child.get(key);
        if (!next) {
          const [name, url, line, col] = w.frames[f];
          next = nodes.length + 1;
          nodes.push({ id: next, callFrame: cf(name || '(anonymous)', url, line > 0 ? line - 1 : -1, col > 0 ? col - 1 : -1), children: [] });
          nodes[id - 1].children.push(next);
          child.set(key, next);
        }
        id = next;
      }
      if (id === 1) id = 2;
    }
    samples.push(id);
    timeDeltas.push(Math.max(0, time - prev));
    prev = Math.max(prev, time);
  }
  // Idle from the start of the recording to the first sample.
  samples.unshift(3);
  timeDeltas.unshift(0);
  timeDeltas[1] = Math.max(0, w.samples[0][0] - startTime);
  return { nodes, samples, timeDeltas, startTime, endTime };
}
