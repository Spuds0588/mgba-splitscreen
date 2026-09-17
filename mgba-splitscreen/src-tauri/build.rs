use std::env;
use std::path::{Path, PathBuf};

/// Extract the `-D` preprocessor defines the cmake build actually used, so bindgen
/// generates structs with the same layout as the compiled `libmgba`. Without this,
/// conditionally-compiled fields (e.g. `mCore.dirs` under `ENABLE_DIRECTORIES`, or the
/// `pthread_mutex_t` vs no-op `Mutex` under `USE_PTHREADS`) shift field offsets and
/// cause segfaults at runtime.
fn cmake_defines(dst: &Path) -> Vec<String> {
    let flags_dir = dst.join("build").join("CMakeFiles");
    let mut defines = Vec::new();
    if let Ok(entries) = std::fs::read_dir(&flags_dir) {
        for entry in entries.flatten() {
            let flags = entry.path().join("flags.make");
            if let Ok(contents) = std::fs::read_to_string(&flags) {
                for line in contents.lines() {
                    if let Some(defs) = line.strip_prefix("C_DEFINES = ") {
                        for token in defs.split_whitespace() {
                            if let Some(name) = token.strip_prefix("-D") {
                                defines.push(name.to_string());
                            }
                        }
                    }
                }
            }
        }
    }
    defines.sort();
    defines.dedup();
    defines
}

fn main() {
    // Android builds never link the native core. The Android app is a thin shell
    // around the same WebAssembly engine the web version ships (inside the system
    // WebView), so running cmake for aarch64 would cross-compile the whole
    // emulator through the NDK to produce a library nothing on that target calls.
    // Skipping it keeps an Android build a couple of minutes instead of an
    // hour-long toolchain fight. The `cfg(target_os = "android")` gates in
    // src/lib.rs are the other half of this pair: they drop the modules that would
    // otherwise expect bindings.rs and the linked core to exist.
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("android") {
        println!("cargo:warning=native mGBA core skipped on Android; the WASM engine is used");
        // tauri-build still has to run on every platform: it generates the
        // context (icons, manifest fragments, capabilities) the app needs.
        tauri_build::build();
        return;
    }

    // 1. Build libmgba using the cmake crate
    let mgba_path = "../..";

    let dst = cmake::Config::new(mgba_path)
        // Optimized emulation core (Release also defines NDEBUG, which trims debug-only
        // code paths). The Rust side is optimized separately via `cargo build --release`.
        .profile("Release")
        .define("LIBMGBA_ONLY", "ON")
        .define("BUILD_STATIC", "ON")
        .define("BUILD_SHARED", "OFF")
        .define("DISABLE_DEPS", "ON")
        .define("ENABLE_VFS", "ON")
        .define("M_CORE_GBA", "ON")
        .define("M_CORE_GB", "ON")
        .build();

    // 2. Inform cargo about the link path
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=mgba");

    // 3. Generate bindings using bindgen, with the same defines as the C build
    let mut builder = bindgen::Builder::default()
        .header("mgba_bindings.h")
        .clang_arg(format!("-I{}/include", mgba_path))
        .clang_arg(format!("-I{}/src", mgba_path))
        .clang_arg(format!("-I{}/include", dst.display()))
        .blocklist_item("FP_NAN")
        .blocklist_item("FP_INFINITE")
        .blocklist_item("FP_ZERO")
        .blocklist_item("FP_SUBNORMAL")
        .blocklist_item("FP_NORMAL")
        .blocklist_item("FP_INT_UPWARD")
        .blocklist_item("FP_INT_DOWNWARD")
        .blocklist_item("FP_INT_TOWARDZERO")
        .blocklist_item("FP_INT_TONEARESTFROMZERO")
        .blocklist_item("FP_INT_TONEAREST")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));

    for define in cmake_defines(&dst) {
        builder = builder.clang_arg(format!("-D{}", define));
    }

    let bindings = builder.generate().expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");

    // 4. Build Tauri
    tauri_build::build()
}
