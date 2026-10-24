// Links libjsui (the shared C API library: libjsui.so / libjsui.dylib /
// jsui.dll + jsui.lib). Where it is found, first match wins:
//   JSUI_LIB_DIR        a directory holding the library, e.g. the `lib/` of a
//                       release archive (jsui-shared-<platform>.tar.gz)
//   <repo>/build        `cmake -B build -DJSUI_BUILD_SHARED=ON` in this repo
// The directory also goes into the rpath of this crate's examples and tests,
// so they run without LD_LIBRARY_PATH / DYLD_LIBRARY_PATH. Programs that
// depend on the crate add their own rpath (or ship the library next to the
// executable).
use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=JSUI_LIB_DIR");
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let dir = match env::var_os("JSUI_LIB_DIR") {
        Some(d) => PathBuf::from(d),
        None => manifest.join("../../build"),
    };
    let dir = dir.canonicalize().unwrap_or(dir);
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=jsui");
    let target = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target != "windows" {
        println!("cargo:rustc-link-arg-examples=-Wl,-rpath,{}", dir.display());
        println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{}", dir.display());
    }
    // For the example's "Rust 1.xx" label.
    let rustc = env::var("RUSTC").unwrap_or_else(|_| "rustc".into());
    let version = std::process::Command::new(rustc)
        .arg("--version")
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .and_then(|s| s.split_whitespace().nth(1).map(str::to_owned))
        .unwrap_or_default();
    println!("cargo:rustc-env=JSUI_RUSTC_VERSION={version}");
    // Dependents can read it as DEP_JSUI_LIB_DIR.
    println!("cargo:lib_dir={}", dir.display());
}
