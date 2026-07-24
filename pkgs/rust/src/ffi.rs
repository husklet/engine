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
#[derive(Debug)]
pub(crate) struct Handle(*mut Process);
// SAFETY: activation processes have no thread affinity. The safe wrapper never
// exposes the pointer, provides no concurrent access, and destroys it exactly once.
unsafe impl Send for Handle {}
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
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct ProcessDomain {
    pub identity: [u64; 2],
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
    pub(crate) fn hl_activation_start_with_stdio(
        executable: *const c_char,
        guest: u32,
        config: *const c_char,
        streams: *const Streams,
        process: *mut *mut Process,
    ) -> i32;
    pub(crate) fn hl_activation_start_with_transport(
        executable: *const c_char,
        guest: u32,
        config: *const c_char,
        streams: *const Streams,
        transport: c_int,
        process: *mut *mut Process,
    ) -> i32;
    pub(crate) fn hl_activation_start_terminal(
        executable: *const c_char,
        guest: u32,
        config: *const c_char,
        size: TerminalSize,
        master: *mut i32,
        process: *mut *mut Process,
    ) -> i32;
    pub(crate) fn hl_activation_start_terminal_with_transport(
        executable: *const c_char,
        guest: u32,
        config: *const c_char,
        size: TerminalSize,
        transport: c_int,
        master: *mut i32,
        process: *mut *mut Process,
    ) -> i32;
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
    pub(crate) fn hl_ckpt_trigger_create(descriptor: *mut c_int, mapping: *mut *mut c_void) -> c_int;
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
}

pub(crate) fn guest_fd_limit() -> u32 {
    // SAFETY: this query reads the current process resource limit and has no pointer arguments or side effects.
    unsafe { hl_engine_guest_fd_limit() }
}
pub(crate) fn start_terminal(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    size: TerminalSize,
) -> Result<(Handle, File), i32> {
    let mut process = std::ptr::null_mut();
    let mut master = -1;
    let status = unsafe {
        hl_activation_start_terminal(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            size,
            &mut master,
            &mut process,
        )
    };
    if status == 0 && !process.is_null() && master >= 0 {
        Ok((Handle(process), unsafe { File::from_raw_fd(master) }))
    } else {
        Err(status)
    }
}

pub(crate) fn start_terminal_with_transport(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    size: TerminalSize,
    transport: &std::os::unix::net::UnixStream,
) -> Result<(Handle, File), i32> {
    let mut process = std::ptr::null_mut();
    let mut master = -1;
    let status = unsafe {
        hl_activation_start_terminal_with_transport(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            size,
            transport.as_raw_fd(),
            &mut master,
            &mut process,
        )
    };
    if status == 0 && !process.is_null() && master >= 0 {
        Ok((Handle(process), unsafe { File::from_raw_fd(master) }))
    } else {
        Err(status)
    }
}
pub(crate) fn resize(file: &File, size: TerminalSize) -> Result<(), i32> {
    let status = unsafe { hl_terminal_resize(file.as_raw_fd(), size) };
    if status == 0 {
        Ok(())
    } else {
        Err(status)
    }
}

pub(crate) fn start(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    streams: &Streams,
) -> Result<Handle, i32> {
    let mut process = std::ptr::null_mut();
    let status = unsafe {
        hl_activation_start_with_stdio(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            streams,
            &mut process,
        )
    };
    if status == 0 && !process.is_null() {
        Ok(Handle(process))
    } else {
        Err(status)
    }
}

#[allow(dead_code)] // Kept private until native VFS dispatch makes the capability truthful.
pub(crate) fn start_with_transport(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    streams: &Streams,
    transport: &std::os::unix::net::UnixStream,
) -> Result<Handle, i32> {
    let mut process = std::ptr::null_mut();
    let status = unsafe {
        hl_activation_start_with_transport(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            streams,
            transport.as_raw_fd(),
            &mut process,
        )
    };
    if status == 0 && !process.is_null() {
        Ok(Handle(process))
    } else {
        Err(status)
    }
}
pub(crate) fn wait(process: &Handle) -> Result<EngineExit, i32> {
    let mut exit = EngineExit::default();
    let status = unsafe { hl_activation_wait(process.0, &mut exit) };
    if status == 0 {
        Ok(exit)
    } else {
        Err(status)
    }
}
pub(crate) fn try_wait(process: &Handle) -> Result<Option<EngineExit>, i32> {
    let mut ready = 0;
    let mut exit = EngineExit::default();
    let status = unsafe { hl_activation_try_wait(process.0, &mut ready, &mut exit) };
    if status != 0 {
        Err(status)
    } else if ready == 0 {
        Ok(None)
    } else {
        Ok(Some(exit))
    }
}
pub(crate) fn kill(process: &Handle) -> Result<(), i32> {
    let status = unsafe { hl_activation_kill(process.0) };
    if status == 0 {
        Ok(())
    } else {
        Err(status)
    }
}
pub(crate) fn terminate_domain(identity: [u64; 2]) -> Result<(), i32> {
    let status = unsafe { hl_activation_domain_terminate(ProcessDomain { identity }) };
    if status == 0 {
        Ok(())
    } else {
        Err(status)
    }
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
#[allow(clippy::needless_pass_by_value)] // Consumption enforces exactly-once destruction.
pub(crate) fn destroy(process: Handle) {
    unsafe { hl_activation_process_destroy(process.0) }
}
pub(crate) fn process_id(process: &Handle) -> Result<u64, i32> {
    let mut id = 0;
    let status = unsafe { hl_activation_process_id(process.0, &mut id) };
    if status == 0 {
        Ok(id)
    } else {
        Err(status)
    }
}
pub(crate) fn signal(process: u64, signal: i32) -> std::io::Result<()> {
    let process = c_int::try_from(process).map_err(|_| std::io::Error::from_raw_os_error(22))?;
    if unsafe { process_signal(process, signal) } == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
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

/// The broker socketpair. The parent end is kept by the server; the child end is handed to activation.
pub(crate) fn broker_pair() -> std::io::Result<(std::os::unix::net::UnixDatagram, OwnedDescriptor)> {
    let mut parent = -1;
    let mut child = -1;
    if unsafe { hl_ckpt_broker_pair(&mut parent, &mut child) } != 0 {
        return Err(std::io::Error::last_os_error());
    }
    // SAFETY: both descriptors were just created by socketpair(2) and are owned by this process.
    let parent = unsafe { std::os::unix::net::UnixDatagram::from_raw_fd(parent) };
    Ok((parent, OwnedDescriptor(child)))
}

/// Waits for one engine process to announce itself and returns its private channel.
pub(crate) fn broker_accept(
    broker: &std::os::unix::net::UnixDatagram,
    timeout: std::time::Duration,
) -> Option<std::os::unix::net::UnixStream> {
    let milliseconds = c_int::try_from(timeout.as_millis()).unwrap_or(c_int::MAX);
    let mut host_pid = 0_u64;
    let descriptor =
        unsafe { hl_ckpt_broker_accept(broker.as_raw_fd(), milliseconds, &mut host_pid) };
    if descriptor < 0 {
        return None;
    }
    // SAFETY: the descriptor was installed into this process by recvmsg and is owned by it.
    Some(unsafe { std::os::unix::net::UnixStream::from_raw_fd(descriptor) })
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

/// Activation with any combination of a provider transport, a checkpoint broker and a trigger page.
pub(crate) fn start_with_channels(
    executable: &std::ffi::CStr,
    guest: u32,
    config: &std::ffi::CStr,
    streams: &Streams,
    checkpoint: c_int,
    trigger: c_int,
) -> Result<Handle, i32> {
    let mut process = std::ptr::null_mut();
    let status = unsafe {
        hl_activation_start_with_channels(
            executable.as_ptr(),
            guest,
            config.as_ptr(),
            streams,
            std::ptr::null(),
            -1,
            checkpoint,
            trigger,
            std::ptr::null_mut(),
            &mut process,
        )
    };
    if status == 0 && !process.is_null() {
        Ok(Handle(process))
    } else {
        Err(status)
    }
}
