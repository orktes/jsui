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
for f in ("platform_surfaceless.c", "platform_wayland.c"):
    p = root + f; s = open(p).read()
    if "util/u_debug.h" not in s and "os_get_option" in s and '#include "util/os_misc.h"' not in s:
        s = s.replace('#include "egl_dri2.h"', '#include "egl_dri2.h"\n#include "util/os_misc.h"', 1); open(p, "w").write(s)
print("patched")
