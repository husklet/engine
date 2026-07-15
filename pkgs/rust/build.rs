use std::{env, path::PathBuf};

fn main() {
    let target = env::var("TARGET").expect("Cargo sets TARGET");
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("manifest directory"));
    let directory = match target.as_str() {
        "aarch64-apple-darwin" => "aarch64-apple-darwin",
        "aarch64-unknown-linux-gnu" => "aarch64-unknown-linux-gnu",
        _ => panic!("hl-engine does not support target {target}"),
    };
    println!(
        "cargo:rustc-link-search=native={}",
        root.join("assets/lib").join(directory).display()
    );
    println!("cargo:rustc-link-lib=static:+whole-archive=hl-engine");
    if target == "aarch64-unknown-linux-gnu" {
        for library in ["pthread", "dl", "m", "atomic"] {
            println!("cargo:rustc-link-lib={library}");
        }
    }
    println!("cargo:rerun-if-changed=assets/lib/{directory}/libhl-engine.a");
}
