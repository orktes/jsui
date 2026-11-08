# gpu-stack: Mesa Turnip (KGSL) + Zink for Ubuntu Touch on the Galaxy Tab A7

An alternative GPU route for regular Wayland apps (Firefox, Electron) on the SM-T500, next to
libhybris: Mesa's Turnip Vulkan driver talks to Qualcomm's Android KGSL driver (`/dev/kgsl-3d0`)
directly, and Zink provides GL/GLES on top. It installs to `~/gpu-stack` on the tablet and leaves
the read-only rootfs alone.

- `Dockerfile`, `build-mesa.sh`, `collect.sh`: build Mesa 26.2.6 (put `mesa-26.2.6.tar.xz` in
  `src/`) in ubuntu:24.04 arm64, plus the Khronos loader, vulkan-tools, mesa-demos and the tests.
  `-Dfreedreno-kmds=kgsl,msm`: kgsl alone drops libdrm and with it the dmabuf WSI.
- `patches/`: `MESA_ZINK_NODEVICE=1` lets zink run without a DRM GPU node: EGL surfaceless,
  wayland and device platforms (Chromium's ANGLE asks for EGL_PLATFORM_DEVICE_EXT), and GBM/DRM-fd
  screens pick the Turnip device. The second patch adds cached readback for the wl_shm WSI path
  (Turnip/KGSL has no cached+coherent memory type).
- GBM is built (`-Dgbm=enabled`): Chromium and Firefox allocate window buffers with GBM on
  `/dev/dri/renderD128` (the msm_drm display node), which the rootfs libgbm can't serve. This one
  allocates through zink/Turnip (LINEAR dmabufs).
- `env.sh`: sourced by `bin/gpu-env CMD`, which runs anything on this stack. Until the
  compositor speaks linux-dmabuf it adds `MESA_VK_WSI_DEBUG=sw`; `GPU_STACK_DMABUF=1` skips that.
- `bin/firefox-gpu`, `bin/claude-desktop-gpu`: test launchers with their own profiles under
  `~/gpu-stack/profiles`. Chromium's GPU sandbox blocks `/dev/kgsl-3d0`; see the findings.
- `bin/dmabuf-check.sh` (tablet) and `poll-dmabuf.sh` (Mac): end-to-end dmabuf check; they
  respect `~/caps/deploy.lock`.
- `bench/scroll.html`, `bin/ff-bench.sh gpu|sw`, `bin/cd-bench.sh gpu|sw`: scroll fps / CPU, GPU vs software.
- `tests/`: offscreen Vulkan/GLES checks, the Turnip modifier/export probe, and a Wayland EGL
  frame-cost probe (`wlgl`).

`deploy.sh` pushes `stage/` plus `bin/` to the tablet over adb.
