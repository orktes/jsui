// PanResponder: RN gesture helper on top of the responder props handled by
// the event system (onStartShouldSetResponder, onResponderMove, ...).

function newGestureState() {
  return {
    stateID: Math.random(),
    moveX: 0,
    moveY: 0,
    x0: 0,
    y0: 0,
    dx: 0,
    dy: 0,
    vx: 0,
    vy: 0,
    numberActiveTouches: 0,
    _accountsForMovesUpTo: 0,
  };
}

// Velocity (vx / vy, px/ms like React Native) is measured over the moves of
// roughly the last VELOCITY_WINDOW ms, from the events' timestamps (the
// host's event time when it reports one, else the frame time). Events that
// share a timestamp (several delivered within one frame) count as one
// sample at their latest position, so the velocity is never 0 / 0 or
// infinite. The release keeps the velocity of the moves before it, as RN's
// PanResponder does, unless the pointer rested longer than the window
// before letting go.
const VELOCITY_WINDOW = 100;

function eventTime(e) {
  const ne = e.nativeEvent;
  const t = ne.timestamp != null ? ne.timestamp : e.timeStamp;
  return typeof t === 'number' && Number.isFinite(t) ? t : 0;
}

function addSample(samples, t, x, y) {
  const last = samples[samples.length - 1];
  if (last && t <= last.t) {
    // Same frame (or a host clock that did not advance): latest position wins.
    last.x = x;
    last.y = y;
  } else {
    samples.push({ t, x, y });
  }
  // Keep one sample at or before the window start as the anchor.
  const newest = samples[samples.length - 1].t;
  while (samples.length > 2 && samples[1].t <= newest - VELOCITY_WINDOW) samples.shift();
}

function update(gs, e, samples, release) {
  const ne = e.nativeEvent;
  const t = eventTime(e);
  const x = ne.pageX;
  const y = ne.pageY;
  const last = samples[samples.length - 1];
  const moved = !last || x !== last.x || y !== last.y;
  gs.moveX = x;
  gs.moveY = y;
  gs.dx = x - gs.x0;
  gs.dy = y - gs.y0;
  if (moved) {
    addSample(samples, t, x, y);
    const first = samples[0];
    const newest = samples[samples.length - 1];
    const dt = newest.t - first.t;
    if (dt > 0) {
      gs.vx = (newest.x - first.x) / dt;
      gs.vy = (newest.y - first.y) / dt;
    }
  } else if (release && last && t - last.t > VELOCITY_WINDOW) {
    // Rested before letting go.
    gs.vx = gs.vy = 0;
  }
  gs._accountsForMovesUpTo = t;
  gs.numberActiveTouches = ne.touches ? ne.touches.length : 1;
}

export const PanResponder = {
  create(config) {
    const gs = newGestureState();
    let samples = [];  // recent {t, x, y} of this gesture (velocity)
    const call = (name, e) => (config[name] ? config[name](e, gs) : undefined);
    const start = (e) => {
      const ne = e.nativeEvent;
      gs.x0 = gs.moveX = ne.pageX;
      gs.y0 = gs.moveY = ne.pageY;
      gs.dx = gs.dy = gs.vx = gs.vy = 0;
      gs._accountsForMovesUpTo = eventTime(e);
      samples = [{ t: gs._accountsForMovesUpTo, x: ne.pageX, y: ne.pageY }];
      gs.numberActiveTouches = 1;
    };
    const panHandlers = {
      onStartShouldSetResponder(e) {
        return !!call('onStartShouldSetPanResponder', e);
      },
      onMoveShouldSetResponder(e) {
        update(gs, e, samples, false);
        return !!call('onMoveShouldSetPanResponder', e);
      },
      onStartShouldSetResponderCapture(e) {
        start(e);
        return !!call('onStartShouldSetPanResponderCapture', e);
      },
      onMoveShouldSetResponderCapture(e) {
        return !!call('onMoveShouldSetPanResponderCapture', e);
      },
      onResponderGrant(e) {
        if (!gs.x0 && !gs.y0) start(e);
        gs.dx = gs.dy = 0;
        call('onPanResponderGrant', e);
        return config.onShouldBlockNativeResponder ? config.onShouldBlockNativeResponder(e, gs) : true;
      },
      onResponderReject(e) {
        call('onPanResponderReject', e);
      },
      onResponderStart(e) {
        call('onPanResponderStart', e);
      },
      onResponderMove(e) {
        update(gs, e, samples, false);
        call('onPanResponderMove', e);
      },
      onResponderEnd(e) {
        call('onPanResponderEnd', e);
      },
      onResponderRelease(e) {
        update(gs, e, samples, true);
        gs.numberActiveTouches = 0;
        call('onPanResponderRelease', e);
        Object.assign(gs, newGestureState());
        samples = [];
      },
      onResponderTerminate(e) {
        call('onPanResponderTerminate', e);
        Object.assign(gs, newGestureState());
        samples = [];
      },
      onResponderTerminationRequest(e) {
        return config.onPanResponderTerminationRequest ? !!config.onPanResponderTerminationRequest(e, gs) : true;
      },
    };
    return { panHandlers, getInteractionHandle: () => null };
  },
};

export default PanResponder;
