#!/bin/sh
# Runs inside ut-mesa-build (Ubuntu 24.04 arm64). /work = ~/Development/ut-tab/gpu-stack.
set -e
V=26.2.6
mkdir -p /build && cd /build
rm -rf mesa-$V; tar xf /work/src/mesa-$V.tar.xz
cd mesa-$V && python3 /work/patches/apply-zink-nodevice.py . && python3 /work/patches/apply-wsi-sw-cached.py .
rm -rf build.fail; [ -f build/build.ninja ] || { rm -rf build; meson setup build \
  --prefix=/home/phablet/gpu-stack --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms=wayland -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dglvnd=enabled \
  -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dgallium-drivers=zink -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm \
  -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled \
  -Dvulkan-layers= -Dtools= -Dbuild-tests=false; }
nice -n 19 ninja -C build -j12
rm -rf /work/stage && DESTDIR=/work/stage ninja -C build install
