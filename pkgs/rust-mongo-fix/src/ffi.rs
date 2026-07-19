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
    pub(crate) fn hl_terminal_resize(master: i32, size: TerminalSize) -> i32;
    pub(crate) fn hl_activation_wait(process: *mut Process, exit: *mut EngineExit) -> i32;
    pub(crate) fn hl_activation_try_wait(
        process: *mut Process,
        ready: *mut u32,
        exit: *mut EngineExit,
    ) -> i32;
    pub(crate) fn hl_activation_kill(process: *mut Process) -> i32;
    pub(crate) fn hl_activation_domain_terminate(domain: ProcessDomain) -> i32;
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
