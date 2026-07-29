use super::{
    accept, as_handle, as_raw, bind, closesocket, connect, io, last_error, last_socket_error,
    listen, os_bytes, recv, send, setsockopt, AsRawHandle, CreatePipe, DuplicateHandle, Duration,
    Dword, File, FromRawHandle, GetCurrentProcess, Handle, OpenOptions, OwnedHandle, RawDescriptor,
    RawHandle, SecurityAttributes, SockaddrUn, Socket, WSASocketW, WSAStartup, AF_UNIX, FALSE,
    INVALID_SOCKET, SOCKET_ERROR, SOCK_STREAM, SOL_SOCKET, SO_RCVTIMEO, SO_SNDTIMEO, WSAEINVAL,
    WSA_FLAG_NO_HANDLE_INHERIT, WSA_FLAG_OVERLAPPED,
};

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
