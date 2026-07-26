use std::{env, path::PathBuf};

/// Host triples this crate can link, and whether they need the GNU/Linux
/// libraries the Mach-O link does not. A table rather than a panicking `match`:
/// "host supported" and "archive present" are separate facts.
const HOSTS: &[(&str, bool)] = &[
    ("aarch64-apple-darwin", false),
    ("aarch64-unknown-linux-gnu", true),
    ("x86_64-unknown-linux-gnu", true),
];

fn main() {
    let target = env::var("TARGET").expect("Cargo sets TARGET");
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("manifest directory"));
    let Some(&(directory, gnu_linux)) = HOSTS.iter().find(|(triple, _)| *triple == target) else {
        let supported = HOSTS
            .iter()
            .map(|(triple, _)| *triple)
            .collect::<Vec<_>>()
            .join(", ");
        panic!("hl-engine does not support target {target}; supported hosts are {supported}");
    };
    let archive = root
        .join("assets/lib")
        .join(directory)
        .join("libhl-engine.a");
    // Before the existence check, so producing the archive re-runs this script.
    println!("cargo:rerun-if-changed=assets/lib/{directory}/libhl-engine.a");
    // A missing archive is not an unsupported target: warn, and name the command
    // that builds it -- panicking failed `rust.clippy` on hosts whose only defect
    // was an unbuilt local artifact. The link directives are skipped with it:
    // `rustc-link-lib=static:` makes rustc resolve the archive while producing
    // the rlib, so emitting them would fail clippy as hard as a panic.
    if !archive.is_file() {
        // cargo renders each `cargo:warning=` as one line.
        for line in [
            format!("hl-engine supports {target} but no prebuilt archive for it is in this tree."),
            format!("expected: {}", archive.display()),
            "lints and `cargo check` work without it; ANY LINK against this crate".to_owned(),
            "will fail with undefined hl_engine_* symbols until it exists. Build it".to_owned(),
            "from this checkout with:".to_owned(),
            "    cmake --build <build-dir> --target refresh-crate-archives-linux".to_owned(),
            "(equivalently: tools/refresh_crate_archives.sh --linux)".to_owned(),
            "Only aarch64-apple-darwin and aarch64-unknown-linux-gnu archives are".to_owned(),
            "committed and published; see pkgs/rust/assets/PROVENANCE.md.".to_owned(),
        ] {
            println!("cargo:warning={line}");
        }
        return;
    }
    println!(
        "cargo:rustc-link-search=native={}",
        archive.parent().expect("archive has a parent").display()
    );
    println!("cargo:rustc-link-lib=static:+whole-archive=hl-engine");
    if gnu_linux {
        for library in ["pthread", "dl", "m", "atomic"] {
            println!("cargo:rustc-link-lib={library}");
        }
    }
}
