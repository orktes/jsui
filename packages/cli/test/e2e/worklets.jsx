// End-to-end fixture for tests/native/react_worklets_e2e_test.cpp:
// react-native-worklets on JSUI (the CLI's worklets transform, the
// runtime's worklet runtimes). 400 x 300. Each host message runs one case
// and posts what it saw:
// - closure: an auto-workletized runOnUI callback with captured numbers,
//   objects, a nested worklet and a remote function (runOnJS);
// - nested: a worklet passed as an argument, scheduleOnRN back;
// - sync / syncThrow / async / asyncErr: runOnUISync and runOnUIAsync
//   results (values that JSON cannot carry among them) and errors;
// - error: a worklet that throws (reported with its location);
// - drive: a UI-runtime requestAnimationFrame loop sets an Animated value
//   every frame (the box's translateX), no JS involved;
// - counter: a synchronizable bumped from the UI runtime;
// - worker: a createWorkletRuntime runtime (async and sync);
// - timer: setTimeout on the UI runtime;
// - remotes: how many remote functions the bundle still holds.
import React from 'react';
import { AppRegistry, View, Animated } from 'react-native';
import { Host, useHostMessage } from '@orktes/jsui-react';
import {
  runOnUI,
  runOnJS,
  scheduleOnUI,
  scheduleOnRN,
  runOnUISync,
  runOnUIAsync,
  createWorkletRuntime,
  runOnRuntime,
  runOnRuntimeSync,
  isWorkletFunction,
  createSynchronizable,
  getRuntimeKind,
} from 'react-native-worklets';
import { __internal } from '@orktes/jsui-react/worklets';

const post = (name, value) => Host.postMessage(name, value);
const K = 7;
const cfg = { scale: 2, list: [1, 2, 3], nested: { s: 'hi' } };

function square(x) {
  'worklet';
  return x * x;
}

function sumList(o) {
  'worklet';
  return o.list.reduce((a, b) => a + b, 0) * o.scale;
}

const box = new Animated.Value(0);
const counter = createSynchronizable(0);
let worker = null;

function App() {
  useHostMessage('closure', () => {
    const report = (v) => post('closure', v);
    runOnUI((n) => {
      runOnJS(report)({ kind: getRuntimeKind(), n: square(n) + K, sum: sumList(cfg), s: cfg.nested.s, isWorklet: isWorkletFunction(square) });
    })(3);
  });
  useHostMessage('nested', () => {
    const report = (v) => post('nested', v);
    scheduleOnUI((f, x) => {
      scheduleOnRN(report, f(x));
    }, square, 9);
  });
  useHostMessage('sync', () => {
    const r = runOnUISync((a, b) => ({
      sum: a + b,
      kind: getRuntimeKind(),
      u: undefined,
      nan: NaN,
      neg: -0,
      list: [1, 'x', null],
      f32: new Float32Array([1.5, 2.5]),
      map: new Map([['a', 1]]),
      set: new Set([3]),
    }), 2, 3);
    post('sync', {
      sum: r.sum,
      kind: r.kind,
      u: 'u' in r && r.u === undefined,
      nan: Number.isNaN(r.nan),
      neg: Object.is(r.neg, -0),
      list: r.list,
      f32: r.f32 instanceof Float32Array ? Array.from(r.f32) : null,
      map: r.map.get('a'),
      set: r.set.has(3),
    });
  });
  useHostMessage('syncThrow', () => {
    try {
      runOnUISync(() => {
        throw new TypeError('bad input');
      });
      post('syncThrow', 'no error');
    } catch (e) {
      post('syncThrow', `${e.name}: ${e.message}`);
    }
  });
  useHostMessage('async', () => {
    runOnUIAsync((x) => x * 2, 21).then((v) => post('async', v));
    runOnUIAsync(() => {
      throw new Error('nope');
    }).catch((e) => post('asyncErr', e.message));
  });
  useHostMessage('error', () => {
    runOnUI(function boom() {
      'worklet';
      throw new Error('kaboom');
    })();
  });
  useHostMessage('drive', () => {
    const done = (v) => post('driveEnd', v);
    runOnUI(() => {
      const t0 = performance.now();
      const step = (t) => {
        box.setValue((t - t0) / 10);
        if (t - t0 < 160) requestAnimationFrame(step);
        else runOnJS(done)(box.getValue());
      };
      requestAnimationFrame(step);
    })();
  });
  useHostMessage('counter', () => {
    const done = (v) => post('counter', { ui: v, js: counter.getBlocking() });
    runOnUI(() => {
      let n = 0;
      const bump = () => {
        counter.setBlocking((x) => x + 1);
        if (++n < 5) requestAnimationFrame(bump);
        else runOnJS(done)(counter.getDirty());
      };
      requestAnimationFrame(bump);
    })();
  });
  useHostMessage('worker', () => {
    if (!worker) worker = createWorkletRuntime('background');
    const report = (v) => post('worker', v);
    runOnRuntime(worker, (x) => {
      runOnJS(report)({ kind: getRuntimeKind(), v: x + 1 });
    })(41);
  });
  useHostMessage('workerSync', () => {
    post('workerSync', runOnRuntimeSync(worker, (x) => getRuntimeKind() * 100 + x, 5));
  });
  useHostMessage('timer', () => {
    const report = (v) => post('timer', v);
    runOnUI(() => {
      const t0 = performance.now();
      setTimeout(() => runOnJS(report)(performance.now() - t0), 50);
    })();
  });
  useHostMessage('remotes', () => post('remotes', __internal.remotes.size));
  useHostMessage('dev', () => post('dev', __DEV__));
  return (
    <View style={{ flex: 1, backgroundColor: '#123' }}>
      <Animated.View nativeID="box" style={{ width: 40, height: 40, backgroundColor: '#e84', transform: [{ translateX: box }] }} />
    </View>
  );
}

AppRegistry.registerComponent('main', () => App);
post('ready', true);
