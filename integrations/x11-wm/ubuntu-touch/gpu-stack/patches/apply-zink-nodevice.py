# Zink on Turnip/KGSL has no DRM device. Mesa only reaches its deviceless (kopper)
# path when LIBGL_ALWAYS_SOFTWARE is set, which also makes zink pick a CPU device.
# MESA_ZINK_NODEVICE=1 takes the deviceless path with the real GPU.
import sys, re
root = sys.argv[1] + "/src/egl/drivers/dri2/"
def sub(f, old, new):
    p = root + f; s = open(p).read()
    if new in s: return
    assert old in s, (f, old); open(p, "w").write(s.replace(old, new, 1))
sub("platform_surfaceless.c",
    "   if (!driver_loaded && disp->Options.ForceSoftware) {",
    "   if (!driver_loaded && (disp->Options.ForceSoftware ||\n"
    "                          (disp->Options.Zink && os_get_option(\"MESA_ZINK_NODEVICE\")))) {")
sub("platform_wayland.c",
    "   if (disp->Options.ForceSoftware)\n      return dri2_initialize_wayland_swrast(disp);",
    "   if (disp->Options.ForceSoftware ||\n       (disp->Options.Zink && os_get_option(\"MESA_ZINK_NODEVICE\")))\n      return dri2_initialize_wayland_swrast(disp);")
sub("platform_wayland.c",
    "   if (!dri2_setup_device(disp, disp->Options.ForceSoftware)) {",
    "   if (!dri2_setup_device(disp, disp->Options.ForceSoftware ||\n                                   dri2_dpy->fd_render_gpu < 0)) {")

# EGL device platform (ANGLE's GL back end): with MESA_ZINK_NODEVICE treat every EGLDevice,
# including DRM ones we cannot drive (msm_drm display node), as the deviceless zink device.
sub("platform_device.c",
    "   if (!request_software && _eglDeviceSupports(disp->Device, _EGL_DEVICE_DRM)) {",
    "   if (disp->Options.Zink && os_get_option(\"MESA_ZINK_NODEVICE\")) {\n"
    "      if (!device_probe_device_sw(disp))\n"
    "         goto cleanup;\n"
    "   } else if (!request_software && _eglDeviceSupports(disp->Device, _EGL_DEVICE_DRM)) {")
# MESA_ZINK_NODEVICE_DEBUG=1 logs which platform each eglGetPlatformDisplay asks for.
p2 = sys.argv[1] + "/src/egl/main/eglapi.c"; s2 = open(p2).read()
old2 = "_eglGetPlatformDisplayCommon(EGLenum platform, void *native_display,\n"
if "MESA_ZINK_NODEVICE_DEBUG" not in s2:
    i = s2.index(old2); j = s2.index("{", i) + 1
    s2 = s2[:j] + "\n   if (os_get_option(\"MESA_ZINK_NODEVICE_DEBUG\"))\n      fprintf(stderr, \"MESA-EGL: eglGetPlatformDisplay platform 0x%x native %p\\n\", platform, native_display);\n" + s2[j:]
    open(p2, "w").write(s2)

# GBM/DRM-fd screens (Chromium allocates window buffers with GBM on the msm_drm display node):
# Turnip on KGSL reports no DRM major/minor, so no device ever matches the fd. With
# MESA_ZINK_NODEVICE take the first device that has no DRM node instead.
pz = sys.argv[1] + "/src/gallium/drivers/zink/zink_screen.c"; sz = open(pz).read()
oldz = """      if (drm_props.renderMajor == dev_major &&
          drm_props.renderMinor == dev_minor)
         return i;
   }

   return -1;"""
newz = """      if (drm_props.renderMajor == dev_major &&
          drm_props.renderMinor == dev_minor)
         return i;
   }

   if (os_get_option("MESA_ZINK_NODEVICE")) {
      for (uint32_t i = 0; i < pdev_count; ++i) {
         VKSCR(GetPhysicalDeviceProperties2)(pdevs[i], &props);
         if (!drm_props.renderMajor && !drm_props.primaryMajor)
            return i;
      }
   }

   return -1;"""
if newz not in sz:
    assert oldz in sz; open(pz, "w").write(sz.replace(oldz, newz, 1))
for f in ("platform_surfaceless.c", "platform_wayland.c", "platform_device.c"):
    p = root + f; s = open(p).read()
    if "util/u_debug.h" not in s and "os_get_option" in s and '#include "util/os_misc.h"' not in s:
        s = s.replace('#include "egl_dri2.h"', '#include "egl_dri2.h"\n#include "util/os_misc.h"', 1); open(p, "w").write(s)
print("patched")
