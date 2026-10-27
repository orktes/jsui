// The Three.js engine demo (site/demo/index.html): the game of the engine
// examples (integrations/raylib, sdl3, sokol, unity, unreal, godot) in a
// Three.js WebGPU scene, with their shared HUD (integrations/shared/ui/app.jsx)
// on top. JSUI draws with Three.js' own GPUDevice (createJsui({ device })):
// the HUD is a texture surface the game composites over its frame in its
// own final pass, as an engine would.
//
// The game <-> UI contract is the native examples' (example_host.h):
//   * `game` shared state: engine, renderer, fps, units, paused, the
//     selected unit and what reached the game because the UI did not
//     consume it (clicks, wheel, keys, pad).
//   * `entities` frame data: x, y, visible, hue per unit in logical px,
//     every frame (mapBuffer), for the nameplates (no JS per frame).
//   * messages: game:spawn {count}, game:clear, game:pause {paused},
//     game:quit (back to the showcase here), ui:anchors, ui:selected.
//
// Input pass-through: every pointer, wheel and key event goes to the HUD
// first (dispatchEvent returns whether it consumed it); the rest drives the
// game: click / tap a unit to select it (a raycast) or empty ground to clear
// the selection, drag to orbit, wheel / pinch to zoom, Space spawns, Esc /
// Start pauses, the right stick orbits.
import * as THREE from 'three/webgpu';
import { createJsui } from '../jsui-web.js';

const { sRGBTransferEOTF, texture, uv, vec4 } = THREE.TSL;

// The deployed WASM modules and their versions (index.html, set by
// site/scripts/build.mjs).
const modules = globalThis.jsuiModules ?? null;
const err = document.getElementById('err');
const splash = document.getElementById('splash');
let failed = false;
const fail = (msg) => {
  if (failed) return;  // the first failure is the cause
  failed = true;
  err.textContent = msg;
  err.style.display = 'block';
  splash.classList.add('gone');
};
// Whatever goes wrong shows up here, not as a blank page: errors outside
// main() and a lost device too (index.html covers a start that never ends).
addEventListener('error', (e) => fail(`${e.message}`));
addEventListener('unhandledrejection', (e) => fail(`${e.reason?.message ?? e.reason}`));

const MAX_UNITS = 48;  // the HUD's nameplates (MAX_PLATES)
const HUES = ['#f2c35b', '#4fd1ff', '#b26bff', '#4ade80', '#ff9a3c', '#4f8cff', '#ff4d6a'];
const TOUCH = matchMedia('(pointer: coarse)').matches;

// Frames per second over the last 60 frames after a 10-frame warm-up,
// shown at most every 250 ms (example_host.h FpsCounter).
class FpsCounter {
  times = new Float64Array(60);
  seen = 0;
  shownAt = -1;
  fps = 0;
  frame(now) {
    if (this.seen++ < 10) return;
    const n = this.seen - 10, W = this.times.length;
    this.times[(n - 1) % W] = now;
    if (n < 20) return;
    const oldest = this.times[n < W ? 0 : n % W], count = Math.min(n, W);
    if (now <= oldest || (this.shownAt >= 0 && now - this.shownAt < 250)) return;
    this.fps = Math.round(((count - 1) * 1000) / (now - oldest));
    this.shownAt = now;
  }
}

async function main() {
  if (!navigator.gpu) return fail('This demo needs WebGPU (Chrome, Edge, Safari 26 or Firefox 141+).');
  // A page loaded in the background (a link opened from another app, a
  // prerender) starts once it is shown: its canvas is configured visible.
  if (document.visibilityState === 'hidden') {
    await new Promise((r) => document.addEventListener('visibilitychange', r, { once: true }));
  }
  const canvas = document.getElementById('scene');
  const renderer = new THREE.WebGPURenderer({ canvas, antialias: true });
  await renderer.init();
  if (!renderer.backend.isWebGPUBackend) return fail('Three.js fell back to WebGL: this demo needs WebGPU.');
  renderer.onDeviceLost = (info) => fail(`The WebGPU device was lost (${info.reason ?? 'unknown'}: ${info.message}). `
    + 'Reload the page to start again.');
  const ratio = Math.min(devicePixelRatio, 2);
  renderer.setPixelRatio(ratio);
  renderer.setSize(innerWidth, innerHeight);
  // No tone mapping: it would apply to the HUD too (see site/three/main.js).

  // JSUI on Three.js' device. The main surface stays empty ('Main'); the
  // HUD is a texture surface of the whole viewport at the canvas' ratio.
  const jsui = await createJsui({
    device: renderer.backend.device,
    bundleUrl: './ui.js',
    component: 'Main',
    moduleBase: new URL('../jsui/', location.href),
    moduleVersions: modules?.versions,
  });
  window.jsui = jsui;
  jsui.on('error', fail);
  jsui.on('devicelost', () => fail('The WebGPU device was lost. Reload the page to start again.'));
  const hud = jsui.createTextureSurface({ component: 'App', width: innerWidth, height: innerHeight, pixelRatio: ratio });
  hud.focus();  // gamepads go to the HUD

  // The HUD texture as a Three.js color node: rgba8unorm with sRGB-encoded,
  // premultiplied values and top-down rows (see site/three/main.js).
  const uiTexture = (gpuTexture) => Object.assign(new THREE.ExternalTexture(gpuTexture), {
    generateMipmaps: false, minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter, premultiplyAlpha: false,
  });
  const hudNode = texture(uiTexture(hud.texture), uv().flipY());
  const hudColor = vec4(sRGBTransferEOTF(hudNode.rgb.div(hudNode.a.max(1e-4))).mul(hudNode.a), hudNode.a);
  hud.on('texture', (t) => { hudNode.value = uiTexture(t); });  // a new size: a new texture

  // ---------------------------------------------------------------- scene
  const scene = new THREE.Scene();
  scene.background = new THREE.Color('#070a14');
  scene.fog = new THREE.Fog('#070a14', 34, 80);
  const camera = new THREE.PerspectiveCamera(50, innerWidth / innerHeight, 0.1, 200);
  const target = new THREE.Vector3(0, 0.8, 0);
  scene.add(new THREE.HemisphereLight('#9fb4ff', '#1a1420', 1.6));
  const sun = new THREE.DirectionalLight('#ffffff', 2.2);
  sun.position.set(6, 12, 4);
  scene.add(sun);

  const ground = new THREE.Mesh(new THREE.PlaneGeometry(40, 40),
    new THREE.MeshStandardNodeMaterial({ color: '#0e1322', roughness: 0.9 }));
  ground.rotation.x = -Math.PI / 2;
  scene.add(ground);
  const grid = new THREE.GridHelper(40, 40, '#9aa3b8', '#9aa3b8');
  Object.assign(grid.material, { transparent: true, opacity: 0.55 });
  grid.position.y = 0.003;
  scene.add(grid);
  // The pillar in the middle, with its wire outline.
  const pillarGeo = new THREE.CylinderGeometry(0.9, 1.2, 2.2, 24);
  const pillar = new THREE.Mesh(pillarGeo, new THREE.MeshStandardNodeMaterial({ color: '#1a2440', roughness: 0.6 }));
  pillar.position.y = 1.1;
  pillar.add(new THREE.LineSegments(new THREE.EdgesGeometry(pillarGeo, 1),
    new THREE.LineBasicNodeMaterial({ color: '#4fd1ff', transparent: true, opacity: 0.6 })));
  scene.add(pillar);

  // Units: a cube each, with a wire outline and a shadow disc; the selected
  // one glows, gets a white outline and pulsing rings on the ground.
  const cubeGeo = new THREE.BoxGeometry(1, 1, 1);
  const cubeEdges = new THREE.EdgesGeometry(cubeGeo);
  const edgeMat = new THREE.LineBasicNodeMaterial({ color: '#ffffff', transparent: true, opacity: 0.35 });
  const discGeo = new THREE.CircleGeometry(0.6, 32);
  const discMat = new THREE.MeshBasicNodeMaterial({ color: '#000000', transparent: true, opacity: 0.35, depthWrite: false });
  const units = [];
  let spawned = 0;
  const spawn = () => {
    if (units.length >= MAX_UNITS) {
      const old = units.shift();
      scene.remove(old.mesh, old.disc);
      old.mesh.material.dispose();
      if (state.selected >= 0) state.selected--;  // -1 if it was the one removed
    }
    const i = spawned++;
    const size = 0.6 + Math.floor(Math.random() * 7) / 10;
    const color = new THREE.Color(HUES[i % HUES.length]);
    const mesh = new THREE.Mesh(cubeGeo, new THREE.MeshStandardNodeMaterial({ color, roughness: 0.45,
      emissive: color, emissiveIntensity: 0.5 }));
    mesh.scale.setScalar(size);
    mesh.add(new THREE.LineSegments(cubeEdges, edgeMat));
    const disc = new THREE.Mesh(discGeo, discMat);
    disc.rotation.x = -Math.PI / 2;
    disc.scale.setScalar(size);
    scene.add(mesh, disc);
    units.push({
      angle: Math.random() * Math.PI * 2,
      radius: 2.5 + Math.random() * 7,
      speed: (Math.random() < 0.5 ? 1 : -1) * (0.2 + Math.random() * 0.6),
      height: 0.5 + Math.random() * 1.5,
      size, phase: Math.random() * Math.PI * 2, color, mesh, disc,
    });
  };
  const clear = () => {
    for (const u of units) {
      scene.remove(u.mesh, u.disc);
      u.mesh.material.dispose();
    }
    units.length = 0;
    state.selected = -1;
  };
  // The selection's outline and rings follow the selected unit.
  const outline = new THREE.LineSegments(cubeEdges, new THREE.LineBasicNodeMaterial({ color: '#ffffff' }));
  const ringMat = new THREE.MeshBasicNodeMaterial({ color: '#ffffff', side: THREE.DoubleSide, depthWrite: false });
  const rings = new THREE.Group();
  for (let k = 0; k < 3; k++) {
    const r = new THREE.Mesh(new THREE.RingGeometry(1 + k * 0.05, 1.025 + k * 0.05, 64), ringMat);
    r.rotation.x = -Math.PI / 2;
    rings.add(r);
  }
  rings.position.y = 0.02;
  scene.add(outline, rings);

  // ---------------------------------------------------------------- state
  const state = {
    engine: 'Three.js', renderer: `WebGPU · three r${THREE.REVISION} · one GPUDevice`,
    fps: 0, entities: 0, paused: false, selected: -1,
    passed: { clicks: 0, wheel: 0, keys: 0, pad: 0 },
    hints: TOUCH ? 'Tap a unit to select it · drag to orbit · pinch to zoom' : undefined,
  };
  let published = '';
  const publish = () => {
    state.entities = units.length;
    const json = JSON.stringify(state);
    if (json === published) return;
    published = json;
    jsui.setState('game', state);
  };
  for (let i = 0; i < 8; i++) spawn();

  // Safe-area insets (a notch, the home indicator) for the HUD: texture
  // surfaces have no DOM, so the page publishes them (useSafeAreaInsets).
  const probe = document.createElement('div');
  probe.style.cssText = 'position:fixed;visibility:hidden;pointer-events:none;padding:env(safe-area-inset-top) '
    + 'env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)';
  document.body.append(probe);
  const publishInsets = () => {
    const cs = getComputedStyle(probe);
    jsui.setState('jsui.safeAreaInsets', { top: parseFloat(cs.paddingTop) || 0, right: parseFloat(cs.paddingRight) || 0,
      bottom: parseFloat(cs.paddingBottom) || 0, left: parseFloat(cs.paddingLeft) || 0 });
  };
  publishInsets();

  // For the demo test (site/test/demo.mjs) and the ?debug overlay.
  const stats = Object.assign(window.demoStats ??= {}, { frames: 0, canvas, device: renderer.backend.device });
  // `freeze` stops the units without pausing (the test clicks them on a slow software GPU).
  const demo = window.demo = { state, hud, anchors: null, reported: null, unitOnScreen: null, pick: null, freeze: false };

  jsui.on('message', (name, data) => {
    if (name === 'game:spawn') {
      const n = Math.min(50, Math.max(1, data?.count | 0 || 1));
      for (let i = 0; i < n; i++) spawn();
    } else if (name === 'game:clear') clear();
    else if (name === 'game:pause') state.paused = !!data?.paused;
    else if (name === 'game:quit') location.href = '../#/examples';
    else if (name === 'ui:anchors') demo.anchors = data;
    else if (name === 'ui:selected') demo.reported = data?.index;
  });

  // ---------------------------------------------------------------- camera
  // Orbit around the middle; tall screens back off so the ring of units fits.
  let yaw = 0.8, pitch = 0.55, zoom = 1, orbitVelocity = 0;
  const baseDistance = () => 16 * Math.min(2, Math.max(1, 0.85 / camera.aspect));
  const placeCamera = () => {
    pitch = Math.min(1.35, Math.max(0.15, pitch));
    const d = baseDistance() * zoom;
    camera.position.set(Math.cos(yaw) * Math.cos(pitch) * d, Math.sin(pitch) * d, Math.sin(yaw) * Math.cos(pitch) * d);
    camera.lookAt(target);
    camera.updateMatrixWorld();
  };
  const zoomBy = (f) => { zoom = Math.min(2.5, Math.max(0.375, zoom * f)); };

  const unitPos = (u, out = new THREE.Vector3()) =>
    out.set(Math.cos(u.angle) * u.radius, u.height + 0.25 * Math.sin(time * 2 + u.phase), Math.sin(u.angle) * u.radius);

  // The unit under (x, y) (CSS px), nearest first, or -1.
  const raycaster = new THREE.Raycaster();
  const ndc = new THREE.Vector2();
  const pick = (x, y) => {
    ndc.set((x / innerWidth) * 2 - 1, -(y / innerHeight) * 2 + 1);
    raycaster.setFromCamera(ndc, camera);
    const hits = raycaster.intersectObjects(units.map((u) => u.mesh), false);
    return hits.length ? units.findIndex((u) => u.mesh === hits[0].object) : -1;
  };
  const v = new THREE.Vector3();
  const unitOnScreen = (i) => {
    const u = units[i];
    if (!u) return null;
    unitPos(u, v).applyMatrix4(camera.matrixWorldInverse);
    if (v.z > -0.1) return null;  // behind the camera
    unitPos(u, v).project(camera);
    const x = (v.x + 1) / 2 * innerWidth, y = (1 - v.y) / 2 * innerHeight;
    return x >= 0 && y >= 0 && x < innerWidth && y < innerHeight ? { x, y } : null;
  };
  Object.assign(demo, { pick, unitOnScreen });

  // ---------------------------------------------------------------- input
  // Presses the HUD did not consume are the game's: a click / tap (released
  // within a few px) selects, a drag orbits, two fingers pinch-zoom.
  const game = new Map();  // pointerId -> { x, y, x0, y0, travel, slop }
  let pinch = 0;
  const spread = () => {
    const [a, b] = [...game.values()];
    return Math.hypot(a.x - b.x, a.y - b.y);
  };
  canvas.addEventListener('pointerdown', (e) => {
    const consumed = hud.dispatchEvent(e, e.clientX, e.clientY);
    canvas.style.cursor = hud.cursor;
    if (consumed || e.button !== 0) return;
    state.passed.clicks++;
    canvas.setPointerCapture(e.pointerId);
    game.set(e.pointerId, { x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY, travel: 0,
      slop: e.pointerType === 'mouse' ? 4 : 10 });
    if (game.size === 2) pinch = spread();
  });
  canvas.addEventListener('pointermove', (e) => {
    hud.dispatchEvent(e, e.clientX, e.clientY);
    canvas.style.cursor = hud.cursor;
    const p = game.get(e.pointerId);
    if (!p) return;
    const dx = e.clientX - p.x, dy = e.clientY - p.y;
    p.x = e.clientX;
    p.y = e.clientY;
    p.travel = Math.max(p.travel, Math.hypot(p.x - p.x0, p.y - p.y0));
    if (game.size >= 2) {
      const d = spread();
      if (pinch > 0 && d > 0) zoomBy(pinch / d);
      pinch = d;
      for (const q of game.values()) q.travel = Infinity;  // a pinch is never a tap
    } else {
      const k = e.pointerType === 'mouse' ? 0.006 : 0.008;
      yaw += dx * k;
      pitch += dy * k;
    }
  });
  const release = (e) => {
    hud.dispatchEvent(e, e.clientX, e.clientY);
    const p = game.get(e.pointerId);
    if (!p) return;
    game.delete(e.pointerId);
    pinch = game.size === 2 ? spread() : 0;
    if (e.type === 'pointerup' && p.travel <= p.slop && game.size === 0) state.selected = pick(e.clientX, e.clientY);
  };
  canvas.addEventListener('pointerup', release);
  canvas.addEventListener('pointercancel', release);
  canvas.addEventListener('pointerleave', (e) => {
    if (e.pointerType === 'mouse') hud.pointerLeave();
  });
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (hud.dispatchEvent(e, e.clientX, e.clientY)) return;
    state.passed.wheel++;
    zoomBy(e.deltaY > 0 ? 1.1 : 1 / 1.1);
  }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  addEventListener('keydown', (e) => {
    if (hud.dispatchEvent(e)) {
      e.preventDefault();
      return;
    }
    if (e.repeat) return;
    state.passed.keys++;
    if (e.key === ' ') {
      e.preventDefault();
      spawn();
    } else if (e.key === 'Escape') state.paused = !state.paused;
  });
  addEventListener('keyup', (e) => hud.dispatchEvent(e));

  // Gamepads: JSUI polls them for the HUD (focus moves, A presses); what it
  // did not take is the game's: Start pauses, the right stick orbits.
  const padDown = [];
  const pollPads = () => {
    orbitVelocity = 0;
    const pads = navigator.getGamepads?.() ?? [];
    for (let i = 0; i < pads.length; i++) {
      const p = pads[i];
      if (!p?.connected) continue;
      const was = padDown[i] ??= [];
      p.buttons.forEach((b, k) => {
        if (b.pressed && !was[k] && !jsui.gamepadConsumed(i, k)) {
          state.passed.pad++;
          if (k === 9) state.paused = !state.paused;  // Start
        }
        was[k] = b.pressed;
      });
      const rx = p.axes[2] ?? 0;
      if (Math.abs(rx) > 0.15 && !jsui.gamepadAxisConsumed(i, 2)) orbitVelocity += rx * 1.8;
    }
  };

  // A resize, and the page shown again (from the back/forward cache or a
  // hidden tab): setSize gives the canvas a new drawing buffer as well.
  const resize = () => {
    renderer.setSize(innerWidth, innerHeight);
    camera.aspect = innerWidth / innerHeight;
    camera.updateProjectionMatrix();
    Object.assign(overlayCam, { right: innerWidth, top: innerHeight });
    overlayCam.updateProjectionMatrix();
    hudQuad.scale.set(innerWidth, innerHeight, 1);
    hudQuad.position.set(innerWidth / 2, innerHeight / 2, 0);
    hud.setSize(innerWidth, innerHeight, ratio);
    publishInsets();
  };

  // The HUD over the frame, in an overlay pass (premultiplied).
  const overlay = new THREE.Scene();
  const overlayCam = new THREE.OrthographicCamera(0, innerWidth, innerHeight, 0, -1, 1);
  const hudMat = new THREE.MeshBasicNodeMaterial({ colorNode: hudColor, transparent: true,
    blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
    depthTest: false, depthWrite: false });
  const hudQuad = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), hudMat);
  overlay.add(hudQuad);
  resize();
  addEventListener('resize', resize);
  visualViewport?.addEventListener('resize', resize);
  addEventListener('pageshow', resize);
  document.addEventListener('visibilitychange', () => document.visibilityState === 'visible' && resize());

  // ---------------------------------------------------------------- frame
  const clock = new THREE.Timer();
  const fps = new FpsCounter();
  let time = 0, hadPlates = false;
  renderer.autoClear = false;
  // iOS Safari (26.5) does not show a WebGPU canvas configured at load
  // until its drawing buffer changes size: one frame a pixel shorter, then
  // back, after the first frames and again at 0.5 s and 1.5 s (as
  // site/three/main.js).
  const nudges = [0, 500, 1500];
  let started = 0, restore = false;
  const nudge = () => {
    if (restore) {
      restore = false;
      renderer.setSize(innerWidth, innerHeight);
      return;
    }
    const now = performance.now();
    started ||= now;
    if (!nudges.length || stats.frames < 2 || now - started < nudges[0]) return;
    nudges.shift();
    stats.nudges = (stats.nudges ?? 0) + 1;
    renderer.setSize(innerWidth, innerHeight - 1, false);
    restore = true;
  };
  renderer.setAnimationLoop(() => {
    try {
      nudge();
      frame();
    } catch (e) {
      renderer.setAnimationLoop(null);
      fail(`${e?.message ?? e}`);
    }
  });
  const p = new THREE.Vector3();
  const frame = () => {
    clock.update();
    const dt = Math.min(clock.getDelta(), 0.1);
    pollPads();
    if (!state.paused && !demo.freeze) {
      time += dt;
      for (const u of units) u.angle += u.speed * dt;
    }
    yaw += orbitVelocity * dt;
    placeCamera();

    // Units, the selection, and the nameplates' frame data.
    const sel = units[state.selected];
    const plates = units.length ? jsui.mapBuffer('entities', Float32Array, units.length * 4) : null;
    units.forEach((u, i) => {
      unitPos(u, u.mesh.position);
      u.disc.position.set(u.mesh.position.x, 0.01, u.mesh.position.z);
      u.mesh.material.emissiveIntensity = u === sel ? 0.9 + 0.25 * Math.sin(time * 6) : 0.5;
      if (!plates) return;
      p.copy(u.mesh.position);
      p.y += u.size * 0.5 + 0.35;
      const inView = p.clone().applyMatrix4(camera.matrixWorldInverse).z < -0.1;
      p.project(camera);
      const x = Math.round((p.x + 1) / 2 * innerWidth), y = Math.round((1 - p.y) / 2 * innerHeight);
      plates[i * 4] = x;
      plates[i * 4 + 1] = y;
      plates[i * 4 + 2] = inView && x > -60 && y > -60 && x < innerWidth + 60 && y < innerHeight + 60 ? 1 : 0;
      plates[i * 4 + 3] = i % HUES.length;
    });
    if (plates) jsui.commitBuffer('entities');
    else if (hadPlates) jsui.setBuffer('entities', new Float32Array(0));
    hadPlates = !!plates;
    outline.visible = rings.visible = !!sel;
    if (sel) {
      outline.position.copy(sel.mesh.position);
      outline.scale.setScalar(sel.size * 1.18);
      rings.position.set(sel.mesh.position.x, 0.02, sel.mesh.position.z);
      rings.scale.setScalar(sel.size * (1.1 + 0.08 * Math.sin(time * 4)));
      ringMat.color.copy(sel.color);
    }

    renderer.clear();
    renderer.render(scene, camera);
    renderer.clearDepth();
    renderer.render(overlay, overlayCam);

    stats.frames++;
    fps.frame(performance.now());
    state.fps = fps.fps;
    publish();
  };
  splash.classList.add('gone');
}

main().catch((e) => fail(`${e?.message ?? e}`));
