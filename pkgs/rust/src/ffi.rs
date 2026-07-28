#![allow(unsafe_code)]
use std::{
    ffi::{c_char, c_int, c_void},
    fs::File,
    os::fd::{AsRawFd, FromRawFd},
};

#[repr(C)]
pub(crate) struct Process {
    _private: [u8; 0],
}

impl Process {
    pub(crate) fn signal(process: u64, signal: i32) -> std::io::Result<()> {
        let process =
            c_int::try_from(process).map_err(|_| std::io::Error::from_raw_os_error(22))?;
        if unsafe { process_signal(process, signal) } == 0 {
            Ok(())
        } else {
            Err(std::io::Error::last_os_error())
        }
    }
}
#[derive(Debug)]
pub(crate) struct Handle(*mut Process);
// SAFETY: activation processes have no thread affinity. The safe wrapper never
// exposes the pointer, provides no concurrent access, and destroys it exactly once.
unsafe impl Send for Handle {}

impl Handle {
    pub(crate) fn wait(&self) -> Result<EngineExit, i32> {
        let mut exit = EngineExit::default();
        let status = unsafe { hl_activation_wait(self.0, &mut exit) };
        if status == 0 {
            Ok(exit)
        } else {
            Err(status)
        }
    }

    pub(crate) fn try_wait(&self) -> Result<Option<EngineExit>, i32> {
        let mut ready = 0;
        let mut exit = EngineExit::default();
        let status = unsafe { hl_activation_try_wait(self.0, &mut ready, &mut exit) };
        if status != 0 {
            Err(status)
        } else if ready == 0 {
            Ok(None)
        } else {
            Ok(Some(exit))
        }
    }

    pub(crate) fn kill(&self) -> Result<(), i32> {
        let status = unsafe { hl_activation_kill(self.0) };
        if status == 0 {
            Ok(())
        } else {
            Err(status)
        }
    }

    pub(crate) fn id(&self) -> Result<u64, i32> {
        let mut id = 0;
        let status = unsafe { hl_activation_process_id(self.0, &mut id) };
        if status == 0 {
            Ok(id)
        } else {
            Err(status)
        }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        unsafe { hl_activation_process_destroy(self.0) }
    }
}
#[repr(C)]
pub(crate) struct Streams {
    pub input: i32,
    pub output: i32,
    pub error: i32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct TerminalSize {
    pub rows: u16,
    pub columns: u16,
}

impl TerminalSize {
    pub(crate) fn apply(self, file: &File) -> Result<(), i32> {
        let status = unsafe { hl_terminal_resize(file.as_raw_fd(), self) };
        if status == 0 {
            Ok(())
        } else {
            Err(status)
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ProcessDomain {
    pub identity: [u64; 2],
}

impl ProcessDomain {
    pub(crate) fn terminate(identity: [u64; 2]) -> Result<(), i32> {
        let status = unsafe { hl_activation_domain_terminate(Self { identity }) };
        if status == 0 {
            Ok(())
        } else {
            Err(status)
        }
    }
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(crate) struct ProcessInfo {
    pub host_id: u64,
    pub initial: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct EngineExit {
    pub abi: u32,
    pub size: u32,
    pub kind: u32,
    pub guest_status: i32,
    pub detail: u64,
}
impl Default for EngineExit {
    fn default() -> Self {
        Self {
            abi: 5,
            size: 24,
            kind: 0,
            guest_status: 0,
            detail: 0,
        }
    }
}

unsafe extern "C" {
    pub(crate) fn hl_engine_guest_fd_limit() -> u32;
    pub(crate) fn hl_activation_start_with_channels(
        executable: *const c_char,
        guest: u32,
        config: *const c_char,
        streams: *const Streams,
        size: *const TerminalSize,
        transport: c_int,
        checkpoint: c_int,
        trigger: c_int,
        master: *mut i32,
        process: *mut *mut Process,
    ) -> i32;
    pub(crate) fn hl_ckpt_broker_pair(parent: *mut c_int, child: *mut c_int) -> c_int;
    pub(crate) fn hl_ckpt_broker_accept(
        broker: c_int,
        timeout_ms: c_int,
        host_pid: *mut u64,
    ) -> c_int;
    pub(crate) fn hl_ckpt_trigger_create(
        descriptor: *mut c_int,
        mapping: *mut *mut c_void,
    ) -> c_int;
    pub(crate) fn hl_ckpt_trigger_bump(mapping: *mut c_void) -> u32;
    pub(crate) fn hl_ckpt_trigger_destroy(mapping: *mut c_void, descriptor: c_int);
    pub(crate) fn hl_terminal_resize(master: i32, size: TerminalSize) -> i32;
    pub(crate) fn hl_activation_wait(process: *mut Process, exit: *mut EngineExit) -> i32;
    pub(crate) fn hl_activation_try_wait(
        process: *mut Process,
        ready: *mut u32,
        exit: *mut EngineExit,
    ) -> i32;
    pub(crate) fn hl_activation_kill(process: *mut Process) -> i32;
    pub(crate) fn hl_activation_domain_terminate(domain: ProcessDomain) -> i32;
    pub(crate) fn hl_activation_domain_processes(
        domain: ProcessDomain,
        initial_process_id: u64,
        processes: *mut ProcessInfo,
        capacity: u32,
        count: *mut u32,
    ) -> i32;
    pub(crate) fn hl_activation_process_destroy(process: *mut Process);
    pub(crate) fn hl_activation_process_id(process: *const Process, id: *mut u64) -> i32;
    fn pipe(descriptors: *mut c_int) -> c_int;
    fn fcntl(descriptor: c_int, command: c_int, ...) -> c_int;
    #[link_name = "kill"]
    fn process_signal(process: c_int, signal: c_int) -> c_int;
    #[cfg(target_os = "linux")]
    fn __libc_current_sigrtmin() -> c_int;
}

/// The host signal the engine reserves to bounce a process out of a blocking host syscall to its next
/// dispatcher safepoint (where checkpoint capture runs).
///
/// It MUST be the engine's own `THREAD_INT_SIG` (`src/linux_abi/thread.c`), the only signal for which every
/// engine process installs a permanent, guest-unreachable handler. Any guest-reachable signal is useless
/// here: the engine installs a host handler for one only when the guest itself calls `rt_sigaction` on it,
/// so kicking with (say) `SIGURG` is a silent no-op against a shell that never handles `SIGURG` -- its
/// default action is *ignore*, which does not interrupt a blocked `read`. That is exactly how an
/// interactive `bash` parked on its PTY was left unreachable, and capture then never began at all.
pub(crate) fn interrupt_signal() -> c_int {
    // Linux: SIGRTMIN + 7, matching native_compat.h's SIGINFO alias. macOS: SIGINFO(29), which sig_l2m omits.
    #[cfg(target_os = "linux")]
    {
        unsafe { __libc_current_sigrtmin() + 7 }
    }
    #[cfg(target_os = "macos")]
    {
        29
    }
}

pub(crate) fn guest_fd_limit() -> u32 {
    // SAFETY: this query reads the current process resource limit and has no pointer arguments or side effects.
    unsafe { hl_engine_guest_fd_limit() }
}
pub(crate) fn domain_processes(
    identity: [u64; 2],
    initial_process_id: u64,
    maximum: u32,
) -> Result<Vec<ProcessInfo>, i32> {
    let mut count = 0;
    let status = unsafe {
        hl_activation_domain_processes(
            ProcessDomain { identity },
            initial_process_id,
            std::ptr::null_mut(),
            0,
            &mut count,
        )
    };
    if status != 0 && status != 5 {
        return Err(status);
    }
    if count > maximum {
        return Err(5);
    }
    for _ in 0..4 {
        let capacity = count;
        let mut processes = vec![ProcessInfo::default(); capacity as usize];
        let status = unsafe {
            hl_activation_domain_processes(
                ProcessDomain { identity },
                initial_process_id,
                processes.as_mut_ptr(),
                capacity,
                &mut count,
            )
        };
        if status == 0 {
            processes.truncate(count as usize);
            return Ok(processes);
        }
        if status != 5 || count > maximum {
            return Err(status);
        }
    }
    Err(5)
}
pub(crate) fn pipe_pair() -> std::io::Result<(File, File)> {
    const F_SETFD: c_int = 2;
    const FD_CLOEXEC: c_int = 1;
    let mut descriptors = [-1, -1];
    if unsafe { pipe(descriptors.as_mut_ptr()) } != 0 {
        return Err(std::io::Error::last_os_error());
    }
    let read = unsafe { File::from_raw_fd(descriptors[0]) };
    let write = unsafe { File::from_raw_fd(descriptors[1]) };
    if unsafe { fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) } != 0
        || unsafe { fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) } != 0
    {
        return Err(std::io::Error::last_os_error());
    }
    Ok((read, write))
}

const _: () = assert!(std::mem::size_of::<EngineExit>() == 24);
const _: () = assert!(std::mem::size_of::<*mut c_void>() == std::mem::size_of::<usize>());

// --- checkpoint streaming transport -------------------------------------------------------------
//
// The engine side of this transport lives entirely in C (SCM_RIGHTS, an anonymous shared page). These
// wrappers turn the raw descriptors into owned Rust types at the single unsafe boundary; everything above
// them -- the protocol codec, the demultiplexing server, the embedder's trait -- is safe code.

#[derive(Debug)]
pub(crate) struct Broker(std::os::unix::net::UnixDatagram);

impl Broker {
    pub(crate) fn pair() -> std::io::Result<(Self, OwnedDescriptor)> {
        let mut parent = -1;
        let mut child = -1;
        if unsafe { hl_ckpt_broker_pair(&mut parent, &mut child) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        let parent = unsafe { std::os::unix::net::UnixDatagram::from_raw_fd(parent) };
        Ok((Self(parent), OwnedDescriptor(child)))
    }

    pub(crate) fn accept(
        &self,
        timeout: std::time::Duration,
    ) -> Option<(std::os::unix::net::UnixStream, u64)> {
        let milliseconds = c_int::try_from(timeout.as_millis()).unwrap_or(c_int::MAX);
        let mut host_pid = 0_u64;
        let descriptor =
            unsafe { hl_ckpt_broker_accept(self.0.as_raw_fd(), milliseconds, &mut host_pid) };
        if descriptor < 0 {
            return None;
        }
        Some((
            unsafe { std::os::unix::net::UnixStream::from_raw_fd(descriptor) },
            host_pid,
        ))
    }
}

/// A raw descriptor this process owns and closes on drop.
#[derive(Debug)]
pub(crate) struct OwnedDescriptor(c_int);

impl OwnedDescriptor {
    pub(crate) const fn raw(&self) -> c_int {
        self.0
    }
}

impl Drop for OwnedDescriptor {
    fn drop(&mut self) {
        if self.0 >= 0 {
            // SAFETY: exclusive ownership of a descriptor this type created.
            unsafe { close_descriptor(self.0) };
        }
    }
}

unsafe extern "C" {
    #[link_name = "close"]
    fn close_descriptor(descriptor: c_int) -> c_int;
}

/// The shared generation counter used to request a capture.
#[derive(Debug)]
pub(crate) struct Trigger {
    descriptor: c_int,
    mapping: *mut c_void,
}

// SAFETY: the mapping is a single shared word; every access goes through hl_ckpt_trigger_bump, which is a
// plain store, and the handle is never aliased.
unsafe impl Send for Trigger {}
unsafe impl Sync for Trigger {}

impl Trigger {
    pub(crate) fn create() -> std::io::Result<Self> {
        let mut descriptor = -1;
        let mut mapping = std::ptr::null_mut();
        if unsafe { hl_ckpt_trigger_create(&mut descriptor, &mut mapping) } != 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(Self {
            descriptor,
            mapping,
        })
    }

    pub(crate) const fn raw(&self) -> c_int {
        self.descriptor
    }

    /// Advances the generation, which is what every engine process observes at its next safepoint.
    pub(crate) fn bump(&self) -> u32 {
        unsafe { hl_ckpt_trigger_bump(self.mapping) }
    }
}

impl Drop for Trigger {
    fn drop(&mut self) {
        unsafe { hl_ckpt_trigger_destroy(self.mapping, self.descriptor) };
        self.mapping = std::ptr::null_mut();
        self.descriptor = -1;
    }
}

/// Activation with any combination of stdio streams OR a terminal size, a provider transport, a
/// checkpoint broker and a trigger page — all forwarded together through the one combined C entry point.
///
/// Exactly one of `streams` / `size` describes the process I/O: pass `streams` for a plain (stdio)
/// process, or `size` for a PTY. When `size` is set the engine returns the pty master, delivered here as
/// the `Option<File>`. `transport`, `checkpoint` and `trigger` are each `-1` when not requested.
#[allow(clippy::too_many_arguments)] // The one C entry that composes every activation channel.
pub(crate) fn start_combined(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    streams: Option<&Streams>,
    size: Option<TerminalSize>,
    transport: c_int,
    checkpoint: c_int,
    trigger: c_int,
) -> Result<(Handle, Option<File>), i32> {
    let mut process = std::ptr::null_mut();
    let mut master = -1;
    let size_ptr = size.as_ref().map_or(std::ptr::null(), std::ptr::from_ref);
    let streams_ptr = streams.map_or(std::ptr::null(), std::ptr::from_ref);
    let status = unsafe {
        hl_activation_start_with_channels(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            streams_ptr,
            size_ptr,
            transport,
            checkpoint,
            trigger,
            &mut master,
            &mut process,
        )
    };
    if status != 0 || process.is_null() {
        return Err(status);
    }
    // The engine only fills the master when a terminal size was requested; a stdio process leaves it -1.
    let terminal = if size.is_some() {
        if master < 0 {
            return Err(status);
        }
        // SAFETY: the descriptor was just created by the engine and is owned by this process.
        Some(unsafe { File::from_raw_fd(master) })
    } else {
        None
    };
    Ok((Handle(process), terminal))
}
