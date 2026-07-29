//! Guest paths, which are Linux paths and not host paths.
//!
//! This module is deliberately *not* a platform fork. It exists because `std::path::Path` is a host
//! path parser wearing a name that suggests it is a general one, and every guest path in this crate
//! is consumed by the Linux guest as an arbitrary `/`-separated byte string. The two types happen to
//! share a name; they do not share a meaning:
//!
//! ```text
//! Path::new("/etc/passwd").is_absolute()   Linux: true    Windows: false
//! Path::new(r"a\b").components()           Linux: one Normal    Windows: two Normal
//! Path::new("/etc/x").strip_prefix("/")    joins back as  C:\tmp\proj\etc/x
//! ```
//!
//! The first line is the dangerous one, and it is why these helpers exist at all. `wire.rs`'s
//! file-owner guard rejects absolute guest keys; on Windows `is_absolute` answers `false` for
//! `/etc/passwd`, the guard stops firing, and an absolute path is written into the launch config
//! where the engine expects a normalized relative one. Nothing warns, nothing fails to compile, and
//! no existing test can see it. Asking the byte string directly gives one answer on every host.
//!
//! **Backslash.** A Linux guest may legitimately name one entry `a\b`; systemd unit files do this
//! in ordinary distribution images. Unix hosts give that byte the same meaning, while Windows
//! `Path` reads it as a separator. Windows therefore rejects such paths until the public spec uses
//! a host-independent guest-path type. Unix hosts must not narrow Linux filename semantics.
//!
//! Bytes come from `OsStr::as_encoded_bytes`, which is the platform-native encoding: bytes on Unix
//! and WTF-8 on Windows. Both encode ASCII as ASCII, so `/`, `\`, `.` and NUL compare byte-for-byte
//! on either host.

use std::path::Path;

/// The guest-path separator. Always this one, on every host.
pub(crate) const SEPARATOR: u8 = b'/';

/// The raw bytes behind a guest path.
pub(crate) fn bytes(path: &Path) -> &[u8] {
    path.as_os_str().as_encoded_bytes()
}

/// Rooted at the guest filesystem root.
///
/// This is the answer `Path::is_absolute` gives on Unix and does not give on Windows.
pub(crate) fn is_absolute(path: &[u8]) -> bool {
    path.first() == Some(&SEPARATOR)
}

/// Names the guest root and nothing else.
pub(crate) fn is_root(path: &[u8]) -> bool {
    !path.is_empty() && path.iter().all(|byte| *byte == SEPARATOR)
}

/// The `/`-separated segments, including the empty ones a doubled or trailing separator produces.
///
/// Empty segments are kept rather than skipped: they are what makes `a//b` and `a/` detectably
/// unnormalized, and `Path::components` silently discards them.
pub(crate) fn segments(path: &[u8]) -> impl Iterator<Item = &[u8]> {
    let path = if is_absolute(path) { &path[1..] } else { path };
    path.split(move |byte| *byte == SEPARATOR)
}

/// A segment that names exactly one guest directory entry.
///
/// This is the byte-level form of `matches!(component, Component::Normal(_))`, with the two
/// difference that matters on every host: empty and special segments are rejected rather than
/// discarded. Windows also rejects `\`, whose host-path meaning differs from Linux.
pub(crate) fn is_normal(segment: &[u8]) -> bool {
    !segment.is_empty()
        && segment != b"."
        && segment != b".."
        && (!cfg!(windows) || !segment.contains(&b'\\'))
        && !segment.contains(&0)
}

/// Every segment names exactly one entry: no empty, `.`, `..`, or NUL segment anywhere.
///
/// Windows additionally rejects `\` until guest paths stop using the host `Path` type.
pub(crate) fn all_normal(path: &[u8]) -> bool {
    segments(path).all(is_normal)
}

/// A normalized path rooted at the guest filesystem root.
pub(crate) fn is_normalized_absolute(path: &[u8]) -> bool {
    is_absolute(path) && all_normal(path)
}

/// A normalized path relative to some guest directory. Never empty, never rooted.
pub(crate) fn is_normalized_relative(path: &[u8]) -> bool {
    !path.is_empty() && !is_absolute(path) && all_normal(path)
}

/// Traverses out of the directory it is resolved against.
///
/// Weaker than [`is_normalized_absolute`] on purpose: it is the exact question
/// `engine::validation` asks today, and answering a different one would reject launches that work.
pub(crate) fn escapes(path: &[u8]) -> bool {
    segments(path).any(|segment| segment == b"..")
}

/// Splits an absolute guest path into the segments to be appended to a host directory.
///
/// `projection.rs` used `strip_prefix("/")` and one `Path::join`, which yields
/// `C:\tmp\proj\etc/x` — mixed separators that Win32 accepts and the first string comparison
/// written against them does not. Joining segment by segment produces a native host path.
pub(crate) fn host_segments(path: &[u8]) -> impl Iterator<Item = &[u8]> {
    segments(path).filter(|segment| !segment.is_empty())
}

#[cfg(test)]
mod tests {
    use super::{
        all_normal, escapes, host_segments, is_absolute, is_normalized_absolute,
        is_normalized_relative, is_root, segments,
    };
    use std::path::Path;

    /// The regression this whole module exists for.
    ///
    /// **`Path::new("/etc/passwd").is_absolute()` is `false` on Windows**, because absoluteness
    /// there requires a prefix (a drive letter or a UNC root) and a bare leading separator is not
    /// one. `wire.rs::file_owners` rejects absolute guest keys with exactly that call, so on Windows
    /// the rejection silently stops happening and `/etc/passwd` is written into the launch
    /// configuration as a file-owner key where the engine expects a normalized relative path. It
    /// compiles, it warns about nothing, and no test that goes through `std::path` can see it.
    ///
    /// The conclusion is not "add a `#[cfg]`". It is that **guest paths must not go through
    /// `std::path` at all**: they are Linux paths — `/`-separated arbitrary byte strings — and
    /// `Path` on Windows is a Win32 path parser. Two different types that share a name.
    ///
    /// The `Path` assertion below is stated as a host-conditional fact rather than skipped on Unix,
    /// because that disagreement is the reason for the byte-level answer and recording it is what
    /// stops the next reader from "simplifying" this module back into `Path::is_absolute`.
    #[test]
    fn a_guest_absolute_path_is_absolute_on_every_host() {
        assert!(is_absolute(b"/etc/passwd"));
        assert!(is_absolute(b"/"));
        assert!(!is_absolute(b"etc/passwd"));
        assert!(!is_absolute(b""));

        let host = Path::new("/etc/passwd").is_absolute();
        if cfg!(windows) {
            assert!(
                !host,
                "std::path is expected to disagree here; that disagreement is the whole point"
            );
        } else {
            assert!(host);
        }
        assert!(
            is_absolute(super::bytes(Path::new("/etc/passwd"))),
            "the byte-level answer must not depend on the host"
        );
    }

    /// `wire.rs::file_owners` and the namespace validators require every component to name one
    /// entry. `a\b` is one valid Linux filename and must stay valid on Unix hosts. Windows rejects
    /// it because its host `Path` type would reinterpret it.
    #[test]
    fn a_backslash_segment_preserves_linux_semantics_on_unix() {
        assert_eq!(
            Path::new(r"a\b").components().count(),
            1 + usize::from(cfg!(windows))
        );
        assert_eq!(all_normal(br"a\b"), !cfg!(windows));
        assert_eq!(is_normalized_relative(br"a\b"), !cfg!(windows));
        assert_eq!(is_normalized_absolute(br"/a\b"), !cfg!(windows));
        assert!(is_normalized_relative(b"a/b"));
    }

    /// `Path::components` discards empty components, so `a//b` and `a/` pass a Normal-only check
    /// while being exactly the unnormalized input the check is documented to reject.
    #[test]
    fn unnormalized_separators_are_visible_at_the_byte_level() {
        assert_eq!(Path::new("a//b").components().count(), 2);
        assert!(!is_normalized_relative(b"a//b"));
        assert!(!is_normalized_relative(b"a/"));
        assert!(!is_normalized_relative(b"/a"));
        assert!(!is_normalized_relative(b""));
        assert!(!is_normalized_absolute(b"/a/"));
        assert!(is_normalized_absolute(b"/a/b"));
        assert!(!is_normalized_absolute(b"a/b"));
    }

    #[test]
    fn traversal_and_root_are_classified_without_the_host() {
        assert!(escapes(b"/a/../b"));
        assert!(escapes(b"../escape"));
        assert!(!escapes(b"/a/b"));
        assert!(!escapes(b"/a/..b"));
        assert!(is_root(b"/"));
        assert!(is_root(b"//"));
        assert!(!is_root(b"/a"));
        assert!(!is_root(b""));
    }

    #[test]
    fn host_segments_drop_the_root_and_every_empty_run() {
        assert_eq!(
            host_segments(b"/etc/x").collect::<Vec<_>>(),
            vec![&b"etc"[..], &b"x"[..]]
        );
        assert_eq!(host_segments(b"/").collect::<Vec<_>>(), Vec::<&[u8]>::new());
        assert_eq!(
            segments(b"/a/").collect::<Vec<_>>(),
            vec![&b"a"[..], &b""[..]]
        );
    }
}
