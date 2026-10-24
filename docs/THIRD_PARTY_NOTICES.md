# Third-party software in JSUI binaries

The prebuilt JSUI libraries, the viewer and the Godot and Unity plugins
contain the following open-source software. Each is used under the license
named here; the full license texts are in the projects' repositories (linked).

| Component | Used for | License |
| --- | --- | --- |
| [Skia](https://skia.org) (m157; m124 on Windows) | 2D rendering | BSD-3-Clause |
| [QuickJS-ng](https://github.com/quickjs-ng/quickjs) | JavaScript engine | MIT |
| [react-native-quickjs](https://github.com/react-native-quickjs/quickjs) | QuickJS debugger patches and Chrome DevTools Protocol agent (builds with `JSUI_DEBUGGER`) | MIT |
| [Yoga](https://github.com/facebook/yoga) | flexbox layout | MIT |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | text shaping (in Skia) | MIT ("Old MIT") |
| [ICU](https://github.com/unicode-org/icu) | Unicode, line breaking (in Skia) | Unicode License v3 |
| [FreeType](https://freetype.org) | font rasterization (in Skia, Linux / Windows) | FreeType License (FTL) |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG images (in Skia) | PNG Reference Library License v2 |
| [libjpeg-turbo](https://libjpeg-turbo.org) | JPEG images (in Skia) | IJG License, BSD-3-Clause |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | WebP images (in Skia) | BSD-3-Clause |
| [Wuffs](https://github.com/google/wuffs) | GIF images (in Skia) | Apache-2.0 |
| [Expat](https://libexpat.github.io) | XML (in Skia) | MIT |
| [zlib](https://zlib.net) | compression | zlib License |
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Vulkan memory (in Skia, Linux) | MIT |
| [Dawn](https://dawn.googlesource.com/dawn) headers | WebGPU interface (in Skia) | BSD-3-Clause |
| [miniaudio](https://miniaud.io) (incl. stb_vorbis) | built-in audio playback | MIT-0 / public domain |
| [Inter](https://rsms.me/inter/) | embedded default font | SIL Open Font License 1.1 |
| [SDL2](https://libsdl.org) | jsui-viewer only | zlib License |
| [godot-cpp](https://github.com/godotengine/godot-cpp) | Godot plugin only | MIT |

Portions of this software are copyright © The FreeType Project
(https://freetype.org). All rights reserved.

On Linux, the GNU C++ runtime (libstdc++, libgcc) is linked statically into
the shared library under the GCC Runtime Library Exception.
