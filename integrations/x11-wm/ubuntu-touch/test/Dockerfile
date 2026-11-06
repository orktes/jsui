# jsui-wl --mir without a tablet: builds jsui-wl (amd64) against UBports'
# Mir 1.8 and runs it against mir-test-server, a Mir server on Mir's stub
# platforms standing in for Ubuntu Touch's lomiri-system-compositor. From
# the repository root:
#
#   docker build -f integrations/x11-wm/ubuntu-touch/test/Dockerfile -t jsui-wl-mir .
#   docker run --rm -v "$PWD/shots:/out" jsui-wl-mir
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
 && echo "deb [trusted=yes] http://repo2.ubports.com/ 24.04-2.x main" > /etc/apt/sources.list.d/ubports.list \
 && apt-get update && apt-get install -y --no-install-recommends \
      git unzip python3 cmake ninja-build clang lld binutils pkg-config \
      libfontconfig1-dev zlib1g-dev libwlroots-dev wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev \
      libmir1client-dev libmir1al-dev libmir1server-dev mir1test-dev mir1-test-tools \
      foot fonts-dejavu-core imagemagick \
 && rm -rf /var/lib/apt/lists/*
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
COPY --from=node:22-bookworm-slim /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx
WORKDIR /src
COPY . .
RUN npm install --no-audit --no-fund --workspace packages/cli --workspace packages/react \
 && mkdir -p /opt/jsui-wm \
 && npx jsui build integrations/x11-wm/ui/shell.jsx -o /opt/jsui-wm/shell.js --format source --minify
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DJSUI_BUILD_TOOLS=OFF -DJSUI_BUILD_TESTS=OFF -DJSUI_BUILD_VIEWER=OFF \
      -DJSUI_AUDIO=OFF -DJSUI_DEBUGGER=OFF -DJSUI_BUILD_WAYLAND_WM=ON \
 && cmake --build /build --target jsui-wl && cp /build/jsui-wl /opt/jsui-wm/ \
 && (cp /build/icudtl.dat /opt/jsui-wm/ 2>/dev/null || true)
RUN clang++ -std=c++17 -O1 integrations/x11-wm/ubuntu-touch/test/mir-test-server.cpp -o /opt/jsui-wm/mir-test-server \
      -I/usr/include/mir1test $(pkg-config --cflags --libs mir1al mir1server mir1platform mir1test) -lmir1-test-assist
COPY integrations/x11-wm/docker/foot.ini /etc/xdg/foot/foot.ini
ENTRYPOINT ["sh", "/src/integrations/x11-wm/ubuntu-touch/test/test-mir.sh"]
