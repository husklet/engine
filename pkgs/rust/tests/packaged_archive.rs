//! Guards the prebuilt engine archives that `cargo publish` actually ships.
//!
//! `build.rs` links `assets/lib/<target>/libhl-engine.a`; the C sources under
//! `src/` are never compiled by the crate. Those archives are committed
//! binaries, so they can silently fall behind the headers they were built from.
//! That happened once already: archives built at `HL_CONFIG_ABI 9` stayed in
//! the tree while the launch encoder moved to 13, and every published release
//! failed at runtime with "launch config has an invalid prefix" while still
//! linking and building cleanly.
//!
//! Linking proves nothing here, because an ABI mismatch is a runtime rejection.
//! These tests therefore exercise the committed archive the way a downstream
//! consumer does: they launch a guest through it.

use hl_engine::{Engine, Exit, Guest};
use std::path::{Path, PathBuf};

fn manifest() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn fixture(name: &str) -> PathBuf {
    manifest().join("testdata").join(name)
}

/// The archive linked into this test binary must accept a launch configuration
/// produced by this crate's encoder. A stale archive fails here, on both guest
/// backends, with a spawn error instead of a typed guest exit.
#[test]
fn committed_archive_launches_a_guest_on_both_backends() {
    let engine = Engine::new();
    for (guest, fixture_name) in [
        (Guest::Aarch64, "exit42-aarch64"),
        (Guest::X86_64, "exit42-x86_64"),
    ] {
        let exit = engine
            .command(guest, fixture(fixture_name))
            .status()
            .unwrap_or_else(|error| {
                panic!(
                    "the committed archive at assets/lib/ rejected a launch for {guest:?}: \
                     {error}\n\
                     This usually means the archive is stale relative to \
                     include/hl/config.h. Rebuild it with \
                     `make package-embedded-linux` (or `package-embedded-macos` on a mac), \
                     copy build/package/<arch>/libhl-engine.a into \
                     pkgs/rust/assets/lib/<target>/, and refresh \
                     pkgs/rust/assets/PROVENANCE.md."
                )
            });
        assert_eq!(exit, Exit::Code(42), "guest {guest:?} did not run to exit");
    }
}

/// Static cross-check against the C header, in the repository checkout only.
///
/// The launch ABI the Rust encoder emits is compiled into this crate, and the
/// archive that must accept it is built from `include/hl/config.h`. If those two
/// numbers disagree the archive cannot possibly be current, and this fails
/// without needing to run a guest — a cheaper, clearer signal than a spawn
/// error. Skipped when the C tree is absent (published crate, vendored copy).
#[test]
fn crate_launch_abi_matches_the_c_header() {
    let header = manifest().join("../../include/hl/config.h");
    if !header.exists() {
        return;
    }
    let expected = header_abi(&header).expect("include/hl/config.h defines HL_CONFIG_ABI");
    assert_eq!(
        hl_engine::launch_abi(),
        expected,
        "the crate encodes launch ABI {} but include/hl/config.h declares {expected}; \
         the committed archives under pkgs/rust/assets/lib/ are built from that header \
         and must be regenerated together with any ABI bump",
        hl_engine::launch_abi(),
    );
}

fn header_abi(path: &Path) -> Option<u32> {
    let text = std::fs::read_to_string(path).ok()?;
    for line in text.lines() {
        let Some(rest) = line.trim().strip_prefix("#define HL_CONFIG_ABI ") else {
            continue;
        };
        return rest.trim().trim_end_matches('u').parse().ok();
    }
    None
}
