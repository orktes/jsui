#!/bin/sh
# Runs inside ut-mesa-build (Dockerfile; Ubuntu 24.04 arm64). /work = this directory, with
# src/mesa-$V.tar.xz downloaded from archive.mesa3d.org (check its .sig). Installs to
# /work/stage/home/phablet/gpu-stack; deploy.sh pushes that to the tablet.
#   docker volume create ut-mesa-build
#   docker run --rm -v $PWD:/work -v ut-mesa-build:/build ut-mesa-build /work/build-mesa.sh
# (The source tree lives in a docker volume: tar on a macOS bind mount trips over renames.)
set -e
V=26.2.6
mkdir -p /build && cd /build
rm -rf mesa-$V; tar xf /work/src/mesa-$V.tar.xz
cd mesa-$V
for p in zink-nodevice wsi-sw-cached wsi-dmabuf-check; do patch -p1 -s < /work/patches/$p.patch; done
# kgsl,msm: kgsl alone drops libdrm, and with it the Wayland dmabuf WSI (wsi_common_drm.c) and GBM.
meson setup build \
  --prefix=/home/phablet/gpu-stack --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms=wayland -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dglvnd=enabled \
  -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dgallium-drivers=zink -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl,msm \
  -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled \
  -Dvulkan-layers= -Dtools= -Dbuild-tests=false
nice -n 19 ninja -C build -j12
rm -rf /work/stage && DESTDIR=/work/stage ninja -C build install
