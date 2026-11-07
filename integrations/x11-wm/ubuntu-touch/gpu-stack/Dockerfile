FROM ubuntu:24.04
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential ninja-build pkg-config python3-pip python3-mako python3-yaml python3-packaging \
    bison flex glslang-tools libdrm-dev libwayland-dev wayland-protocols libwayland-egl-backend-dev \
    libexpat1-dev zlib1g-dev libzstd-dev libelf-dev libglvnd-dev libx11-dev libxext-dev libxrandr-dev \
    libxcb1-dev libxshmfence-dev libvulkan-dev vulkan-tools libvulkan1 xz-utils ca-certificates git cmake \
    libegl-dev libgles-dev libwayland-bin python3-setuptools patchelf && \
    pip3 install --break-system-packages 'meson==1.6.1'
