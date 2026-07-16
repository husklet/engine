use crate::{ffi, Error};
use std::{
    fs::File,
    io::{Read, Write},
};

/// Terminal dimensions measured in character cells.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Size {
    rows: u16,
    columns: u16,
}

impl Size {
    /// Creates a non-empty terminal size.
    ///
    /// # Errors
    /// Returns an error when either dimension is zero.
    pub fn new(rows: u16, columns: u16) -> Result<Self, Error> {
        if rows == 0 || columns == 0 {
            return Err(Error::InvalidConfig("terminal dimensions must be nonzero"));
        }
        Ok(Self { rows, columns })
    }

    #[must_use]
    pub const fn rows(self) -> u16 {
        self.rows
    }

    #[must_use]
    pub const fn columns(self) -> u16 {
        self.columns
    }

    pub(crate) const fn native(self) -> ffi::TerminalSize {
        ffi::TerminalSize {
            rows: self.rows,
            columns: self.columns,
        }
    }
}

/// Owned master side of a guest controlling terminal.
///
/// Standard output and standard error are merged into this byte stream. Dropping
/// the value closes the master without leaking a host descriptor.
#[derive(Debug)]
pub struct Terminal {
    file: File,
}

impl Terminal {
    pub(crate) fn new(file: File) -> Self {
        Self { file }
    }

    /// Changes the terminal window size and notifies the foreground process group.
    ///
    /// # Errors
    /// Returns a platform error when the terminal has already closed.
    pub fn resize(&self, size: Size) -> Result<(), Error> {
        ffi::resize(&self.file, size.native()).map_err(|status| Error::Engine { status, detail: 0 })
    }
}

impl Read for Terminal {
    fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
        match self.file.read(bytes) {
            // Linux PTY masters report EIO after the final slave closes. For
            // stream consumers this is the terminal equivalent of EOF.
            Err(error) if error.raw_os_error() == Some(5) => Ok(0),
            result => result,
        }
    }
}

impl Write for Terminal {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.file.write(bytes)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.file.flush()
    }
}
