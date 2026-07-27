//! The Unix arm of [`crate::sys`]: Linux and macOS.
//!
//! Everything here was already in the crate; it moved rather than changed. The one deliberate
//! difference from the code it replaces is that `OwnedDescriptor` now wraps `std::os::fd::OwnedFd`
//! instead of hand-rolling the close, and the raw `close` extern is gone with it.
#![allow(unsafe_code)]

use std::{
    ffi::{c_int, OsStr, OsString},
    fs::{File, Metadata, OpenOptions},
    io,
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::{
            ffi::{OsStrExt, OsStringExt},
            fs::{OpenOptionsExt, PermissionsExt},
            net::UnixStream,
        },
    },
    path::Path,
    time::Duration,
};

/// A descriptor as the C engine names it. An `int` fd here; see `windows.rs` for the other half.
pub(crate) type RawDescriptor = c_int;

/// The host signal numbers the control plane sends.
///
/// These are the numbers this *host* uses, not the guest's: `child.rs` delivers them with `kill(2)`
/// to a host pid. Linux and macOS agree on the first five and disagree on the rest.
pub(crate) mod signal {
    pub(crate) const HANGUP: i32 = 1;
    pub(crate) const INTERRUPT: i32 = 2;
    pub(crate) const QUIT: i32 = 3;
    pub(crate) const KILL: i32 = 9;
    pub(crate) const TERMINATE: i32 = 15;

    #[cfg(target_os = "linux")]
    pub(crate) const USER1: i32 = 10;
    #[cfg(target_os = "linux")]
    pub(crate) const USER2: i32 = 12;
    #[cfg(target_os = "linux")]
    pub(crate) const STOP: i32 = 19;
    #[cfg(target_os = "linux")]
    pub(crate) const CONTINUE: i32 = 18;

    #[cfg(target_os = "macos")]
    pub(crate) const USER1: i32 = 30;
    #[cfg(target_os = "macos")]
    pub(crate) const USER2: i32 = 31;
    #[cfg(target_os = "macos")]
    pub(crate) const STOP: i32 = 17;
    #[cfg(target_os = "macos")]
    pub(crate) const CONTINUE: i32 = 19;

    /// The host signal the engine reserves to bounce a process out of a blocking host syscall to its
    /// next dispatcher safepoint (where checkpoint capture runs).
    ///
    /// It MUST be the engine's own `THREAD_INT_SIG` (`src/linux_abi/thread.c`), the only signal for
    /// which every engine process installs a permanent, guest-unreachable handler. Any
    /// guest-reachable signal is useless here: the engine installs a host handler for one only when
    /// the guest itself calls `rt_sigaction` on it, so kicking with (say) `SIGURG` is a silent no-op
    /// against a shell that never handles `SIGURG` -- its default action is *ignore*, which does not
    /// interrupt a blocked `read`. That is exactly how an interactive `bash` parked on its PTY was
    /// left unreachable, and capture then never began at all.
    pub(crate) fn interrupt_engine() -> i32 {
        // Linux: SIGRTMIN + 7, matching native_compat.h's SIGINFO alias.
        #[cfg(target_os = "linux")]
        {
            // SAFETY: a pure query of the C library's runtime signal base; no arguments, no state.
            unsafe { __libc_current_sigrtmin() + 7 }
        }
        // macOS: SIGINFO(29), which sig_l2m omits.
        #[cfg(target_os = "macos")]
        {
            29
        }
    }

    #[cfg(target_os = "linux")]
    unsafe extern "C" {
        fn __libc_current_sigrtmin() -> std::ffi::c_int;
    }
}

unsafe extern "C" {
    fn pipe(descriptors: *mut c_int) -> c_int;
    fn fcntl(descriptor: c_int, command: c_int, ...) -> c_int;
    #[link_name = "kill"]
    fn process_signal(process: c_int, signal: c_int) -> c_int;
}

/// A raw descriptor this process owns and closes on drop.
///
/// The engine takes the raw number across the FFI boundary and duplicates it there, so this type
/// only has to keep the number valid for the duration of the call.
#[derive(Debug)]
pub(crate) struct OwnedDescriptor(OwnedFd);

impl OwnedDescriptor {
    /// Adopts a descriptor the engine installed into this process.
    ///
    /// # Safety
    /// `raw` must be an open descriptor owned by this process and not owned by anything else.
    pub(crate) unsafe fn adopt(raw: RawDescriptor) -> Self {
        // SAFETY: forwarded to the caller by this function's own contract.
        Self(unsafe { OwnedFd::from_raw_fd(raw) })
    }

    pub(crate) fn raw(&self) -> RawDescriptor {
        self.0.as_raw_fd()
    }
}

/// A reliable, ordered, bidirectional local byte stream.
///
/// `AF_UNIX` + `SOCK_STREAM`, which is also what the Windows arm provides. The protocols carried
/// over it frame themselves, so message boundaries are not part of this contract, and neither is
/// ancillary data: no Rust code in this crate sends or receives a descriptor.
///
/// A newtype rather than an alias for `UnixStream`, so that the inherent method set is exactly the
/// one the Windows arm can offer and the call sites above `sys` compile unchanged on both.
///
/// `pub` because `transport::Channel::from_stream` is public API and names it. Opaque: every method
/// below is crate-private, and the only public way in is the `From<UnixStream>` conversion.
#[derive(Debug)]
pub struct Stream(UnixStream);

impl From<UnixStream> for Stream {
    fn from(stream: UnixStream) -> Self {
        Self(stream)
    }
}

impl Stream {
    /// A second handle on the same stream, so a reader and a writer can hold one each.
    pub(crate) fn try_clone(&self) -> io::Result<Self> {
        self.0.try_clone().map(Self)
    }

    /// Bounds a blocking read. `None` restores an unbounded one.
    pub(crate) fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.0.set_read_timeout(timeout)
    }

    /// Bounds a blocking write. `None` restores an unbounded one.
    pub(crate) fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.0.set_write_timeout(timeout)
    }

    /// The raw descriptor, for the duration of the FFI call that reads it.
    pub(crate) fn raw(&self) -> RawDescriptor {
        self.0.as_raw_fd()
    }
}

impl io::Read for Stream {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        self.0.read(bytes)
    }
}

impl io::Write for Stream {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0.write(bytes)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

/// A connected pair, created before process creation so neither side discovers an ambient
/// descriptor.
pub(crate) fn stream_pair() -> io::Result<(Stream, Stream)> {
    let (left, right) = UnixStream::pair()?;
    Ok((Stream(left), Stream(right)))
}

/// Adopts the accepted checkpoint channel the engine handed back over `SCM_RIGHTS`.
///
/// # Safety
/// `raw` must be an open stream socket owned by this process and not owned by anything else.
pub(crate) unsafe fn adopt_stream(raw: RawDescriptor) -> Stream {
    // SAFETY: forwarded to the caller by this function's own contract.
    Stream(unsafe { UnixStream::from_raw_fd(raw) })
}

/// An anonymous unidirectional pipe, closed on exec at both ends.
///
/// `FD_CLOEXEC` is not hygiene here: without it the guest descriptor scan sees sockets it cannot
/// account for and refuses to checkpoint at all.
pub(crate) fn pipe_pair() -> io::Result<(File, File)> {
    const F_SETFD: c_int = 2;
    const FD_CLOEXEC: c_int = 1;
    let mut descriptors = [-1, -1];
    // SAFETY: `pipe(2)` writes exactly two descriptors into the array it is given.
    if unsafe { pipe(descriptors.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: both descriptors were just created by pipe(2) and are owned by this process.
    let read = unsafe { File::from_raw_fd(descriptors[0]) };
    // SAFETY: as above.
    let write = unsafe { File::from_raw_fd(descriptors[1]) };
    // SAFETY: both descriptors are open and owned; F_SETFD takes one int.
    if unsafe { fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) } != 0
        // SAFETY: as above.
        || unsafe { fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) } != 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok((read, write))
}

/// Adopts a file the engine created, such as the terminal master.
///
/// # Safety
/// `raw` must be an open descriptor owned by this process and not owned by anything else.
pub(crate) unsafe fn adopt_file(raw: RawDescriptor) -> File {
    // SAFETY: forwarded to the caller by this function's own contract.
    unsafe { File::from_raw_fd(raw) }
}

/// The raw descriptor for a file, for the duration of the FFI call that reads it.
pub(crate) fn file_raw(file: &File) -> RawDescriptor {
    file.as_raw_fd()
}

/// The host's discard stream, opened for the direction the guest will use it in.
pub(crate) fn null_stdio(read: bool) -> io::Result<File> {
    OpenOptions::new().read(read).write(!read).open("/dev/null")
}

/// Fills `bytes` from the host's cryptographic entropy source.
pub(crate) fn secure_random(bytes: &mut [u8]) -> io::Result<()> {
    use std::io::Read as _;
    File::open("/dev/urandom")?.read_exact(bytes)
}

/// Creates a new file no other account can read.
///
/// The mode is applied by `open(2)` rather than afterwards, so there is no window in which the file
/// exists with the directory's default protection.
pub(crate) fn create_private_file(path: &Path) -> io::Result<File> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
}

/// Whether `path` grants access to its owner and to nobody else.
///
/// Only the tests for [`create_private_file`] and [`set_mode`] use this; it exists so that the
/// private-file guarantee is a checked property rather than a property of one host's API.
#[cfg(test)]
pub(crate) fn is_owner_only(path: &Path) -> io::Result<bool> {
    Ok(std::fs::metadata(path)?.permissions().mode() & 0o077 == 0)
}

/// Applies a Linux mode to a host file.
pub(crate) fn set_mode(path: &Path, mode: u32) -> io::Result<()> {
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
}

/// Creates a symbolic link at `link` pointing at `target`.
pub(crate) fn symlink(target: &Path, link: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(target, link)
}

/// Whether the metadata names a Unix domain socket.
pub(crate) fn is_socket(metadata: &Metadata) -> bool {
    use std::os::unix::fs::FileTypeExt as _;
    metadata.file_type().is_socket()
}

/// Delivers a host signal to one host process.
pub(crate) fn signal_process(process: u64, signal: i32) -> io::Result<()> {
    let process = c_int::try_from(process).map_err(|_| io::Error::from_raw_os_error(22))?;
    // SAFETY: `kill(2)` takes two integers and touches no memory this process owns.
    if unsafe { process_signal(process, signal) } == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// The bytes a host string contributes to the guest's launch configuration.
///
/// On Unix an `OsStr` *is* a byte string, so this is a lossless identity and never fails. The
/// Windows arm has to choose an encoding; see it for the policy and why it can return `None`.
// The `Option` is the platform contract, not this arm's return type. Unwrapping it here would move
// the fallibility back out to every caller as a `#[cfg]`, which is the shape `sys` exists to avoid.
#[allow(clippy::unnecessary_wraps)]
pub(crate) fn os_bytes(value: &OsStr) -> Option<&[u8]> {
    Some(value.as_bytes())
}

/// The host string for bytes received from the engine. The inverse of [`os_bytes`].
#[allow(clippy::unnecessary_wraps)] // As above: infallible here, fallible in the contract.
pub(crate) fn os_string(bytes: Vec<u8>) -> Option<OsString> {
    Some(OsString::from_vec(bytes))
}
