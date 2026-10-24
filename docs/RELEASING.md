# Releasing

A release is a `vX.Y.Z` tag. `.github/workflows/release.yml` then publishes
the npm packages and attaches the prebuilt native binaries to the GitHub
Release of the tag (see [Native binaries](#native-binaries)).

## The npm packages

Three packages are published under the `@orktes` npm scope, always with the
same version:

| Package       | Directory        | Contents                                         |
| ------------- | ---------------- | ------------------------------------------------ |
| `@orktes/jsui-react` | `packages/react` | React Native API on the JSUI runtime             |
| `@orktes/jsui-cli`   | `packages/cli`   | `jsui build` / `jsui dev` (esbuild)              |
| `@orktes/jsui-web`   | `web`            | browser host + the three WASM modules            |

`.github/workflows/release.yml` publishes them with npm trusted publishing
(GitHub OIDC, with provenance; there is no npm token in the repository). It
does not compile anything: the WASM modules come from the CI run on `main`
for the released commit (artifacts `jsui-wasm`, `jsui-wasm-webgl`,
`jsui-wasm-gpu`), the same ones the Pages deploy uses, so that run must have
succeeded with all three, including the advisory WebGPU job.

## Cutting a release

1. Bump the version in lockstep: `node scripts/set-version.mjs 0.2.0`
   (updates the three `package.json`s and `@orktes/jsui-cli`'s dependency on
   `@orktes/jsui-react`). Merge that to `main` and wait for CI to pass.
2. Tag the merge commit and push the tag: `git tag v0.2.0 && git push origin v0.2.0`.
   (Or run the Release workflow by hand from the Actions tab on `main`.)
3. The workflow checks the tag matches the package versions, downloads the
   WASM modules, runs the JS tests, and publishes `@orktes/jsui-react`, then
   `@orktes/jsui-web`, then `@orktes/jsui-cli` (which depends on both). Versions already on npm are skipped, so a
   failed run can simply be re-run.
4. In parallel it creates the GitHub Release `vX.Y.Z` (notes from
   `docs/releases/X.Y.Z.md` when it exists, followed by the list generated
   from the merged PRs; prerelease for `-rc` versions) and uploads the native
   binaries. Re-running replaces them.

Prerelease versions (`0.2.0-rc.1`) are published under the `next` dist-tag;
everything else goes to `latest`.

## Follow-up releases (the quick path)

Releases are cut whenever something meaningful lands (a feature, a
noticeable perf win, a user-visible fix); there is no schedule.

1. Pick the version: patch (`0.5.1`) for fixes only, minor (`0.6.0`) for new
   features or behaviour changes (while we are on 0.x, breaking changes are
   minor bumps too; call them out in the notes).
2. On a branch from `main`:
   `node scripts/set-version.mjs 0.5.1 && npm install`, and write the notes
   in `docs/releases/0.5.1.md` (a short summary, then Highlights /
   Performance / Fixes as needed; `git log --format=%s v0.5.0..origin/main`
   lists what merged). The workflow puts this file above GitHub's generated
   list of merged PRs; without it the release gets the generated list alone.
3. Merge the PR and wait for the CI run of the merge commit on `main` to
   succeed (the release takes that run's WASM and native artifacts, so a red
   or cancelled run on that exact commit blocks the release; a later push to
   `main` cancels it).
4. `git fetch origin main && git tag v0.5.1 origin/main && git push origin v0.5.1`
   (tag the merge commit itself, not a later one whose CI has not finished).
5. Check: the Release workflow run is green;
   `npm view @orktes/jsui-react version` (and `-cli`, `-web`) shows the new
   version; the GitHub Release has the archives listed under
   [Native binaries](#native-binaries) plus `SHA256SUMS.txt`; the `upm`
   branch has a `com.orktes.jsui X.Y.Z` commit and the tag `upm/vX.Y.Z`.

If the workflow fails, fix the cause on `main` and either re-run the failed
jobs (when the fix is outside the tagged commit, e.g. npm settings) or cut
the next patch version (when the tagged commit itself is wrong). Do not move
a published tag.

## Trusted publisher setup (once per package)

npm only lets a package that already exists trust a workflow, so the first
version of each package was published by hand (`npm publish --workspace …`
after downloading the CI artifacts into `web/dist*`). Then, for each of the
three packages on npmjs.com: Settings → Trusted publishing → GitHub Actions,
with organization/user `orktes`, repository `jsui`, workflow filename
`release.yml`, no environment. After that, Settings → Publishing access →
"Require two-factor authentication and disallow tokens" blocks any other way
of publishing.

## Native binaries

The CI run on `main` builds them in the `dist-*` jobs of
`.github/workflows/ci.yml` (on pull requests only when packaging or build
files change), and the release takes that run's artifacts, so it ships exactly
what CI tested:

| Release asset | Contents | Built by |
| --- | --- | --- |
| `jsui-X.Y.Z-linux-x64.tar.gz` | `include/jsui/jsui.h`, `lib/libjsui.so`, `lib/cmake/jsui/` (`find_package(jsui)`), `bin/jsui-viewer` | `dist-linux` (`ubuntu-24.04`), Skia m157 |
| `jsui-X.Y.Z-linux-arm64.tar.gz` | same | `dist-linux` (`ubuntu-24.04-arm`), Skia m157 |
| `jsui-X.Y.Z-macos-universal.tar.gz` | same with `lib/libjsui.dylib` (arm64 + x86_64, macOS 13+) | `dist-macos`, Skia m157 built for both CPUs, merged with `lipo` |
| `jsui-X.Y.Z-windows-x64.zip` | same with `bin/jsui.dll`, `bin/icudtl.dat`, `lib/jsui.lib` | `dist-windows` (ClangCL, static CRT), Skia m157 |
| `jsui-X.Y.Z-ios-xcframework.zip` | `JSUI.xcframework`: static library (JSUI + QuickJS + Yoga + Skia) + header, device and simulator arm64 | `dist-ios` (advisory: left out when it failed) |
| `jsui-godot-X.Y.Z.zip` | `addons/jsui/`: `jsui.gdextension` + the GDExtension library of every platform above | all `dist-*` desktop jobs |
| `com.orktes.jsui-X.Y.Z.tgz` | the Unity package (`integrations/unity/package`) with `Plugins/macOS/jsui_unity.bundle`, `Plugins/Linux/x86_64/libjsui_unity.so` and `Plugins/Windows/x86_64/jsui_unity.dll` (+ `icudtl.dat`) and their `.meta` import settings | `dist-macos`, `dist-linux`, `dist-windows` |
| `jsui-unreal-X.Y.Z.zip` | `Jsui/`: the Unreal plugin (`integrations/unreal/Plugins/Jsui`) with the shared library of macOS, Win64 and Linux x86_64 in `Source/ThirdParty/JsuiLibrary` | `dist-macos`, `dist-windows`, `dist-linux` |
| `SHA256SUMS.txt` | checksums of the above | |

The shared library is `jsui_shared` (`-DJSUI_BUILD_SHARED=ON`): the static
library linked whole into a shared one that exports the `jsui_*` C API and
nothing else (an exported-symbols list on macOS, a version script plus static
libstdc++ / libgcc on Linux, `dllexport` on Windows, where hosts define
`JSUI_SHARED`). `scripts/dist/stage-native.sh` collects each platform's
binaries, `scripts/dist/check-shared.sh` builds `tests/shared/consumer.c`
against the staged package with `find_package(jsui)`, runs it and checks the
exports, and `scripts/dist/assemble-release.sh` makes the archives (the
`dist-assemble` CI job runs it on every build and uploads the result as
`jsui-release-preview`).

CI artifacts for other packaging (the Unity and Unreal plugins): per platform
`jsui-native-<platform>` (`shared/`, `viewer/`, `godot/`, `unity/`),
`jsui-shared-<platform>` (the shared library package alone) and
`jsui-unity-<platform>` (the Unity plugin: `jsui_unity.bundle` on macOS,
`libjsui_unity.so` on Linux, `jsui_unity.dll` + `icudtl.dat` on Windows). Platforms:
`linux-x64`, `linux-arm64`, `macos-universal`, `windows-x64`.

`scripts/dist/assemble-engines.sh` makes the Unity and Unreal archives (run
before `assemble-release.sh`, whose `SHA256SUMS.txt` covers them). The
version comes from the tag: the package's `package.json` and the plugin's
`.uplugin` in the repository stay at their development version. The `.meta`
GUIDs it writes for the native plugins are derived from their paths, so they
stay stable across releases. For a non-prerelease tag the release workflow
also commits the Unity package tree to the orphan branch `upm` and tags it
`upm/vX.Y.Z`, which is what the git URL install
(`https://github.com/orktes/jsui.git#upm`) fetches. Listing on OpenUPM or Fab
is not set up.

Not done yet: notarization of the macOS binaries (they are ad-hoc signed),
Graphite on Windows (needs a source-built Skia there), Android.

### Profile-guided QuickJS

The Linux and macOS `dist-*` jobs compile QuickJS with profile-guided
optimization (6-9% less JS time on React-heavy scenarios; see "QuickJS PGO"
in docs/ARCHITECTURE.md). Their configure step is
`scripts/pgo-quickjs.sh BUILD_DIR <the usual cmake args>`, which in the same
build directory configures an instrumented QuickJS
(`-DJSUI_QUICKJS_PGO=generate`), builds jsui-bench and jsui-render, runs the
training set, merges the profile and reconfigures with
`-DJSUI_QUICKJS_PGO=use -DJSUI_QUICKJS_PROFILE=BUILD_DIR/pgo/quickjs.profdata`.
The job's normal build step then recompiles QuickJS with the profile and
reuses every other object. The profile is made fresh in each job with the
job's own compiler; nothing is checked in or cached across compiler
versions.

| Platform | PGO | Profile |
| --- | --- | --- |
| linux-x64, linux-arm64 | yes (clang; `llvm-profdata` from the `llvm` package, the profile runtime from `libclang-rt-dev`, which Ubuntu does not install with clang) | trained on the runner's own CPU |
| macos-universal | yes (Apple clang, `xcrun llvm-profdata`) | trained in the arm64 build; the x86_64 slice reuses it (Clang's instrumentation profiles are keyed by source function, so they carry over between targets of the same compiler; the few functions that differ are simply not optimized with it) |
| windows-x64 | no | ClangCL would need the profile runtime and `llvm-profdata` from the VS LLVM; not set up |
| ios-xcframework | no | could reuse the macOS arm64 profile (same Xcode clang), but the advisory job runs in parallel with `dist-macos`; passing `-DJSUI_QUICKJS_PGO=use -DJSUI_QUICKJS_PROFILE=...` to its CMake is all it takes |
| WASM, Android | no | other compilers (emscripten's / the NDK's clang), so the profile format may not match; the configure check would turn that into a warning |

It never fails a release: if the instrumented build, the training, the
merge or the profile itself does not work (another LLVM's format, a missing
`llvm-profdata`), the job prints a `pgo-quickjs` warning and builds QuickJS
without PGO; look for `QuickJS PGO: optimizing with` in the configure log
to confirm a build used it. The stage costs the instrumented QuickJS and
two tool links (the rest of the objects are shared with the real build),
an `npm ci` for the training bundles and about a minute of training.

To build the same locally: `npm ci`, then
`scripts/pgo-quickjs.sh build -G Ninja -DCMAKE_BUILD_TYPE=Release` and
`cmake --build build`. With GCC the profile is the directory of `.gcda`
files the training wrote (`build/pgo/raw`); the script handles both.
