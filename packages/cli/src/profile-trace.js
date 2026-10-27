// A CPU profile from a native runtime (Profiler.stop) as a Chrome trace, so
// DevTools' Performance panel shows it as it shows a page's: the JS samples
// on Main, the runtime's frame phases (the profile's `jsuiSpans`) on a
// "JSUI render thread" track as Chrome's own Layout / Paint / ... events
// (colored Rendering, Painting), worklet runs on a "JSUI worklets" track,
// and GPU submits on the GPU track. The live
// recording can only show them inside the JS flame chart: DevTools records
// a CPU profile, not a trace, from a Node-style target.
//
// The events carry the fields DevTools' trace engine reads (a Paint without
// args.data.layerId fails the whole trace). Thread names matter: DevTools
// hides "Compositor", takes "CrRendererMain" for the main thread and the
// "GPU Process" / "CrGpuMain" pair for the GPU track.

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

  // The phases have lanes of their own: their samples count as idle here.
  const phase = new Set(profile.nodes.filter((n) => PHASES.test(n.callFrame.functionName)).map((n) => n.id));
  const idle = profile.nodes.find((n) => n.callFrame.functionName === '(idle)');
  const nodes = profile.nodes.filter((n) => !phase.has(n.id)).map((n) => ({ ...n, children: (n.children || []).filter((c) => !phase.has(c)) }));
  const samples = profile.samples.map((s) => (phase.has(s) && idle ? idle.id : s));
  ev.push({ ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'Profile', id: '0x1', pid, tid, ts: profile.startTime, args: { data: { startTime: profile.startTime } } });
  ev.push({
    ph: 'P', cat: 'disabled-by-default-v8.cpu_profiler', name: 'ProfileChunk', id: '0x1', pid, tid, ts: profile.endTime,
    args: { data: { cpuProfile: { nodes, samples }, timeDeltas: profile.timeDeltas } },
  });

  const render = 2, worklets = 4, gpuPid = 2, gpuTid = 3;
  ev.push({ ph: 'M', name: 'thread_name', pid, tid: render, ts: 0, args: { name: 'JSUI render thread' } });
  ev.push({ ph: 'M', name: 'thread_name', pid, tid: worklets, ts: 0, args: { name: 'JSUI worklets' } });
  ev.push({ ph: 'M', name: 'process_name', pid: gpuPid, tid: gpuTid, ts: 0, args: { name: 'GPU Process' } });
  ev.push({ ph: 'M', name: 'thread_name', pid: gpuPid, tid: gpuTid, ts: 0, args: { name: 'CrGpuMain' } });
  for (const [name, start, end] of profile.jsuiSpans || []) {
    const m = PHASES.exec(name);
    if (!m) continue;
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
      case 'worklets':  // they run on the render thread; a track of their own reads better
        ev.push({ ...at, tid: worklets, cat: 'devtools.timeline', name: 'FunctionCall', args: { data: { functionName: 'worklets', url: '', scriptId: '0', lineNumber: 0, columnNumber: 0, frame: 'F1' } } });
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
