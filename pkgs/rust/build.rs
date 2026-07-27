use std::{env, path::PathBuf};

/// One supported host triple.
///
/// A struct rather than the two-column table this used to be: the Windows row shares neither the
/// archive filename nor the system libraries with the Unix rows, and a second `bool` would not have
/// said which. "Host supported" and "archive present" are still separate facts, decided separately
/// below.
struct Host {
    /// The target triple, and the directory under `assets/lib/` its archive is filed in.
    triple: &'static str,
    /// The archive's filename. rustc's native-library search looks for `lib<name>.a` on Unix and
    /// `<name>.lib` on `*-windows-msvc`, so this is a property of the row rather than a constant.
    archive: &'static str,
    /// What the engine archive itself needs from the host, beyond what rustc links anyway.
    system_libraries: &'static [&'static str],
    /// The `refresh-crate-archives-*` half that produces this row's archive.
    refresh: &'static str,
}

const HOSTS: &[Host] = &[
    Host {
        triple: "aarch64-apple-darwin",
        archive: "libhl-engine.a",
        // The Mach-O link needs none of the GNU/Linux libraries.
        system_libraries: &[],
        refresh: "darwin",
    },
    Host {
        triple: "aarch64-unknown-linux-gnu",
        archive: "libhl-engine.a",
        system_libraries: &["pthread", "dl", "m", "atomic"],
        refresh: "linux",
    },
    Host {
        triple: "x86_64-unknown-linux-gnu",
        archive: "libhl-engine.a",
        system_libraries: &["pthread", "dl", "m", "atomic"],
        refresh: "linux",
    },
    Host {
        triple: "x86_64-pc-windows-msvc",
        archive: "hl-engine.lib",
        // Nothing from the Unix row applies -- `m` in particular has no Windows counterpart, since
        // the math functions live in the UCRT. These are import libraries, so they are emitted as
        // `dylib=`: asking rustc for `static=ntdll` would make it look for `libntdll.a`.
        //
        //   ntdll           the sub-Win32 surface: NtCreateFile, NtQueryInformationProcess,
        //                   NtMapViewOfSection, NtSuspendProcess
        //   kernel32        the bulk of Win32: CreateFileW, VirtualAlloc2, MapViewOfFile3,
        //                   QueryPerformanceCounter, CreateProcessW, job objects. rustc's own link
        //                   line already includes it; naming it is not depending on that
        //   ws2_32          sockets, for the guest ABI's socket surface and the provider channel
        //   synchronization WaitOnAddress / WakeByAddressSingle / WakeByAddressAll, the
        //                   futex-shaped primitive the host sync layer needs. An umbrella library
        //                   forwarding to api-ms-win-core-synchronization-l1-2-0.dll; Windows 8+
        //   advapi32        tokens and ACLs: OpenProcessToken, the DACL entry points this crate's
        //                   own private-file support uses, privilege adjustment
        //   bcrypt          BCryptGenRandom, for the secure host entropy the crate advertises
        //
        // `mincore` and `onecore` are deliberately absent. Both are umbrella import libraries for
        // OneCore-targeted binaries; linking one into a desktop static archive replaces the
        // individual libraries rather than supplementing them, and `mincore` in particular is
        // superseded. Add one only if a OneCore/Store target is actually wanted.
        //
        // This list is a prediction from what the host services layer covers and what the crate
        // advertises -- there is no Windows archive yet to read `nm` output from. Replace it with
        // the first archive's actual undefined symbols rather than growing it speculatively.
        system_libraries: &[
            "ntdll",
            "kernel32",
            "ws2_32",
            "synchronization",
            "advapi32",
            "bcrypt",
        ],
        refresh: "windows",
    },
];

fn main() {
    let target = env::var("TARGET").expect("Cargo sets TARGET");
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("manifest directory"));
    let Some(host) = HOSTS.iter().find(|host| host.triple == target) else {
        let supported = HOSTS
            .iter()
            .map(|host| host.triple)
            .collect::<Vec<_>>()
            .join(", ");
        panic!("hl-engine does not support target {target}; supported hosts are {supported}");
    };
    let directory = host.triple;
    let archive = root.join("assets/lib").join(directory).join(host.archive);
    // Before the existence check, so producing the archive re-runs this script.
    println!(
        "cargo:rerun-if-changed=assets/lib/{directory}/{name}",
        name = host.archive
    );
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
            format!(
                "    cmake --build <build-dir> --target refresh-crate-archives-{half}",
                half = host.refresh
            ),
            format!(
                "(equivalently: tools/refresh_crate_archives.sh --{half})",
                half = host.refresh
            ),
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
    // `+whole-archive` is load-bearing, not an optimisation: the archive's activation constructor
    // has no referenced symbol, so a plain `-l` drops the member and the engine never activates.
    // It maps to `--whole-archive` on a GNU link and `/WHOLEARCHIVE:` on link.exe, and the failure
    // when it is missing is silent on both.
    println!("cargo:rustc-link-lib=static:+whole-archive=hl-engine");
    for library in host.system_libraries {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
}
