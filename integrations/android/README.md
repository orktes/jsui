# JSUI on Android

![JsuiDemo in the Android emulator](../../docs/images/android-emulator.png)

`JsuiDemo` is a minimal NDK host: a `NativeActivity` (no Java code) whose
native library renders a JSUI bundle with Skia Ganesh on **OpenGL ES 3**
through EGL, straight into the window surface (`jsui_render_gl_target`,
default framebuffer), falling back to the CPU rasteriser painting into the
window's buffer (`ANativeWindow_lock`). Frames are paced by `AChoreographer`.
It forwards touches (with the historical samples between frames and their
timestamps, for gesture velocities), hardware keys (Back is Escape; unused, it
leaves the app) and gamepads (W3C standard mapping, sticks, hats and
triggers), and publishes the system bar / display cutout insets as the safe
area. Landscape.

## Build and run

Needs the Android SDK (`build-tools`, `platforms;android-35`, `platform-tools`),
an NDK (r30 tested), CMake, Ninja, Node and a JDK (for `keytool`). Works on
macOS and Linux hosts.

```sh
RUN=1 scripts/build-android.sh     # Skia, UI bundle, APK, install + launch + screenshot (adb)
```

* `scripts/build-skia-android.sh` builds Skia (m157, Ganesh + GL, the Android
  font manager with bundled FreeType / expat, bundled ICU / HarfBuzz) with the
  NDK into `third_party/skia-android/out/android-<cpu>` (`CPUS=arm64` or `x64`).
* `scripts/build-android.sh` bundles `APP` (default: the Godot demo UI) with
  `jsui build`, builds `libjsuidemo.so` with the NDK's CMake toolchain, and
  packages `build-android/JsuiDemo.apk` with `aapt2` (the bundle under
  `assets/ui/`, read through `AAssetManager` as `asset:///ui/...`), `zipalign`
  (16 KB pages) and `apksigner` (debug key). `ABI=x86_64` builds for Intel
  emulators.

An emulator for it:

```sh
sdkmanager "system-images;android-35;google_apis;arm64-v8a" "emulator"
avdmanager create avd -n jsui_api35 -k "system-images;android-35;google_apis;arm64-v8a" -d pixel_7
emulator -avd jsui_api35 -gpu host &
```

## Live reload

```sh
npx jsui dev integrations/godot/demo/ui/app.jsx --host 0.0.0.0
adb shell am start -n com.orktes.jsui.demo/android.app.NativeActivity --es jsui_url http://10.0.2.2:5173/bundle.js
```

(`10.0.2.2` is the host machine from the emulator; use the computer's LAN
address on a device.) `--es jsui_renderer cpu` forces the CPU path.

## Embedding

Link `libjsui.a` (built by this CMake project with the NDK toolchain) and the
Skia libraries, then per frame on the thread that owns the EGL context:

```c
JsuiGpuContext* gpu = jsui_gpu_create_gl(get_proc, NULL);  // once, with the context current
...
jsui_tick(rt, now_ms);
if (jsui_needs_redraw(rt)) {
  JsuiGLTarget t = {0};
  t.width = w; t.height = h; t.stencil_bits = 8;    // the window surface's config
  jsui_render_gl_target(rt, gpu, &t);
  eglSwapBuffers(display, surface);
}
```

Not yet: the on-screen keyboard for `TextInput` (hardware keyboards work),
Vulkan (Graphite), audio (bring an `on_audio` handler, e.g. on AAudio /
Oboe), and an Android library package (AAR) for Gradle projects.
