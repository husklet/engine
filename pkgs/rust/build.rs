use std::{env, path::PathBuf};

/// Host triples this crate can link, and whether they need the GNU/Linux
/// libraries that the Mach-O link does not.
///
/// This is a table rather than a `match` with a `_ => panic!("unsupported
/// target")` arm because two different facts were conflated behind that panic:
/// whether the crate SUPPORTS a host, and whether a prebuilt archive for it
/// happens to be in the tree. Only the first belongs here. Exactly two of these
/// archives are committed and published (see the `include` list in Cargo.toml),
/// so `x86_64-unknown-linux-gnu` is supported and is expected to be BUILT
/// locally -- and the diagnostic below has to say so, instead of claiming the
/// host is unsupported.
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
    // Rerun BEFORE the existence check, so producing the archive re-runs this
    // script rather than leaving the failure cached.
    println!("cargo:rerun-if-changed=assets/lib/{directory}/libhl-engine.a");
    // A missing archive is not an unsupported target, and saying so was the whole
    // bug: the message has to name the command that produces the file. Only the
    // two aarch64 archives are committed -- ~24MB each against the 10MB
    // crates.io budget -- so on any other supported host the archive is a local
    // build product, and this is the first thing that host sees.
    //
    // REPORTED, not asserted, and deliberately so. These bytes are a LINK input,
    // and a lint is not a link. Panicking here failed `rust.clippy` -- a CTest
    // case in the `unit` lane -- on a host whose only defect was an unbuilt local
    // artifact: the same conflation of two facts as the
    // `_ => panic!("unsupported target")` arm this replaced.
    //
    // The link directives are SKIPPED with it, and they have to be: `rustc-link-lib
    // =static:...` makes rustc resolve the archive while producing the rlib, so
    // emitting it against a missing file fails `cargo clippy` as hard as a panic
    // did ("could not find native static library `hl-engine`"). Skipping leaves
    // lints and `cargo check` working and moves the failure to the link that
    // actually needs the bytes, where it lands as undefined references to
    // hl_engine_* directly beneath these warnings.
    if !archive.is_file() {
        // One println per line: cargo renders `cargo:warning=` as a single line.
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
