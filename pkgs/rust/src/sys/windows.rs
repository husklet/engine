//! The Windows arm of [`crate::sys`].
//!
//! Three things here are decisions rather than translations, and each is written up where it is
//! implemented:
//!
//! * **The local byte stream is `AF_UNIX` + `SOCK_STREAM`**, spoken to `ws2_32` directly because
//!   `std` exposes no `AF_UNIX` on Windows even though the kernel has it. Not a named pipe and not
//!   loopback TCP — see [`stream_pair`].
//! * **`create_private_file` and `set_mode` build real DACLs.** A no-op arm would compile, pass every
//!   existing test, and hand the launch configuration and the projected guest namespace to every
//!   account on the box. See [`set_mode`].
//! * **Host strings encode as UTF-8 and reject what will not.** See [`os_bytes`].
//!
//! Two functions are honest stubs rather than emulations, because emulating them would be worse than
//! failing: [`signal_process`] (there is no `kill(2)`, and the C engine already owns the operation)
//! and [`is_socket`] (no Unix domain socket has a file type here, so a projected socket is refused
//! rather than silently accepted).
#![allow(unsafe_code)]

use std::{
    ffi::{c_int, c_void, OsStr, OsString},
    fs::{File, Metadata, OpenOptions},
    io,
    os::windows::{
        ffi::OsStrExt as _,
        io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle},
    },
    path::Path,
    time::Duration,
};

/// A descriptor as the C engine names it.
///
/// Deliberately the same `int` the Unix arm uses, and deliberately narrower than a `HANDLE` or a
/// `SOCKET`. Windows kernel handle *values* are documented to be 32-bit significant — that is the
/// contract that lets a 64-bit process hand a handle to a 32-bit one and back — so an `int` carries
/// one, sign-extended on the way out. Keeping the width means the crate reaches "compiles and links"
/// with no change to `include/hl/activation.h`, and the C-side descriptor-type agreement stays a
/// decision the host-backend owner makes rather than one this file forces.
pub(crate) type RawDescriptor = c_int;

/// Widens a descriptor back to the handle the kernel gave out.
fn as_handle(raw: RawDescriptor) -> RawHandle {
    // Sign-extend: a handle value that set the top bit of the `int` must come back as it went in.
    raw as isize as RawHandle
}

/// Narrows a handle to the width the C engine's descriptor parameters carry.
///
/// The truncation is the point, not an accident: see [`RawDescriptor`] for why the 32 low bits are
/// the whole value.
#[allow(clippy::cast_possible_truncation)]
fn as_raw(handle: RawHandle) -> RawDescriptor {
    handle as isize as RawDescriptor
}

/// A Win32 status code as `std::io::Error` wants it. The two spellings name the same 32 bits.
fn status_error(status: Dword) -> io::Error {
    io::Error::from_raw_os_error(i32::from_ne_bytes(status.to_ne_bytes()))
}

/// Signal numbers for the control plane.
///
/// Windows has no signals, so these are not host numbers: they are the *Linux* numbers, which is
/// what the guest ABI speaks and what a Windows host backend would have to forward through
/// `hl_engine_request(HL_ENGINE_REQUEST_SIGNAL)`. Nothing in this crate can deliver one yet —
/// [`signal_process`] says so — so these constants exist to keep `control::Signal::host_number`
/// total rather than to reach a process. Do not read them as a claim that delivery works.
pub(crate) mod signal {
    pub(crate) const HANGUP: i32 = 1;
    pub(crate) const INTERRUPT: i32 = 2;
    pub(crate) const QUIT: i32 = 3;
    pub(crate) const KILL: i32 = 9;
    pub(crate) const TERMINATE: i32 = 15;
    pub(crate) const USER1: i32 = 10;
    pub(crate) const USER2: i32 = 12;
    pub(crate) const STOP: i32 = 19;
    pub(crate) const CONTINUE: i32 = 18;

    /// The engine's reserved safepoint interrupt. `SIGRTMIN + 7` on a glibc host; there is no
    /// runtime `SIGRTMIN` to query here, and the value is only meaningful once the C engine can
    /// deliver it, so the Linux base is stated rather than discovered.
    pub(crate) const fn interrupt_engine() -> i32 {
        34 + 7
    }
}

// --- Win32 -------------------------------------------------------------------------------------
//
// Declared by hand, like `ffi.rs` declares the engine's own surface: the crate has zero
// dependencies and adding `windows-sys` to reach three dozen entry points would be the largest
// change in this file.

type Bool = i32;
type Dword = u32;
type Handle = *mut c_void;
type Socket = usize;

const FALSE: Bool = 0;
const TRUE: Bool = 1;
const INVALID_HANDLE_VALUE: Handle = usize::MAX as Handle;
const INVALID_SOCKET: Socket = usize::MAX;
const SOCKET_ERROR: i32 = -1;

const AF_UNIX: i32 = 1;
const SOCK_STREAM: i32 = 1;
const WSA_FLAG_OVERLAPPED: u32 = 0x01;
const WSA_FLAG_NO_HANDLE_INHERIT: u32 = 0x80;
const SOL_SOCKET: i32 = 0xffff;
const SO_RCVTIMEO: i32 = 0x1006;
const SO_SNDTIMEO: i32 = 0x1005;
const WSAEINVAL: i32 = 10_022;

const GENERIC_WRITE: Dword = 0x4000_0000;
const WRITE_DAC: Dword = 0x0004_0000;
const CREATE_NEW: Dword = 1;
const FILE_ATTRIBUTE_NORMAL: Dword = 0x80;

const ERROR_PRIVILEGE_NOT_HELD: i32 = 1314;

const TOKEN_QUERY: Dword = 0x0008;
const TOKEN_USER_CLASS: i32 = 1;
const ACL_REVISION: Dword = 2;
const SECURITY_DESCRIPTOR_REVISION: Dword = 1;
const SECURITY_MAX_SID_SIZE: usize = 68;
const WIN_WORLD_SID: i32 = 1;
const SE_FILE_OBJECT: i32 = 1;
const DACL_SECURITY_INFORMATION: Dword = 0x0000_0004;
const PROTECTED_DACL_SECURITY_INFORMATION: Dword = 0x8000_0000;
#[cfg(test)]
const ACCESS_ALLOWED_ACE_TYPE: u8 = 0;
#[cfg(test)]
const ACL_SIZE_INFORMATION: i32 = 2;

// The generic file rights, as `winnt.h` composes them.
const FILE_GENERIC_READ: Dword = 0x0012_0089;
const FILE_GENERIC_WRITE: Dword = 0x0012_0116;
const FILE_GENERIC_EXECUTE: Dword = 0x0012_00a0;
const DELETE: Dword = 0x0001_0000;

const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 2;

#[repr(C)]
struct SecurityAttributes {
    length: Dword,
    security_descriptor: *mut c_void,
    inherit_handle: Bool,
}

#[repr(C)]
struct SockaddrUn {
    family: u16,
    path: [u8; 108],
}

/// `ACL`: a revision, a reserved byte, the total size, the ACE count and a reserved word. Eight
/// bytes, and only its size is ever needed here -- `InitializeAcl` fills the rest.
#[repr(C)]
struct AclHeader {
    _revision: u8,
    _reserved1: u8,
    _size: u16,
    _ace_count: u16,
    _reserved2: u16,
}

#[cfg(test)]
#[repr(C)]
struct AclSizeInformation {
    ace_count: Dword,
    acl_bytes_in_use: Dword,
    acl_bytes_free: Dword,
}

#[repr(C)]
struct AceHeader {
    kind: u8,
    _flags: u8,
    _size: u16,
}

#[repr(C)]
struct AccessAllowedAce {
    header: AceHeader,
    mask: Dword,
    sid_start: Dword,
}

/// `SECURITY_DESCRIPTOR` is opaque and 20 bytes on x86-64; `InitializeSecurityDescriptor` fills it.
/// Declared as aligned words so the pointer handed to Win32 is DWORD-aligned, which it requires.
#[repr(C)]
struct SecurityDescriptor([u32; 10]);

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentProcess() -> Handle;
    fn CreatePipe(
        read: *mut Handle,
        write: *mut Handle,
        attributes: *const SecurityAttributes,
        size: Dword,
    ) -> Bool;
    fn DuplicateHandle(
        source_process: Handle,
        source: Handle,
        target_process: Handle,
        target: *mut Handle,
        access: Dword,
        inherit: Bool,
        options: Dword,
    ) -> Bool;
    fn CreateFileW(
        name: *const u16,
        access: Dword,
        share: Dword,
        attributes: *const SecurityAttributes,
        disposition: Dword,
        flags: Dword,
        template: Handle,
    ) -> Handle;
}

#[link(name = "advapi32")]
unsafe extern "system" {
    fn OpenProcessToken(process: Handle, access: Dword, token: *mut Handle) -> Bool;
    fn GetTokenInformation(
        token: Handle,
        class: i32,
        information: *mut c_void,
        length: Dword,
        written: *mut Dword,
    ) -> Bool;
    fn GetLengthSid(sid: *const c_void) -> Dword;
    fn CopySid(length: Dword, destination: *mut c_void, source: *const c_void) -> Bool;
    fn CreateWellKnownSid(
        kind: i32,
        domain: *const c_void,
        sid: *mut c_void,
        length: *mut Dword,
    ) -> Bool;
    fn InitializeAcl(acl: *mut c_void, length: Dword, revision: Dword) -> Bool;
    fn AddAccessAllowedAce(
        acl: *mut c_void,
        revision: Dword,
        mask: Dword,
        sid: *const c_void,
    ) -> Bool;
    fn InitializeSecurityDescriptor(descriptor: *mut c_void, revision: Dword) -> Bool;
    fn SetSecurityDescriptorDacl(
        descriptor: *mut c_void,
        present: Bool,
        acl: *mut c_void,
        defaulted: Bool,
    ) -> Bool;
    fn SetSecurityInfo(
        object: Handle,
        object_type: i32,
        information: Dword,
        owner: *const c_void,
        group: *const c_void,
        dacl: *const c_void,
        sacl: *const c_void,
    ) -> Dword;
    fn SetNamedSecurityInfoW(
        name: *mut u16,
        object_type: i32,
        information: Dword,
        owner: *const c_void,
        group: *const c_void,
        dacl: *const c_void,
        sacl: *const c_void,
    ) -> Dword;
}

// The read-back half, reached only by `is_owner_only` and therefore only by the tests that check
// the private-file guarantee.
#[cfg(test)]
#[link(name = "advapi32")]
unsafe extern "system" {
    fn EqualSid(left: *const c_void, right: *const c_void) -> Bool;
    fn GetAclInformation(
        acl: *const c_void,
        information: *mut c_void,
        length: Dword,
        class: i32,
    ) -> Bool;
    fn GetAce(acl: *const c_void, index: Dword, ace: *mut *mut c_void) -> Bool;
    fn GetNamedSecurityInfoW(
        name: *const u16,
        object_type: i32,
        information: Dword,
        owner: *mut *mut c_void,
        group: *mut *mut c_void,
        dacl: *mut *mut c_void,
        sacl: *mut *mut c_void,
        descriptor: *mut *mut c_void,
    ) -> Dword;
}

#[cfg(test)]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LocalFree(memory: *mut c_void) -> *mut c_void;
}

#[link(name = "bcrypt")]
unsafe extern "system" {
    fn BCryptGenRandom(algorithm: *mut c_void, buffer: *mut u8, length: u32, flags: u32) -> i32;
}

#[link(name = "ws2_32")]
unsafe extern "system" {
    fn WSAStartup(version: u16, data: *mut u8) -> i32;
    fn WSAGetLastError() -> i32;
    fn WSASocketW(
        family: i32,
        kind: i32,
        protocol: i32,
        protocol_info: *mut c_void,
        group: u32,
        flags: u32,
    ) -> Socket;
    fn bind(socket: Socket, address: *const SockaddrUn, length: i32) -> i32;
    fn listen(socket: Socket, backlog: i32) -> i32;
    fn connect(socket: Socket, address: *const SockaddrUn, length: i32) -> i32;
    fn accept(socket: Socket, address: *mut SockaddrUn, length: *mut i32) -> Socket;
    fn recv(socket: Socket, buffer: *mut u8, length: i32, flags: i32) -> i32;
    fn send(socket: Socket, buffer: *const u8, length: i32, flags: i32) -> i32;
    fn setsockopt(socket: Socket, level: i32, name: i32, value: *const u8, length: i32) -> i32;
    fn closesocket(socket: Socket) -> i32;
}

/// The last Win32 error, in the code space `std::io::Error` already understands.
fn last_error() -> io::Error {
    io::Error::last_os_error()
}

/// The last Winsock error. `WSAGetLastError` returns codes from the same space as `GetLastError`,
/// so `WSAETIMEDOUT` still classifies as [`io::ErrorKind::TimedOut`] and the provider transport's
/// existing error mapping keeps working unchanged.
fn last_socket_error() -> io::Error {
    io::Error::from_raw_os_error(unsafe { WSAGetLastError() })
}

fn wide(path: &Path) -> Vec<u16> {
    path.as_os_str().encode_wide().chain(Some(0)).collect()
}

// --- descriptors -------------------------------------------------------------------------------

/// A kernel object this process owns and closes on drop.
///
/// Both descriptors it ever holds — the checkpoint broker's child end and the trigger section — are
/// created by the C engine, which has no Windows backend yet. The type exists so the Rust side is
/// ready and so `ffi.rs` needs no `#[cfg]`.
#[derive(Debug)]
pub(crate) struct OwnedDescriptor(OwnedHandle);

impl OwnedDescriptor {
    /// Adopts a descriptor the engine installed into this process.
    ///
    /// # Safety
    /// `raw` must be an open kernel handle owned by this process and not owned by anything else.
    pub(crate) unsafe fn adopt(raw: RawDescriptor) -> Self {
        // SAFETY: forwarded to the caller by this function's own contract.
        Self(unsafe { OwnedHandle::from_raw_handle(as_handle(raw)) })
    }

    pub(crate) fn raw(&self) -> RawDescriptor {
        as_raw(self.0.as_raw_handle())
    }
}

// --- the local byte stream ---------------------------------------------------------------------

/// A reliable, ordered, bidirectional local byte stream: `AF_UNIX` + `SOCK_STREAM`.
///
/// Windows has had `AF_UNIX` since 1803 and `std` does not expose it, so this is spoken to `ws2_32`
/// directly. The two rejected alternatives are worth recording, because both are cheaper:
///
/// * **Loopback TCP** would give `std::net::TcpStream` for free — timeouts, `try_clone`, one
///   implementation for both hosts. It is rejected because this channel grants provider authority
///   and carries checkpoint image bytes: it must be scoped by filesystem permissions, not reachable
///   from the network stack. Any local process can connect during the accept window, and the
///   endpoint is visible in `netstat`.
/// * **Named pipes** have no read timeout on a synchronous handle. Getting one means overlapped I/O
///   plus `CancelIoEx`, or the deprecated `PIPE_NOWAIT`. `SO_RCVTIMEO` works here directly.
///
/// Only the constructor forks. The framing, the protocol codecs and the digest logic above this type
/// are byte-identical on both hosts.
///
/// `pub` because `transport::Channel::from_stream` is public API and names it. Opaque: every method
/// below is crate-private, and there is no public conversion into it yet — a Windows host backend
/// that wants to hand one in will need `From<OwnedSocket>`, which is a decision for whoever writes
/// it rather than one to guess here.
#[derive(Debug)]
pub struct Stream(Socket);

impl Stream {
    /// A second handle on the same stream, so a reader and a writer can hold one each.
    ///
    /// A Winsock `SOCKET` is an ordinary kernel handle, so `DuplicateHandle` duplicates it; this is
    /// what `std` does for `TcpStream::try_clone`. `WSADuplicateSocket` is for handing a socket to
    /// *another* process and is not what is wanted here.
    pub(crate) fn try_clone(&self) -> io::Result<Self> {
        let mut duplicate: Handle = std::ptr::null_mut();
        // SAFETY: both process handles are the current-process pseudo-handle, and `self.0` is an
        // open socket this value owns.
        let ok = unsafe {
            DuplicateHandle(
                GetCurrentProcess(),
                self.0 as Handle,
                GetCurrentProcess(),
                &raw mut duplicate,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS,
            )
        };
        if ok == FALSE {
            return Err(last_error());
        }
        Ok(Self(duplicate as Socket))
    }

    /// Bounds a blocking read. `None` restores an unbounded one.
    pub(crate) fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.set_timeout(SO_RCVTIMEO, timeout)
    }

    /// Bounds a blocking write. `None` restores an unbounded one.
    pub(crate) fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.set_timeout(SO_SNDTIMEO, timeout)
    }

    fn set_timeout(&self, option: i32, timeout: Option<Duration>) -> io::Result<()> {
        let milliseconds: Dword = match timeout {
            // Zero means "no timeout" to Winsock and is an error on Unix. Keep the Unix answer, or a
            // caller that asks for an already-expired deadline blocks forever instead of failing.
            Some(timeout) if timeout.is_zero() => {
                return Err(io::Error::from_raw_os_error(WSAEINVAL))
            }
            // Round up: a sub-millisecond timeout must not truncate to "wait forever".
            Some(timeout) => Dword::try_from(timeout.as_millis().max(1)).unwrap_or(Dword::MAX),
            None => 0,
        };
        // SAFETY: `self.0` is an open socket and the value is a DWORD of the length declared.
        let status = unsafe {
            setsockopt(
                self.0,
                SOL_SOCKET,
                option,
                std::ptr::from_ref(&milliseconds).cast::<u8>(),
                i32::try_from(size_of::<Dword>()).unwrap_or(4),
            )
        };
        if status == SOCKET_ERROR {
            return Err(last_socket_error());
        }
        Ok(())
    }

    /// The raw descriptor, for the duration of the FFI call that reads it.
    pub(crate) fn raw(&self) -> RawDescriptor {
        as_raw(self.0 as RawHandle)
    }
}

const DUPLICATE_SAME_ACCESS: Dword = 0x0000_0002;

impl io::Read for Stream {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        let capacity = i32::try_from(bytes.len()).unwrap_or(i32::MAX);
        // SAFETY: `self.0` is an open socket and the buffer is valid for `capacity` bytes.
        let read = unsafe { recv(self.0, bytes.as_mut_ptr(), capacity, 0) };
        if read == SOCKET_ERROR {
            return Err(last_socket_error());
        }
        Ok(usize::try_from(read).unwrap_or(0))
    }
}

impl io::Write for Stream {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let length = i32::try_from(bytes.len()).unwrap_or(i32::MAX);
        // SAFETY: `self.0` is an open socket and the buffer is valid for `length` bytes.
        let written = unsafe { send(self.0, bytes.as_ptr(), length, 0) };
        if written == SOCKET_ERROR {
            return Err(last_socket_error());
        }
        Ok(usize::try_from(written).unwrap_or(0))
    }

    fn flush(&mut self) -> io::Result<()> {
        // A stream socket has no user-space buffer to flush.
        Ok(())
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        if self.0 != INVALID_SOCKET {
            // SAFETY: exclusive ownership of a socket this type created.
            unsafe { closesocket(self.0) };
        }
    }
}

/// Initialises Winsock exactly once. `std` does this for its own sockets and does not expose it.
fn winsock() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let mut data = [0_u8; 512];
        // SAFETY: WSADATA is 400 bytes on x86-64; the buffer is larger and writable.
        unsafe { WSAStartup(0x0202, data.as_mut_ptr()) };
    });
}

fn socket() -> io::Result<Stream> {
    winsock();
    // Winsock creates sockets inheritable by default: reading GetHandleInformation back on a fresh
    // socket that nobody has marked returns HANDLE_FLAG_INHERIT. That inverts the Unix hygiene
    // problem -- the default leaks every socket in the process into any child created with
    // bInheritHandles -- so ask for the opposite up front. This is the Windows counterpart of the
    // FD_CLOEXEC the Unix arm sets on its pipes, and it is needed for the same reason: an engine
    // process that finds a descriptor it cannot account for refuses to checkpoint at all.
    //
    // WSA_FLAG_OVERLAPPED is not optional and its absence is not a performance question. A socket
    // created without it is a non-overlapped socket, and Winsock does not honour SO_RCVTIMEO or
    // SO_SNDTIMEO on one: a bounded `receive` blocks forever instead of returning WSAETIMEDOUT.
    // `socket()` sets the flag implicitly, which is why the plain-BSD spelling appears to work and
    // this one has to ask. The provider transport's every read and write is deadline-bounded, so
    // this flag is what makes those deadlines real.
    // SAFETY: no protocol-info structure is supplied, so all pointer arguments are null.
    let raw = unsafe {
        WSASocketW(
            AF_UNIX,
            SOCK_STREAM,
            0,
            std::ptr::null_mut(),
            0,
            WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT,
        )
    };
    if raw == INVALID_SOCKET {
        return Err(last_socket_error());
    }
    Ok(Stream(raw))
}

/// The rendezvous address for one [`stream_pair`] call.
fn rendezvous() -> io::Result<(std::path::PathBuf, SockaddrUn)> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static UNIQUE: AtomicU64 = AtomicU64::new(0);
    let path = std::env::temp_dir().join(format!(
        "hl-engine-pair-{}-{}",
        std::process::id(),
        UNIQUE.fetch_add(1, Ordering::Relaxed)
    ));
    let bytes = os_bytes(path.as_os_str()).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "the host temporary directory is not valid UTF-8 and cannot name an AF_UNIX endpoint",
        )
    })?;
    let mut address = SockaddrUn {
        family: u16::try_from(AF_UNIX).unwrap_or(1),
        path: [0; 108],
    };
    if bytes.len() >= address.path.len() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "the host temporary directory exceeds the AF_UNIX address length",
        ));
    }
    address.path[..bytes.len()].copy_from_slice(bytes);
    Ok((path, address))
}

/// A connected pair, created before process creation so neither side discovers an ambient
/// descriptor.
///
/// Winsock has no `socketpair()`, so a pair is bind, listen, connect, accept over a real filesystem
/// path, and a real file appears there for the duration. The path is removed once both ends exist; a
/// crash in between leaves a stale zero-byte file, which the `hl-projection-*` and
/// `hl-engine-config-*` temporaries in this crate already have to tolerate.
pub(crate) fn stream_pair() -> io::Result<(Stream, Stream)> {
    let (path, address) = rendezvous()?;
    let _ = std::fs::remove_file(&path);
    let listener = socket()?;
    let length = i32::try_from(size_of::<SockaddrUn>()).unwrap_or(110);
    // SAFETY: `listener.0` is an open socket and the address is a live, fully initialised sockaddr.
    if unsafe { bind(listener.0, &raw const address, length) } == SOCKET_ERROR
        // SAFETY: as above.
        || unsafe { listen(listener.0, 1) } == SOCKET_ERROR
    {
        let error = last_socket_error();
        let _ = std::fs::remove_file(&path);
        return Err(error);
    }
    let client = socket()?;
    // A backlog of one is enough: `connect` completes against the listen queue without an
    // `accept`, so this does not deadlock on a single thread.
    // SAFETY: as above.
    let connected = unsafe { connect(client.0, &raw const address, length) };
    let mut peer = SockaddrUn {
        family: 0,
        path: [0; 108],
    };
    let mut peer_length = length;
    // SAFETY: the listener is open and both out-parameters are live and writable.
    let accepted = unsafe { accept(listener.0, &raw mut peer, &raw mut peer_length) };
    // The endpoint has served its purpose the moment both ends exist. Removing it here rather than
    // on drop keeps the window in which another local process could reach it as short as the
    // connect itself.
    let _ = std::fs::remove_file(&path);
    if connected == SOCKET_ERROR || accepted == INVALID_SOCKET {
        return Err(last_socket_error());
    }
    Ok((client, Stream(accepted)))
}

/// Adopts the accepted checkpoint channel the engine handed back.
///
/// # Safety
/// `raw` must be an open stream socket owned by this process and not owned by anything else.
pub(crate) unsafe fn adopt_stream(raw: RawDescriptor) -> Stream {
    winsock();
    Stream(as_handle(raw) as Socket)
}

// --- files -------------------------------------------------------------------------------------

/// An anonymous unidirectional pipe, inherited by no child unless one is asked for explicitly.
///
/// `bInheritHandle = FALSE` is the counterpart of the Unix arm's `FD_CLOEXEC`, and it matters for
/// the same reason: an engine process that finds a descriptor it cannot account for refuses to
/// checkpoint at all.
pub(crate) fn pipe_pair() -> io::Result<(File, File)> {
    let attributes = SecurityAttributes {
        length: Dword::try_from(size_of::<SecurityAttributes>()).unwrap_or(24),
        security_descriptor: std::ptr::null_mut(),
        inherit_handle: FALSE,
    };
    let mut read: Handle = std::ptr::null_mut();
    let mut write: Handle = std::ptr::null_mut();
    // SAFETY: both out-parameters are live, and the attributes outlive the call.
    let ok = unsafe { CreatePipe(&raw mut read, &raw mut write, &raw const attributes, 0) };
    if ok == FALSE {
        return Err(last_error());
    }
    // SAFETY: both handles were just created by CreatePipe and are owned by this process.
    unsafe { Ok((File::from_raw_handle(read), File::from_raw_handle(write))) }
}

/// Adopts a file the engine created, such as the terminal master.
///
/// # Safety
/// `raw` must be an open file handle owned by this process and not owned by anything else.
pub(crate) unsafe fn adopt_file(raw: RawDescriptor) -> File {
    // SAFETY: forwarded to the caller by this function's own contract.
    unsafe { File::from_raw_handle(as_handle(raw)) }
}

/// The raw descriptor for a file, for the duration of the FFI call that reads it.
pub(crate) fn file_raw(file: &File) -> RawDescriptor {
    as_raw(file.as_raw_handle())
}

/// The host's discard stream, opened for the direction the guest will use it in.
pub(crate) fn null_stdio(read: bool) -> io::Result<File> {
    OpenOptions::new().read(read).write(!read).open("NUL")
}

/// Fills `bytes` from the host's cryptographic entropy source.
pub(crate) fn secure_random(bytes: &mut [u8]) -> io::Result<()> {
    let length = u32::try_from(bytes.len())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "entropy request is too large"))?;
    // SAFETY: the buffer is valid for `length` bytes; the system-preferred RNG needs no algorithm
    // handle, which is what the flag selects.
    let status = unsafe {
        BCryptGenRandom(
            std::ptr::null_mut(),
            bytes.as_mut_ptr(),
            length,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG,
        )
    };
    if status < 0 {
        return Err(io::Error::other(format!(
            "BCryptGenRandom failed with NTSTATUS {status:#x}"
        )));
    }
    Ok(())
}

#[path = "windows/security.rs"]
mod security;

#[cfg(test)]
pub(crate) use security::is_owner_only;
pub(crate) use security::{create_private_file, set_mode};

// --- the rest ----------------------------------------------------------------------------------

/// Creates a symbolic link at `link` pointing at `target`.
///
/// Windows needs either Developer Mode or `SeCreateSymbolicLinkPrivilege` to create one, and the
/// bare failure is `ERROR_PRIVILEGE_NOT_HELD`, which names nothing a caller can act on. Under
/// "compiles like any other rust crate" a projection containing a symlink has to fail *diagnosably*.
pub(crate) fn symlink(target: &Path, link: &Path) -> io::Result<()> {
    // `symlink_dir` and `symlink_file` are distinct calls on Windows and the flag is baked into the
    // reparse point, so the kind has to be decided here. The target is resolved the way the guest
    // will resolve it: relative to the directory the link sits in.
    let resolved = link
        .parent()
        .map_or_else(|| target.to_path_buf(), |parent| parent.join(target));
    let result = if resolved.is_dir() {
        std::os::windows::fs::symlink_dir(target, link)
    } else {
        std::os::windows::fs::symlink_file(target, link)
    };
    result.map_err(|error| {
        if error.raw_os_error() == Some(ERROR_PRIVILEGE_NOT_HELD) {
            return io::Error::new(
                io::ErrorKind::PermissionDenied,
                "creating a symbolic link needs Developer Mode (Settings > System > For \
                 developers) or SeCreateSymbolicLinkPrivilege; a projected namespace containing a \
                 symlink cannot be materialized without one",
            );
        }
        error
    })
}

/// Whether the metadata names a Unix domain socket.
///
/// Always `false`: no Windows file has that type, so a launch that projects a host socket is
/// refused rather than accepted with the check quietly disabled.
pub(crate) fn is_socket(_metadata: &Metadata) -> bool {
    false
}

/// Delivers a host signal to one host process.
///
/// Unimplemented, and deliberately not emulated. There is no `kill(2)`; `TerminateProcess` is not
/// `SIGTERM` and `GenerateConsoleCtrlEvent` reaches a console group rather than a process. The
/// engine already owns this operation on its own side (`hl_activation_kill`,
/// `hl_engine_request(HL_ENGINE_REQUEST_SIGNAL)`), and routing every host through that would be a
/// portability fix that improves the Unix build too: sending a raw host signal from Rust bypasses
/// an abstraction the C API already provides. That change needs the C side and has not been made.
pub(crate) fn signal_process(_process: u64, _signal: i32) -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "delivering a host signal to an engine process is not implemented on Windows; it must be \
         routed through the engine rather than emulated",
    ))
}

/// The bytes a host string contributes to the guest's launch configuration.
///
/// These bytes become the Linux guest's argv, environment records, mount paths and file-owner keys.
/// A Linux path is an arbitrary byte string; a Windows `OsStr` is WTF-16, potentially *ill-formed*
/// UTF-16, and there is no total lossless map between the two that also agrees with the Unix
/// behaviour on ASCII. So the crate picks a policy: **UTF-8, and reject what will not encode.**
///
/// `to_str` succeeds exactly when the WTF-16 is well-formed UTF-16, which makes this lossless for
/// every string a user can type and byte-identical to the Unix arm for all valid UTF-8.
///
/// `to_string_lossy` is the tempting alternative and is wrong: it substitutes U+FFFD and hands the
/// guest a path that is not the path the caller named. A rejection is a known unknown; a substituted
/// path is a bug in a guest that nobody will trace back to this line.
pub(crate) fn os_bytes(value: &OsStr) -> Option<&[u8]> {
    value.to_str().map(str::as_bytes)
}

/// The host string for bytes received from the engine. The inverse of [`os_bytes`].
///
/// This one is on the *receive* path — `decode_namespace_install` reads it from the engine, not
/// just from this crate's own output — so it needs a total answer, and "a projected path that is not
/// valid UTF-8 is a protocol error" is it.
pub(crate) fn os_string(bytes: Vec<u8>) -> Option<OsString> {
    String::from_utf8(bytes).ok().map(OsString::from)
}
