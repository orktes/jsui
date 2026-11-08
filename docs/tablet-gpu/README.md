# Tablet GPU route: captures

Galaxy Tab A7 (SM-T500, Adreno 610) under jsui-wl. Apps render with Mesa 26.2.6 from
`integrations/x11-wm/ubuntu-touch/gpu-stack` (Zink GL on Turnip Vulkan, KGSL) and hand
LINEAR dmabufs to jsui-wl (zwp_linux_dmabuf_v1, PRs #328-#330). The captures come from jsui-wl's
control socket (`shot`) at 2026-11-07 23:39Z.

- `firefox-gpu.png`: Firefox 159, WebRender on Zink, the scroll bench page (first-run dialog on top).
- `claude-desktop-gpu.png`: Claude Desktop 4.23118.0, ANGLE-GL on Zink, GPU process unsandboxed
  (approved), test profile.

The client side was checked too: Turnip's WSI (MESA_WSI_DMABUF_CHECK) read back the presented
2000x1130 LINEAR buffers, pitch 8192, ~97% non-zero, and they matched the page colours.
