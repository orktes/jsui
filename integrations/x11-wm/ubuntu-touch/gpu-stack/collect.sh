#!/bin/sh
# Runs inside ut-mesa-build: builds the tests and collects the Khronos loader + tools
# into /work/stage/home/phablet/gpu-stack.
set -e
mkdir -p /work/stage/home/phablet/gpu-stack/share/glvnd/egl_vendor.d
S=/work/stage/home/phablet/gpu-stack
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mesa-utils-bin >/dev/null
mkdir -p $S/bin $S/lib
cc -O2 -o $S/bin/vktest /work/tests/vktest.c -lvulkan
cc -O2 -o $S/bin/gltest /work/tests/gltest.c -lEGL -lGLESv2
cc -O2 -I$S/include -I/usr/include/libdrm -o $S/bin/gbmtest /work/tests/gbmtest.c -L$S/lib -lgbm
cc -O2 -I/usr/include/libdrm -o $S/bin/vkdmabuf /work/tests/vkdmabuf.c -lvulkan
cc -O2 -o $S/bin/wlglobals /work/tests/wlglobals.c -lwayland-client
(cd /tmp && X=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml && wayland-scanner client-header $X xdg-shell-client-protocol.h && wayland-scanner private-code $X xdg-shell.c && cc -O2 -I. -o $S/bin/wlgl /work/tests/wlgl.c xdg-shell.c -lwayland-client -lwayland-egl -lEGL -lGLESv2)
cp -L /usr/lib/aarch64-linux-gnu/libvulkan.so.1 $S/lib/
cp /usr/bin/vulkaninfo /usr/bin/vkcube /usr/bin/vkcube-wayland $S/bin/ 2>/dev/null || cp /usr/bin/vulkaninfo /usr/bin/vkcube $S/bin/
for b in eglinfo es2_info es2gears_wayland eglgears_wayland egltri_wayland; do cp /usr/bin/$b.aarch64-linux-gnu $S/bin/$b; done
rm -f $S/bin/bsd* $S/bin/xmllint
cat > $S/share/glvnd/egl_vendor.d/50_mesa.json <<J
{ "file_format_version" : "1.0.0", "ICD" : { "library_path" : "/home/phablet/gpu-stack/lib/libEGL_mesa.so.0" } }
J
ls -la $S/bin
