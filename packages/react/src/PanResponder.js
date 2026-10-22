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

function update(gs, e) {
  const ne = e.nativeEvent;
  const t = ne.timestamp || e.timeStamp || 0;
  const x = ne.pageX;
  const y = ne.pageY;
  const prevX = gs.moveX;
  const prevY = gs.moveY;
  const dt = t - gs._accountsForMovesUpTo;
  gs.moveX = x;
  gs.moveY = y;
  gs.dx = x - gs.x0;
  gs.dy = y - gs.y0;
  if (dt > 0) {
    gs.vx = (x - prevX) / dt;
    gs.vy = (y - prevY) / dt;
  }
  gs._accountsForMovesUpTo = t;
  gs.numberActiveTouches = ne.touches ? ne.touches.length : 1;
}

export const PanResponder = {
  create(config) {
    const gs = newGestureState();
    const call = (name, e) => (config[name] ? config[name](e, gs) : undefined);
    const start = (e) => {
      const ne = e.nativeEvent;
      gs.x0 = gs.moveX = ne.pageX;
      gs.y0 = gs.moveY = ne.pageY;
      gs.dx = gs.dy = gs.vx = gs.vy = 0;
      gs._accountsForMovesUpTo = ne.timestamp || e.timeStamp || 0;
      gs.numberActiveTouches = 1;
    };
    const panHandlers = {
      onStartShouldSetResponder(e) {
        return !!call('onStartShouldSetPanResponder', e);
      },
      onMoveShouldSetResponder(e) {
        update(gs, e);
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
        update(gs, e);
        call('onPanResponderMove', e);
      },
      onResponderEnd(e) {
        call('onPanResponderEnd', e);
      },
      onResponderRelease(e) {
        update(gs, e);
        gs.numberActiveTouches = 0;
        call('onPanResponderRelease', e);
        Object.assign(gs, newGestureState());
      },
      onResponderTerminate(e) {
        call('onPanResponderTerminate', e);
        Object.assign(gs, newGestureState());
      },
      onResponderTerminationRequest(e) {
        return config.onPanResponderTerminationRequest ? !!config.onPanResponderTerminationRequest(e, gs) : true;
      },
    };
    return { panHandlers, getInteractionHandle: () => null };
  },
};

export default PanResponder;
