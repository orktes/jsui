// The Three.js example (site/three/index.html): a Three.js WebGPU scene and
// JSUI on one GPUDevice (createJsui({ device }), issue #183).
//
//   UI in the game   the 'Console' surface renders into a GPUTexture that a
//                    screen in the room samples (THREE.ExternalTexture); the
//                    mouse is raycast onto the screen and forwarded at the
//                    hit's UV. The 'Hud' surface is composited over the frame
//                    in Three's own final pass.
//   game in the UI   a drone camera renders the core into a Three.js render
//                    target whose GPUTexture the console shows with
//                    <HostTexture name="drone-cam"> (jsui.setTexture), no copy.
//
// Both directions stay on the GPU: nothing is read back or uploaded per frame.
import * as THREE from 'three/webgpu';
import { createJsui } from '../jsui-web.js';

// The deployed WASM modules and their versions (index.html, set by
// site/scripts/build.mjs).
const modules = globalThis.jsuiModules ?? null;
const err = document.getElementById('err');
const fail = (msg) => {
  err.textContent = msg;
  err.style.display = 'block';
};

const CONSOLE = { width: 640, height: 400, ratio: 2 };
const HUD = { width: 300, height: 128, ratio: 2 };

async function main() {
  if (!navigator.gpu) return fail('This example needs WebGPU (Chrome, Edge, Safari 26 or Firefox 141+).');
  const canvas = document.getElementById('scene');
  const renderer = new THREE.WebGPURenderer({ canvas, antialias: true });
  await renderer.init();
  if (!renderer.backend.isWebGPUBackend) return fail('Three.js fell back to WebGL: this example needs WebGPU.');
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  renderer.setSize(innerWidth, innerHeight);
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.1;

  // JSUI on Three.js' device.
  const jsui = await createJsui({
    device: renderer.backend.device,
    bundleUrl: './ui.js',
    moduleBase: new URL('../jsui/', location.href),
    moduleVersions: modules?.versions,
  });
  window.jsui = jsui;
  jsui.on('error', fail);
  const ui = jsui.createTextureSurface({ component: 'Console', width: CONSOLE.width, height: CONSOLE.height,
    pixelRatio: CONSOLE.ratio });
  const hud = jsui.createTextureSurface({ component: 'Hud', width: HUD.width, height: HUD.height, pixelRatio: HUD.ratio });

  // A JSUI texture as a Three.js texture (premultiplied, sRGB-encoded).
  const uiTexture = (gpuTexture) => Object.assign(new THREE.ExternalTexture(gpuTexture), {
    colorSpace: THREE.SRGBColorSpace, generateMipmaps: false, minFilter: THREE.LinearFilter,
    magFilter: THREE.LinearFilter, premultiplyAlpha: false,
  });

  // ---------------------------------------------------------------- scene
  const scene = new THREE.Scene();
  scene.background = new THREE.Color('#03050b');
  scene.fog = new THREE.Fog('#03050b', 9, 22);
  const camera = new THREE.PerspectiveCamera(42, innerWidth / innerHeight, 0.1, 60);
  camera.position.set(0, 1.55, 6.2);
  const lookAt = new THREE.Vector3(0, 1.25, 0);

  scene.add(new THREE.HemisphereLight('#4f78c8', '#0a0d16', 0.5));
  const key = new THREE.PointLight('#4fd1ff', 40, 12, 1.6);
  key.position.set(-1.6, 2.2, 1.2);
  scene.add(key);
  const rim = new THREE.PointLight('#ffb36b', 18, 12, 1.6);
  rim.position.set(2.8, 3, -2);
  scene.add(rim);

  // Floor: a dark plate and a glowing grid.
  const floor = new THREE.Mesh(new THREE.CircleGeometry(14, 64),
    new THREE.MeshStandardNodeMaterial({ color: '#070b16', roughness: 0.35, metalness: 0.6 }));
  floor.rotation.x = -Math.PI / 2;
  scene.add(floor);
  const grid = new THREE.GridHelper(28, 56, '#1b4a73', '#0d2238');
  grid.position.y = 0.002;
  scene.add(grid);

  // The reactor core: a torus knot in a ring, on a plinth.
  const coreColor = new THREE.Color('#4fd1ff');
  const coreMat = new THREE.MeshStandardNodeMaterial({ color: '#0b1a2a', emissive: coreColor.clone(),
    emissiveIntensity: 1.6, roughness: 0.25, metalness: 0.8 });
  const core = new THREE.Mesh(new THREE.TorusKnotGeometry(0.42, 0.13, 220, 32), coreMat);
  core.position.set(-1.55, 1.35, 0);
  scene.add(core);
  const ringMat = new THREE.MeshStandardNodeMaterial({ color: '#1a2438', roughness: 0.3, metalness: 0.9,
    emissive: coreColor.clone(), emissiveIntensity: 0.25 });
  const ring = new THREE.Mesh(new THREE.TorusGeometry(0.85, 0.035, 16, 120), ringMat);
  ring.position.copy(core.position);
  scene.add(ring);
  const plinth = new THREE.Mesh(new THREE.CylinderGeometry(0.55, 0.7, 0.4, 48),
    new THREE.MeshStandardNodeMaterial({ color: '#111a2c', roughness: 0.4, metalness: 0.85 }));
  plinth.position.set(core.position.x, 0.2, 0);
  scene.add(plinth);

  // The console: a tilted screen showing the 'Console' surface's texture.
  const aspect = CONSOLE.width / CONSOLE.height, screenH = 1.55, screenW = screenH * aspect;
  const screenMat = new THREE.MeshBasicNodeMaterial({ map: uiTexture(ui.texture), toneMapped: false });
  const screen = new THREE.Mesh(new THREE.PlaneGeometry(screenW, screenH), screenMat);
  const stand = new THREE.Group();
  stand.position.set(1.25, 1.3, 0.2);
  stand.rotation.y = -0.38;
  stand.add(screen);
  const bezel = new THREE.Mesh(new THREE.BoxGeometry(screenW + 0.12, screenH + 0.12, 0.08),
    new THREE.MeshStandardNodeMaterial({ color: '#0e1524', roughness: 0.35, metalness: 0.9 }));
  bezel.position.z = -0.045;
  stand.add(bezel);
  const post = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.08, 1.3, 16), bezel.material);
  post.position.set(0, -screenH / 2 - 0.55, -0.1);
  stand.add(post);
  scene.add(stand);
  // The screen's glow on the floor and the bezel.
  const screenLight = new THREE.PointLight('#5fa8ff', 6, 4, 2);
  screenLight.position.set(1.1, 1.3, 1.0);
  scene.add(screenLight);
  ui.on('texture', (t) => {  // a new size: a new texture
    screenMat.map = uiTexture(t);
    screenMat.needsUpdate = true;
  });

  // The drone camera: the core from above, into a render target the
  // console shows (<HostTexture name="drone-cam">).
  const drone = new THREE.PerspectiveCamera(50, 16 / 10, 0.1, 30);
  const droneTarget = new THREE.RenderTarget(512, 320, { samples: 1 });
  droneTarget.texture.colorSpace = THREE.SRGBColorSpace;  // rgba8unorm-srgb: shown with its colors

  // The HUD: the 'Hud' surface over the frame, in an overlay pass (premultiplied).
  const overlay = new THREE.Scene();
  const overlayCam = new THREE.OrthographicCamera(0, innerWidth, innerHeight, 0, -1, 1);
  const hudMat = new THREE.MeshBasicNodeMaterial({ map: uiTexture(hud.texture), transparent: true, toneMapped: false,
    blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
    depthTest: false, depthWrite: false });
  const hudQuad = new THREE.Mesh(new THREE.PlaneGeometry(HUD.width, HUD.height), hudMat);
  overlay.add(hudQuad);
  const placeHud = () => hudQuad.position.set(20 + HUD.width / 2, innerHeight - 20 - HUD.height / 2, 0);
  placeHud();

  // ------------------------------------------------------------ UI <-> game
  let spin = 1, pulse = true;
  jsui.on('message', (name, data) => {
    if (name === 'core') {
      coreColor.set(data.color);
      coreMat.emissive.copy(coreColor);
      ringMat.emissive.copy(coreColor);
      key.color.copy(coreColor);
    } else if (name === 'spin') spin = data.speed;
    else if (name === 'pulse') pulse = data.on;
  });

  // Mouse on the screen: raycast, then the event at the hit's UV.
  const raycaster = new THREE.Raycaster();
  const ndc = new THREE.Vector2();
  let over = false;
  const hit = (e) => {
    ndc.set((e.clientX / innerWidth) * 2 - 1, -(e.clientY / innerHeight) * 2 + 1);
    raycaster.setFromCamera(ndc, camera);
    return raycaster.intersectObject(screen)[0];
  };
  const forward = (e) => {
    const h = hit(e);
    if (!h) {
      if (over) ui.pointerLeave();
      over = false;
      canvas.style.cursor = '';
      return;
    }
    over = true;
    ui.dispatchEvent(e, h.uv.x * ui.width, (1 - h.uv.y) * ui.height);
    canvas.style.cursor = ui.cursor;
  };
  for (const type of ['pointerdown', 'pointermove', 'pointerup']) canvas.addEventListener(type, forward);
  canvas.addEventListener('wheel', forward, { passive: true });
  canvas.addEventListener('pointerleave', () => { ui.pointerLeave(); over = false; });
  addEventListener('keydown', (e) => ui.dispatchEvent(e));
  addEventListener('keyup', (e) => ui.dispatchEvent(e));
  // The camera leans toward the mouse a little.
  const lean = new THREE.Vector2();
  addEventListener('pointermove', (e) => lean.set(e.clientX / innerWidth - 0.5, e.clientY / innerHeight - 0.5));

  addEventListener('resize', () => {
    renderer.setSize(innerWidth, innerHeight);
    camera.aspect = innerWidth / innerHeight;
    camera.updateProjectionMatrix();
    Object.assign(overlayCam, { right: innerWidth, top: innerHeight });
    overlayCam.updateProjectionMatrix();
    placeHud();
  });

  // ---------------------------------------------------------------- frame
  const clock = new THREE.Timer();
  let angle = 0, frames = 0, last = performance.now(), droneGPU = null;
  renderer.autoClear = false;
  renderer.setAnimationLoop(() => {
    clock.update();
    const dt = Math.min(clock.getDelta(), 0.1), t = clock.getElapsed();
    angle += dt * spin;
    core.rotation.set(angle * 0.6, angle, 0);
    ring.rotation.set(Math.PI / 2 + Math.sin(t * 0.7) * 0.25, 0, angle * 0.4);
    const p = pulse ? 1.4 + Math.sin(t * 3) * 0.6 : 1.4;
    coreMat.emissiveIntensity = p;
    key.intensity = 26 + p * 10;
    camera.position.x += (lean.x * 1.2 - camera.position.x) * 0.04;
    camera.position.y += (1.55 - lean.y * 0.6 - camera.position.y) * 0.04;
    camera.lookAt(lookAt);

    // The drone cam first: the console samples its texture this frame.
    drone.position.set(core.position.x + Math.cos(t * 0.4) * 2.4, 2.6, Math.sin(t * 0.4) * 2.4);
    drone.lookAt(core.position);
    renderer.setRenderTarget(droneTarget);
    renderer.clear();
    renderer.render(scene, drone);
    renderer.setRenderTarget(null);
    const gpuTex = renderer.backend.get(droneTarget.texture).texture;
    if (gpuTex) {
      if (gpuTex !== droneGPU) droneGPU = gpuTex;
      jsui.setTexture('drone-cam', gpuTex);  // new contents: the views showing it repaint
    }

    renderer.clear();
    renderer.render(scene, camera);
    renderer.clearDepth();
    renderer.render(overlay, overlayCam);

    frames++;
    const now = performance.now();
    if (now - last >= 500) {
      const info = renderer.info.render;
      jsui.setState('scene', { fps: (frames * 1000) / (now - last), drawCalls: info.drawCalls, triangles: info.triangles });
      frames = 0;
      last = now;
    }
  });
  document.getElementById('splash').classList.add('gone');
}

main().catch((e) => fail(`${e?.message ?? e}`));
