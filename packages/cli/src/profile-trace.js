// A CPU profile from a native runtime (Profiler.stop) as a Chrome trace:
// the JS samples on the Main lane, and the runtime's frame phases (the
// profile's `jsuiSpans`) as lanes of their own in a "JSUI" group, which
// DevTools' Performance panel draws as custom tracks (console.timeStamp
// entries). The live recording can only show them inside the JS flame chart
// (DevTools records a CPU profile, not a trace, from a Node-style target).

const PHASES = /^\((commit|style|layout|animated|input|paint|gpu)\)$/;
const LABELS = {
  commit: 'Commit',
  style: 'Style',
  layout: 'Layout',
  animated: 'Animated',
  input: 'Input',
  paint: 'Paint',
  gpu: 'GPU submit',
};

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

  for (const [name, start, end] of profile.jsuiSpans || []) {
    const m = PHASES.exec(name);
    const label = m ? LABELS[m[1]] : name;
    const gpu = m && m[1] === 'gpu';
    ev.push({
      ph: 'I', s: 't', cat: 'devtools.timeline', name: 'TimeStamp', pid, tid, ts: start,
      args: { data: { message: label, name: label, start, end, track: gpu ? 'GPU submit' : 'Render thread', trackGroup: 'JSUI', color: gpu ? 'tertiary' : 'primary' } },
    });
  }
  return { traceEvents: ev, metadata: { source: 'jsui dev' } };
}
