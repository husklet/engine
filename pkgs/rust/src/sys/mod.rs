//! The crate's single host-platform boundary.
//!
//! Modelled on `std::sys`: exactly one place in `hl-engine` selects a platform, and everything above
//! it names only `crate::sys::…`. The crate had grown three two-arm `#[cfg]`s with no `else`
//! (`control.rs`, `machine.rs`, `ffi.rs`) and all three broke the moment a third host appeared, at
//! the point of use rather than at the point of decision. Scattering a fourth arm through the call
//! sites would repeat that; the fork belongs here, where it is one edit per platform.
//!
//! What lives below this line is small and deliberately so. Of the crate's ~12,000 lines the platform
//! fork is the dozen items re-exported here plus [`guest_path`]. The launch-config layout, the
//! checkpoint state machine, the provider framing and the protocol codecs are all shared: none of
//! them ever had a platform dependency, they were only ever *reached through* one.
//!
//! [`guest_path`] is the exception that proves the rule. It is shared, not forked, because a guest
//! path is a Linux path on every host — see its own documentation for why that is a security
//! property and not a portability detail.

// Two `#[cfg]` module declarations rather than a `cfg_if!`: this is what the macro expands to, and
// the crate declares zero dependencies (`Cargo.lock` holds one package). Keeping it that way is
// worth more than the two saved lines.
#[cfg(unix)]
#[path = "unix.rs"]
mod platform;

#[cfg(windows)]
#[path = "windows.rs"]
mod platform;

pub(crate) mod guest_path;

pub(crate) use platform::{
    adopt_file, adopt_stream, create_private_file, file_raw, is_socket, null_stdio, os_bytes,
    os_string, pipe_pair, secure_random, set_mode, signal, signal_process, stream_pair, symlink,
    OwnedDescriptor, RawDescriptor,
};

/// The one item of the platform surface that is not crate-private.
///
/// `transport::Channel::from_stream` is public API and takes one of these. It used to take a
/// `std::os::unix::net::UnixStream`, which is a host type in a signature that has no business
/// naming one; re-exporting the platform stream through `crate::transport` keeps the entry point
/// callable on every host and keeps its innards opaque. The Unix arm carries a `From<UnixStream>`
/// so an existing caller's migration is one `.into()`.
pub use platform::Stream;

/// Whether a path grants access to its owner and to nobody else.
///
/// Test-only by construction. It exists so the private-file guarantee below is a *checked* property
/// rather than a property of one host's API, and nothing in the crate's own paths needs to ask.
#[cfg(test)]
pub(crate) use platform::is_owner_only;

#[cfg(test)]
mod tests {
    use super::{create_private_file, is_owner_only, set_mode};

    /// Allocates a path in the host temporary directory that no other test is using.
    fn scratch(name: &str) -> std::path::PathBuf {
        static UNIQUE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let unique = UNIQUE.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        std::env::temp_dir().join(format!(
            "hl-sys-test-{name}-{}-{unique}",
            std::process::id()
        ))
    }

    /// The launch configuration and the projected guest namespace are written into the host's
    /// world-writable temporary directory, and both hold material the guest is trusted with and
    /// other local accounts are not. On Unix the guarantee is `open(2)`'s `0o600`.
    ///
    /// **A Windows arm that quietly did nothing would compile, would satisfy every other test in
    /// this crate, and would hand both files to every account on the box.** That is a security
    /// regression, not a missing feature: nothing fails, nothing warns, and the only visible
    /// difference is who can read the launch configuration. This test exists so the guarantee is a
    /// checked property on both hosts rather than a property of one host's `OpenOptionsExt`.
    ///
    /// If this fails, treat it as a real defect before treating it as an over-strict assertion.
    #[test]
    fn a_private_file_is_readable_only_by_its_owner() {
        let path = scratch("private");
        let file = create_private_file(&path).expect("create the private file");
        drop(file);
        assert!(
            is_owner_only(&path).expect("read back the private file's protection"),
            "create_private_file must not leave {} reachable by another account",
            path.display()
        );
        std::fs::remove_file(&path).expect("remove the private file");
    }

    /// `set_mode` carries the same guarantee for a file that already exists, which is how
    /// `projection.rs` applies a projected node's mode after writing its bytes.
    ///
    /// The `0o644` half is not decoration. It is what stops this test from passing against a
    /// `set_mode` that ignores its argument and locks everything down, and against an
    /// `is_owner_only` that answers `true` unconditionally.
    #[test]
    fn set_mode_narrows_an_existing_file_to_its_owner() {
        let path = scratch("mode");
        std::fs::write(&path, b"projected").expect("write the projected file");
        set_mode(&path, 0o644).expect("widen the projected file");
        assert!(
            !is_owner_only(&path).expect("read back the widened file's protection"),
            "mode 0o644 grants read outside the owner and must not report as owner-only"
        );
        set_mode(&path, 0o600).expect("narrow the projected file");
        assert!(
            is_owner_only(&path).expect("read back the narrowed file's protection"),
            "set_mode(0o600) must not leave {} reachable by another account",
            path.display()
        );
        std::fs::remove_file(&path).expect("remove the projected file");
    }
}
