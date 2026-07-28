//! Why a test that needs a running guest is skipped instead of being absent.
//!
//! Roughly half of this suite is not an API test at all: it launches a Linux guest through the
//! engine and asserts on what the guest did. There is no Windows engine yet, so none of that half
//! can run there. The tempting fix -- `#![cfg(unix)]` at the top of the affected files -- makes the
//! tests *vanish*: `cargo test` reports a clean pass over the ones that remain, the count silently
//! drops by more than half, and the day a Windows engine lands nobody knows which tests to turn on
//! because there is nothing left to turn on. That is a coverage cliff dressed as a green run.
//!
//! So the gate is a runtime one, modelled on `support/checkpoint_env.rs`, and it self-calibrates:
//! each precondition is *measured* here rather than guessed from a `cfg`, once per test binary. A
//! gated test prints one `SKIP <name>: <reason>` line and returns. When the engine can run guests,
//! every one of them starts running again with no code change, and the diff that "turns them on" is
//! empty.
//!
//! **The gates never fire on Unix.** They are asked the question and answer "run it" without
//! probing, deliberately: on Linux and macOS the engine is production, the Alpine fixture is
//! extracted by CI and `cc` is a build dependency, so a host that cannot satisfy one of these has a
//! defect that must surface as a *failure*. Turning that into a skip would hide on Unix exactly the
//! kind of regression this module exists to stop hiding on Windows.

// Pulled in with `#[path]` by several test binaries, each of which compiles a private copy and uses
// only the gates it needs.
#![allow(dead_code)]

use hl_engine::{Engine, Exit, Guest};
use std::{path::PathBuf, process::Command, sync::OnceLock};

fn manifest(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

/// Whether the linked engine archive can execute a guest of one specific ISA.
///
/// Per-ISA rather than one boolean because "can this engine run a guest" is two questions on a host
/// whose archive does not carry every translator. A production POSIX archive carries both and
/// dispatches between them, so the distinction is invisible there. The Windows archive is built from
/// a single guest-target unity translation unit (x86-64), which makes an aarch64 launch fail with a
/// typed "no backend for that ISA" -- a different fact from "activation is broken", and one the skip
/// reason has to be able to say, because the two have completely different fixes.
///
/// The probe is a real launch of the smallest guest there is: a static Linux binary whose whole job
/// is to exit 42. An archive that links and then cannot run anything is exactly the state a
/// half-finished port is in, and it is the state this has to distinguish from a working one.
fn guest_isa_runs(guest: Guest) -> bool {
    static AARCH64: OnceLock<bool> = OnceLock::new();
    static X86_64: OnceLock<bool> = OnceLock::new();
    let (cell, fixture) = match guest {
        Guest::Aarch64 => (&AARCH64, "testdata/exit42-aarch64"),
        Guest::X86_64 => (&X86_64, "testdata/exit42-x86_64"),
    };
    *cell.get_or_init(|| {
        Engine::new()
            .command(guest, manifest(fixture))
            .status()
            .is_ok_and(|exit| exit == Exit::Code(42))
    })
}

/// Names the ISAs this archive can and cannot run, for a skip reason that is actionable.
fn isa_summary() -> String {
    let carried: Vec<&str> = [(Guest::Aarch64, "aarch64"), (Guest::X86_64, "x86-64")]
        .into_iter()
        .filter(|&(guest, _)| guest_isa_runs(guest))
        .map(|(_, name)| name)
        .collect();
    if carried.is_empty() {
        "no guest ISA".to_owned()
    } else {
        format!("only the {} guest target", carried.join(" and "))
    }
}

/// A scratch directory this binary owns, removed and recreated on first use.
fn scratch() -> PathBuf {
    manifest("target").join(format!("hl-engine-env-{}", std::process::id()))
}

/// Whether the linked engine archive can execute the guests this suite's `testdata/` fixtures use.
///
/// Both ISAs, not either: every fixture in `testdata/` is committed in an aarch64 and an x86-64
/// build, and the tests behind this gate either loop over both or name aarch64 outright. So the
/// question a caller is really asking is "can this engine run MY fixtures", and an archive carrying
/// one translator answers no to that even though it runs guests perfectly well.
fn guest_runs() -> bool {
    guest_isa_runs(Guest::Aarch64) && guest_isa_runs(Guest::X86_64)
}

/// Whether the pinned Alpine fixture can be unpacked into a usable guest root here.
///
/// Separate from [`guest_runs`] because it fails for its own reasons: the tarball carries symlinks
/// and Unix modes, and an extractor that drops them leaves a directory that looks extracted and is
/// not a rootfs.
fn rootfs_extracts() -> bool {
    static EXTRACTS: OnceLock<bool> = OnceLock::new();
    *EXTRACTS.get_or_init(|| {
        // The pinned fixture is `alpine-minirootfs-...-aarch64.tar.gz`, so this needs the aarch64
        // translator specifically -- an engine that runs x86-64 guests cannot execute a single
        // binary in this root filesystem.
        if !guest_isa_runs(Guest::Aarch64) {
            return false;
        }
        let path = scratch().join("rootfs-probe");
        let _ = std::fs::remove_dir_all(&path);
        if std::fs::create_dir_all(&path).is_err() {
            return false;
        }
        let extracted = Command::new("tar")
            .arg("-xzf")
            .arg(manifest(
                "assets/alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz",
            ))
            .arg("-C")
            .arg(&path)
            .status()
            .is_ok_and(|status| status.success());
        // `/bin/sh` is a symlink to busybox in this image, so this asks two questions at once: did
        // the archive unpack, and did the extractor keep the symlinks the guest resolves through?
        let usable = extracted && path.join("bin/sh").exists();
        let _ = std::fs::remove_dir_all(&path);
        usable
    })
}

/// Whether a `cc` on PATH builds the static Linux guest binaries the probe fixtures need.
///
/// Several tests compile a small C program and run it *as the guest*, so this is not "is there a
/// compiler" -- a Windows `cc` producing a PE would satisfy that and produce something the engine
/// cannot load. The check is therefore a real `-static` compile plus a launch of the result.
fn guest_compiler_works() -> bool {
    static WORKS: OnceLock<bool> = OnceLock::new();
    *WORKS.get_or_init(|| {
        if !rootfs_extracts() {
            return false;
        }
        let directory = scratch();
        if std::fs::create_dir_all(&directory).is_err() {
            return false;
        }
        let source = directory.join("compiler-probe.c");
        let binary = directory.join("compiler-probe");
        if std::fs::write(&source, "int main(void) { return 42; }\n").is_err() {
            return false;
        }
        let built = Command::new("cc")
            .args(["-static", "-O0", "-o"])
            .arg(&binary)
            .arg(&source)
            .status()
            .is_ok_and(|status| status.success());
        let works = built
            && Engine::new()
                .command(Guest::Aarch64, &binary)
                .status()
                .is_ok_and(|exit| exit == Exit::Code(42));
        let _ = std::fs::remove_dir_all(&directory);
        works
    })
}

/// Announces a skip and answers `true`, so a caller reads as `if skip(..) { return; }`.
fn announce(test: &str, reason: &str) -> bool {
    eprintln!("SKIP {test}: {reason}");
    true
}

/// Gate for a test that launches a guest from a committed `testdata/` fixture.
#[must_use]
pub fn skip_without_guest(test: &str) -> bool {
    if !cfg!(windows) || guest_runs() {
        return false;
    }
    announce(
        test,
        &format!(
            "the engine archive linked into this binary carries {} and this test's `testdata/` \
             fixtures need both aarch64 and x86-64; delete this guard once it carries both",
            isa_summary()
        ),
    )
}

/// Gate for a test that also needs the pinned Alpine root filesystem.
#[must_use]
pub fn skip_without_rootfs(test: &str) -> bool {
    if !cfg!(windows) || rootfs_extracts() {
        return false;
    }
    if !guest_isa_runs(Guest::Aarch64) {
        return announce(
            test,
            &format!(
                "the pinned Alpine fixture is an aarch64 root filesystem and this engine archive \
                 carries {}",
                isa_summary()
            ),
        );
    }
    announce(
        test,
        "the pinned Alpine fixture does not unpack into a usable guest root on this host \
         (its symlinks and Unix modes did not survive extraction)",
    )
}

/// Gate for a test that also compiles its own C probe and runs it as the guest.
#[must_use]
pub fn skip_without_guest_compiler(test: &str) -> bool {
    if !cfg!(windows) || guest_compiler_works() {
        return false;
    }
    if !rootfs_extracts() {
        return skip_without_rootfs(test);
    }
    announce(
        test,
        "no `cc` on PATH produces a static Linux binary this host's engine can run as a guest",
    )
}
