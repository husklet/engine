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
        // MEASURED, not predicted. This list used to be a guess at what the host services layer
        // would need, with a note to replace it once an archive existed to read symbols from. One
        // does now, and the guess was too large by four entries. Taking the archive's undefined
        // symbols and subtracting its defined ones leaves 201 externals, and every one of them is
        // either a UCRT symbol (which rustc already links for this target), a compiler-rt builtin
        // (`__divti3`, `__udivti3`, `__modti3` -- the __int128 helpers -- and `__chkstk`, all
        // supplied by Rust's own compiler_builtins), or an import from exactly two libraries:
        //
        //   kernel32  everything the host backend touches directly: VirtualAlloc/VirtualProtect,
        //             CreateFileMappingW/MapViewOfFile, CreateProcessW and the ProcThreadAttribute
        //             family, TlsAlloc, the SRWLOCK and CONDITION_VARIABLE primitives, the
        //             vectored exception handler, QueryPerformanceCounter. rustc's own link line
        //             already includes it; naming it is not depending on that.
        //   bcrypt    BCryptGenRandom -- the secure host entropy the crate advertises, and the
        //             source of the random suffix in the seam's mkstemp/mkdtemp.
        //   oldnames  the POSIX SPELLINGS of the UCRT's own functions: open, close, read, write,
        //             dup, dup2, isatty, getpid, access, chdir, getcwd, unlink, lseek, umask.
        //             The UCRT declares all of these (its headers alias them when
        //             _CRT_DECLARE_NONSTDC_NAMES is on, which is the default) but its import
        //             library exports only the underscore-prefixed _open, _close and so on.
        //             oldnames.lib is the shim of weak aliases that maps one to the other, and
        //             without it the link fails with ~20 LNK2019s naming __imp_close and friends
        //             -- which read like a missing system library rather than a missing alias.
        //
        // Deliberately REMOVED, each for a measured reason rather than a stylistic one:
        //
        //   ntdll           not imported at all. The host backend reaches NtCreateFile,
        //                   NtQueryInformationFile and the rest through GetProcAddress into a
        //                   function-pointer table, so the archive carries no static reference to
        //                   ntdll and linking its import library resolves nothing.
        //   ws2_32          no socket call reaches this archive; the Windows host has no socket
        //                   seam yet. The crate's OWN Winsock calls keep it on the link line --
        //                   src/sys/windows.rs marks that extern block #[link(name = "ws2_32")].
        //   advapi32        same: the token and ACL entry points are the crate's, not the
        //                   archive's, and that extern block names the library itself.
        //   synchronization WaitOnAddress is not used. The futex-shaped primitive the host sync
        //                   layer needs is served by SRWLOCK/CONDITION_VARIABLE out of kernel32.
        //
        // `mincore` and `onecore` remain absent for the original reason: both are umbrella import
        // libraries for OneCore-targeted binaries, and linking one into a desktop static archive
        // replaces the individual libraries rather than supplementing them.
        //
        // These are import libraries, so they are emitted as `dylib=`: asking rustc for
        // `static=kernel32` would make it look for `libkernel32.a`. `m` has no counterpart --
        // the math functions live in the UCRT.
        //
        // This is measured against an archive that does NOT yet contain the activation layer
        // (src/core/activation.c has no Windows arm). That layer spawns processes and passes
        // handles, so it may add to this list; grow it from the archive's symbols then, the same
        // way, rather than pre-emptively.
        system_libraries: &["kernel32", "bcrypt", "oldnames"],
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
