# jsui-wl for Ubuntu Touch 24.04 (arm64): builds the compositor against
# UBports' Mir 1.8 client library and packs it with the libraries the
# Ubuntu Touch image lacks (wlroots 0.17 and its few extra dependencies) and
# foot, a terminal. Run on an arm64 machine (or with qemu binfmt) from the
# repository root:
#
#   docker build --platform linux/arm64 -f integrations/x11-wm/ubuntu-touch/Dockerfile \
#     --output type=local,dest=out/ .
#
# out/jsui-wm-ubuntu-touch.tar.gz is what install.sh unpacks on the tablet.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && echo "deb [trusted=yes] http://repo2.ubports.com/ 24.04-2.x main" > /etc/apt/sources.list.d/ubports.list \
 && apt-get update && apt-get install -y --no-install-recommends \
      git unzip python3 cmake ninja-build clang lld binutils pkg-config \
      libfontconfig1-dev zlib1g-dev libwlroots-dev wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev \
      libmir1client-dev libegl-dev libgles-dev \
 && rm -rf /var/lib/apt/lists/*
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
COPY --from=node:22-bookworm-slim /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx
WORKDIR /src
# aseprite's prebuilt Skia has no Linux arm64 package: build Skia from source
# (m157, scripts/build-skia-desktop.sh) in its own layer so it caches.
COPY scripts/build-skia-desktop.sh scripts/
COPY scripts/patches/skia-m157-dawn-header-only.patch scripts/patches/
RUN SKIA_SRC=/tmp/skia-src SKIA_DESKTOP_OUT=/opt/skia-desktop scripts/build-skia-desktop.sh \
 && rm -rf /tmp/skia-src
COPY . .
RUN npm install --no-audit --no-fund --workspace packages/cli --workspace packages/react
ENV PKG=/pkg/jsui-wm
RUN mkdir -p $PKG/lib \
 && npx jsui build integrations/x11-wm/ui/shell.jsx -o $PKG/shell.js --format source --minify
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DJSUI_BUILD_TOOLS=OFF -DJSUI_BUILD_TESTS=OFF -DJSUI_BUILD_VIEWER=OFF \
      -DJSUI_AUDIO=OFF -DJSUI_DEBUGGER=OFF -DJSUI_BUILD_WAYLAND_WM=ON \
      -DJSUI_SKIA_DIR=/opt/skia-desktop \
 && cmake --build /build --target jsui-wl \
 && cp /build/jsui-wl $PKG/ && (cp /build/icudtl.dat $PKG/ 2>/dev/null || true) && strip $PKG/jsui-wl
# Libraries and foot from the Ubuntu 24.04 archive that the Ubuntu Touch
# image doesn't have (see install.sh): unpacked, not installed, so nothing
# on the tablet's read-only root changes.
RUN cd /tmp && apt-get update && apt-get download \
      libwlroots12t64 libdisplay-info1 libliftoff0 libseat1 libvulkan1 libxcb-ewmh2 \
      foot libfcft4t64 libutf8proc3 \
 && for d in *.deb; do dpkg-deb -x $d /tmp/root; done \
 && cp -a /tmp/root/usr/lib/aarch64-linux-gnu/*.so* $PKG/lib/ \
 && cp /tmp/root/usr/bin/foot $PKG/ \
 && cp -a /tmp/root/usr/share/foot $PKG/foot-share 2>/dev/null || true
COPY integrations/x11-wm/docker/foot.ini $PKG/foot.ini
# No foot terminfo on the tablet.
RUN sed -i '/^\[main\]/a term=xterm-256color' $PKG/foot.ini
COPY integrations/x11-wm/ubuntu-touch/jsui-session integrations/x11-wm/ubuntu-touch/install.sh \
     integrations/x11-wm/ubuntu-touch/uninstall.sh $PKG/
RUN cd /pkg && tar czf /jsui-wm-ubuntu-touch.tar.gz jsui-wm

FROM scratch
COPY --from=build /jsui-wm-ubuntu-touch.tar.gz /
