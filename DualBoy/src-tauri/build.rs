use std::env;
use std::path::PathBuf;

fn main() {
    // 1. Build libmgba using the cmake crate
    let mgba_path = "../..";
    
    let dst = cmake::Config::new(mgba_path)
        .define("LIBMGBA_ONLY", "ON")
        .define("BUILD_STATIC", "ON")
        .define("BUILD_SHARED", "OFF")
        .define("DISABLE_DEPS", "ON")
        .define("ENABLE_VFS", "ON")
        .define("M_CORE_GBA", "ON")
        .define("M_CORE_GB", "OFF")
        .build();

    // 2. Inform cargo about the link path
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=mgba");

    // 3. Generate bindings using bindgen
    let bindings = bindgen::Builder::default()
        .header("mgba_bindings.h")
        .clang_arg(format!("-I{}/include", mgba_path))
        .clang_arg(format!("-I{}/src", mgba_path))
        .clang_arg(format!("-I{}/include", dst.display()))
        .clang_arg("-DENABLE_VFS")
        .blocklist_item("FP_NAN")
        .blocklist_item("FP_INFINITE")
        .blocklist_item("FP_ZERO")
        .blocklist_item("FP_SUBNORMAL")
        .blocklist_item("FP_NORMAL")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");

    // 4. Build Tauri
    tauri_build::build()
}
