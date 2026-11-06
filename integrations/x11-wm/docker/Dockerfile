# jsui-wm in a container: the JSUI touch shell on X11 (Xvfb + jsui-wm) or
# Wayland (headless wlroots + jsui-wl), 1920x1200 like an 11" tablet panel, a
# few X11 / GTK / Wayland apps, and VNC to look at it. Build from the
# repository root:
#
#   docker build -f integrations/x11-wm/docker/Dockerfile -t jsui-wm .
#   docker run --rm -p 5900:5900 jsui-wm                  # X11, VNC on localhost:5900
#   docker run --rm -p 5900:5900 -e WM=wayland jsui-wm    # Wayland, VNC (wayvnc)
#   docker run --rm -v "$PWD/shots:/out" jsui-wm tour     # scripted tour + screenshots
#
# ---------------------------------------------------------------- build
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git curl unzip python3 cmake ninja-build clang lld binutils \
      libfontconfig1-dev zlib1g-dev libx11-dev libxcomposite-dev libxdamage-dev \
      libxfixes-dev libxext-dev libxtst-dev libxi-dev \
      libwlroots-dev wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
# Node 22 for the bundler (`jsui build`).
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
COPY --from=node:22-bookworm-slim /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx

WORKDIR /src
COPY . .
RUN npm install --no-audit --no-fund --workspace packages/cli --workspace packages/react
RUN mkdir -p /opt/jsui-wm \
 && npx jsui build integrations/x11-wm/ui/shell.jsx -o /opt/jsui-wm/shell.js --minify
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DJSUI_BUILD_TOOLS=OFF -DJSUI_BUILD_TESTS=OFF -DJSUI_BUILD_VIEWER=OFF \
      -DJSUI_AUDIO=OFF -DJSUI_DEBUGGER=OFF -DJSUI_BUILD_X11WM=ON -DJSUI_BUILD_WAYLAND_WM=ON \
 && cmake --build /build --target jsui-wm jsui-wl \
 && cp /build/jsui-wm /build/jsui-wl /opt/jsui-wm/ \
 && (cp /build/icudtl.dat /opt/jsui-wm/ 2>/dev/null || true) \
 && strip /opt/jsui-wm/jsui-wm /opt/jsui-wm/jsui-wl

# -------------------------------------------------------------- runtime
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      xvfb x11-xserver-utils x11-utils x11-apps xterm xdotool x11vnc \
      mousepad galculator pcmanfm dbus-x11 \
      fonts-dejavu-core libfontconfig1 imagemagick python3 \
      libwlroots12t64 foot wayvnc \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/jsui-wm /opt/jsui-wm
COPY integrations/x11-wm/docker/applications/ /usr/local/share/applications/
COPY integrations/x11-wm/docker/Xresources /etc/jsui-wm/Xresources
COPY integrations/x11-wm/docker/foot.ini /etc/xdg/foot/foot.ini
COPY integrations/x11-wm/docker/tour.py integrations/x11-wm/docker/run.sh /opt/jsui-wm/
ENV NO_AT_BRIDGE=1
EXPOSE 5900
ENTRYPOINT ["/opt/jsui-wm/run.sh"]
CMD ["vnc"]
