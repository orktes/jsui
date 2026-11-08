# gpu-stack: Mesa Turnip (KGSL) + Zink for Ubuntu Touch on the Galaxy Tab A7

A GPU route for regular Wayland apps (Firefox, Electron/Claude Desktop) on the SM-T500
(Adreno 610, Halium 12, read-only rootfs), next to libhybris. Mesa's Turnip Vulkan driver talks
to Qualcomm's Android KGSL driver (`/dev/kgsl-3d0`, world-writable on this image) directly, and
Zink provides GL/GLES and GBM on top. Everything installs to `~/gpu-stack` on the tablet; the
rootfs is not touched. Apps hand LINEAR dmabufs to jsui-wl through zwp_linux_dmabuf_v1
(jsui-wl PRs #328–#330).

## Results (2026-11-08, jsui-wl #329/#330, captures in `docs/tablet-gpu/`)

Firefox 159 auto-scrolling `bench/scroll.html` (`bin/ff-bench.sh gpu|sw`, 20 s, frame cap ~62):

| | fps | p50 | p95 | Firefox CPU | jsui-wl CPU |
|---|---|---|---|---|---|
| GPU: WebRender on Zink/Turnip, dmabuf | **60.3** | 16.1 ms | 21.9 ms | 159% | **42%** |
| SW: WebRender software (old wrapper) | 17.1 | 61.7 ms | 96.8 ms | 144% | 96% |

Other measurements:
- Claude Desktop renders on the GPU too (ANGLE-GL → Zink → Turnip).
- eglgears_wayland: 60.9 fps on dmabuf, versus 4.7 on the wl_shm fallback.
- vkcube: ~114 fps client side.
- Turnip itself: Vulkan 1.3 (with `TU_DEBUG=noconform`); Zink: GL 4.6 core/compat, GLES 3.2.

## Build and deploy

1. Download `mesa-26.2.6.tar.xz` and its `.sig` from archive.mesa3d.org into `src/`. Verify it
   (signing key 57551DE15B968F6341C248F68D8E31AFC32428A6).
2. `docker build --platform linux/arm64 -t ut-mesa-build .`
3. `docker volume create ut-mesa-build && docker run --rm -v $PWD:/work -v ut-mesa-build:/build ut-mesa-build /work/build-mesa.sh`
4. `docker run --rm -v $PWD:/work ut-mesa-build sh -c 'apt-get update -qq; /work/collect.sh'`
   adds the Khronos loader (the rootfs `libvulkan.so.1` is libhybris'), vulkan-tools,
   mesa-demos, the tests, and an absolute-path glvnd vendor file.
5. `./deploy.sh` pushes `stage/` plus `env.sh` and `bin/` to `~/gpu-stack` over adb.

Mesa options that matter: `-Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm
-Dgallium-drivers=zink -Dplatforms=wayland -Dglx=disabled -Dgbm=enabled -Dglvnd=enabled`.

## Local Mesa patches (`patches/`)

- `zink-nodevice.patch`: lets `MESA_ZINK_NODEVICE=1` run zink without a DRM GPU node.
  - EGL surfaceless, wayland and **device** platforms take the deviceless kopper path. Chromium's
    ANGLE asks for `EGL_PLATFORM_DEVICE_EXT`. Upstream reaches that path only with
    `LIBGL_ALWAYS_SOFTWARE`, which also makes zink pick a CPU device.
  - Zink picks Turnip for DRM fds it can't match. That's GBM on `/dev/dri/renderD128`, the msm_drm
    display node.
  - `MESA_ZINK_NODEVICE_DEBUG=1` logs the EGL platform apps ask for.
- `wsi-sw-cached.patch`: the wl_shm fallback WSI read frames back from uncached memory. Turnip on
  KGSL has coherent-uncached or cached-non-coherent memory, never both. The patch reads from
  cached memory and invalidates first, which took ~100 ms per frame down to a few ms. It only
  matters without dmabuf.
- `wsi-dmabuf-check.patch`: `MESA_WSI_DMABUF_CHECK=N` reads back every Nth presented dmabuf and
  logs its centre pixel and % non-zero. Use it to tell a client-side blank frame from a
  compositor-side one. Debug only.

## Running apps

- `env.sh`: source it or use `bin/gpu-env CMD`. It sets `LD_LIBRARY_PATH`, `VK_ICD_FILENAMES` /
  `VK_DRIVER_FILES`, `__EGL_VENDOR_LIBRARY_FILENAMES`, `MESA_LOADER_DRIVER_OVERRIDE=zink`,
  `MESA_ZINK_NODEVICE=1`, `TU_DEBUG=noconform` and `allow_rgb10_configs=false`.
- `gpu-env` checks the compositor for zwp_linux_dmabuf_v1. Without it, it falls back to wl_shm
  (`MESA_VK_WSI_DEBUG=sw`).
- Firefox: `MOZ_ENABLE_WAYLAND=1` with prefs `gfx.webrender.software=false`,
  `gfx.webrender.all=true`, `layers.acceleration.disabled=false`. `bin/firefox-gpu` uses its own
  profile.
- Claude Desktop: `--ozone-platform=wayland --use-gl=angle --use-angle=gl --ignore-gpu-blocklist
  --enable-gpu-rasterization --disable-gpu-sandbox`. `bin/claude-desktop-gpu` uses its own
  `--user-data-dir`; pass the sandbox flag through `CLAUDE_DESKTOP_FLAGS`. The GPU sandbox is
  turned off with the owner's approval; renderer sandboxes stay on.

## Gotchas

- `-Dfreedreno-kmds=kgsl` alone drops libdrm, so `wsi_common_drm.c` and GBM go with it. The
  Wayland WSI then sends fd -1 ("dup failed: Bad file descriptor") or SIGBUSes. Use `kgsl,msm`.
- A610 has no hardware multiview, so Turnip reports Vulkan 1.0 unless `TU_DEBUG=noconform`.
  ANGLE needs 1.1+.
- Zink sorts 10-bit EGL configs first, so clients pick XR30, which jsui-wl can't offer. Keep
  `allow_rgb10_configs=false`.
- Never set `MESA_VK_WSI_DEBUG=sw` when the compositor has dmabuf: every frame becomes a CPU copy
  through wl_shm.
- Chromium's GPU sandbox only opens `/dev/dri/*`, and Turnip needs `/dev/kgsl-3d0` and `/dev/ion`.
  No narrower switch exists in this Electron.
- This Electron allows only ANGLE: `--use-gl=egl` gives "not found in allowed implementations",
  and ANGLE-Vulkan is refused on ozone-wayland.
- Chromium and Firefox allocate window buffers with GBM on renderD128. The rootfs libgbm (24.0.9)
  can't, which gives "Cannot create bo …" and then "Context was lost" loops. Our libgbm allocates
  through zink.
- After GPU-process crashes, Claude Desktop writes `isHardwareAccelerationDisabled` /
  `isHardwareAccelerationAutoDisabled: true` into `<profile>/claude_desktop_config.json` and
  starts with `--disable-gpu` from then on. Set them back to false.
- Tablet `/tmp` is a 1.4 GB RAM tmpfs: install to `~/gpu-stack` (eMMC) and capture into `~/caps`.

## Scripts (`bin/`, tablet side; run screen tests under `with-lock`)

- `with-lock CMD`: waits for and takes `~/caps/deploy.lock`, the shell thread's deploy lock.
- `ff-bench.sh gpu|sw`, `cd-bench.sh gpu|sw`: fps, CPU per process and thread, a mid-run screenshot.
- `content-check.sh`: dmabuf readback (`MESA_WSI_DMABUF_CHECK`) plus jsui-wl screenshots for
  vkcube, wlgl, Firefox and Claude.
- `dmabuf-check.sh` (with `poll-dmabuf.sh` on the Mac), `ff-trace.sh`, `cd-trace.sh`,
  `cd-verify.sh`: protocol, widget and GPU-process tracing.
- `tests/`:
  - offscreen Vulkan and GLES checks (`vktest`, `gltest`)
  - Turnip modifier and export probe (`vkdmabuf`)
  - GBM probe (`gbmtest`)
  - Wayland EGL frame-cost probe (`wlgl`) and globals lister (`wlglobals`)
