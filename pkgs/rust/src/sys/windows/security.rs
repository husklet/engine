use super::{
    c_void, io, last_error, status_error, wide, AccessAllowedAce, AclHeader, AddAccessAllowedAce,
    AsRawHandle, CopySid, CreateFileW, CreateWellKnownSid, Dword, File, FromRawHandle,
    GetCurrentProcess, GetLengthSid, GetTokenInformation, Handle, InitializeAcl,
    InitializeSecurityDescriptor, OpenProcessToken, OwnedHandle, Path, SecurityAttributes,
    SecurityDescriptor, SetNamedSecurityInfoW, SetSecurityDescriptorDacl, SetSecurityInfo,
    ACL_REVISION, CREATE_NEW, DACL_SECURITY_INFORMATION, DELETE, FALSE, FILE_ATTRIBUTE_NORMAL,
    FILE_GENERIC_EXECUTE, FILE_GENERIC_READ, FILE_GENERIC_WRITE, GENERIC_WRITE,
    INVALID_HANDLE_VALUE, PROTECTED_DACL_SECURITY_INFORMATION, SECURITY_DESCRIPTOR_REVISION,
    SECURITY_MAX_SID_SIZE, SE_FILE_OBJECT, TOKEN_QUERY, TOKEN_USER_CLASS, TRUE, WIN_WORLD_SID,
    WRITE_DAC,
};
#[cfg(test)]
use super::{
    AceHeader, AclSizeInformation, EqualSid, GetAce, GetAclInformation, GetNamedSecurityInfoW,
    LocalFree, ACCESS_ALLOWED_ACE_TYPE, ACL_SIZE_INFORMATION,
};

/// A security identifier, kept DWORD-aligned because every Win32 SID entry point requires it.
struct Sid(Vec<u32>);

impl Sid {
    fn pointer(&self) -> *const c_void {
        self.0.as_ptr().cast()
    }

    fn length(&self) -> Dword {
        // SAFETY: the buffer holds a SID this type built or copied.
        unsafe { GetLengthSid(self.pointer()) }
    }
}

fn words(bytes: usize) -> usize {
    bytes.div_ceil(size_of::<u32>()).max(1)
}

/// The SID of the account this process runs as — the "owner" a Linux mode's owner bits describe.
fn token_user() -> io::Result<Sid> {
    let mut token: Handle = std::ptr::null_mut();
    // SAFETY: the current-process pseudo-handle is always valid and the out-parameter is live.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw mut token) } == FALSE {
        return Err(last_error());
    }
    // SAFETY: `token` was just opened; it is closed exactly once, below.
    let token = unsafe { OwnedHandle::from_raw_handle(token) };
    let mut needed: Dword = 0;
    // First call sizes the buffer; it is expected to fail with ERROR_INSUFFICIENT_BUFFER.
    // SAFETY: a null buffer with a zero length is the documented way to ask for the size.
    unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            TOKEN_USER_CLASS,
            std::ptr::null_mut(),
            0,
            &raw mut needed,
        )
    };
    // TOKEN_USER leads with a pointer, so the buffer must be pointer-aligned, not merely
    // DWORD-aligned: a `Vec<u32>` would satisfy Win32's stated requirement and still be a
    // misaligned read here.
    let mut buffer = vec![0_u64; (needed as usize).div_ceil(size_of::<u64>()).max(1)];
    // SAFETY: the buffer is at least `needed` bytes and pointer-aligned, as TOKEN_USER requires.
    let ok = unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            TOKEN_USER_CLASS,
            buffer.as_mut_ptr().cast(),
            needed,
            &raw mut needed,
        )
    };
    if ok == FALSE {
        return Err(last_error());
    }
    // TOKEN_USER is a SID_AND_ATTRIBUTES: a pointer to a SID elsewhere in this same buffer, then a
    // DWORD of attributes. Copy the SID out so its lifetime stops depending on the buffer.
    // SAFETY: the buffer holds a TOKEN_USER whose first field is the SID pointer.
    let sid = unsafe { *buffer.as_ptr().cast::<*const c_void>() };
    // SAFETY: `sid` points into the buffer just filled by GetTokenInformation.
    let length = unsafe { GetLengthSid(sid) };
    let mut copy = vec![0_u32; words(length as usize)];
    // SAFETY: the destination is `length` bytes and DWORD-aligned.
    if unsafe { CopySid(length, copy.as_mut_ptr().cast(), sid) } == FALSE {
        return Err(last_error());
    }
    Ok(Sid(copy))
}

/// `S-1-1-0`, the World. What a Linux mode's group and other bits map onto.
fn everyone() -> io::Result<Sid> {
    let mut buffer = vec![0_u32; words(SECURITY_MAX_SID_SIZE)];
    let mut length = Dword::try_from(SECURITY_MAX_SID_SIZE).unwrap_or(68);
    // SAFETY: the buffer is `length` bytes and DWORD-aligned; the well-known World SID needs no
    // domain SID.
    let ok = unsafe {
        CreateWellKnownSid(
            WIN_WORLD_SID,
            std::ptr::null(),
            buffer.as_mut_ptr().cast(),
            &raw mut length,
        )
    };
    if ok == FALSE {
        return Err(last_error());
    }
    Ok(Sid(buffer))
}

/// A discretionary access control list, DWORD-aligned as `InitializeAcl` requires.
struct Acl(Vec<u32>);

impl Acl {
    /// Builds a DACL granting exactly the listed rights and nothing else.
    ///
    /// An entry with a zero mask is kept rather than dropped: an empty-but-present DACL is how
    /// Windows spells "nobody has access", which is what mode `0` means, and dropping the entry
    /// would produce a *missing* DACL, which means the opposite — everyone has access.
    fn build(entries: &[(&Sid, Dword)]) -> io::Result<Self> {
        // The ACL header, then one ACCESS_ALLOWED_ACE per entry. The ACE's declared size counts a
        // DWORD of SID that the real SID replaces, hence the subtraction.
        let mut bytes = size_of::<AclHeader>();
        for (sid, _) in entries {
            bytes += size_of::<AccessAllowedAce>() - size_of::<Dword>() + sid.length() as usize;
        }
        let mut acl = Self(vec![0_u32; words(bytes)]);
        let length = Dword::try_from(bytes).unwrap_or(Dword::MAX);
        // SAFETY: the buffer is `length` bytes and DWORD-aligned.
        if unsafe { InitializeAcl(acl.pointer(), length, ACL_REVISION) } == FALSE {
            return Err(last_error());
        }
        for (sid, mask) in entries {
            // SAFETY: the ACL was sized to hold every entry, and each SID outlives the call.
            if unsafe { AddAccessAllowedAce(acl.pointer(), ACL_REVISION, *mask, sid.pointer()) }
                == FALSE
            {
                return Err(last_error());
            }
        }
        Ok(acl)
    }

    fn pointer(&mut self) -> *mut c_void {
        self.0.as_mut_ptr().cast()
    }
}

/// The Win32 rights one octal digit of a Linux mode describes.
fn rights(digit: u32) -> Dword {
    let mut mask = 0;
    if digit & 0o4 != 0 {
        mask |= FILE_GENERIC_READ;
    }
    if digit & 0o2 != 0 {
        mask |= FILE_GENERIC_WRITE | DELETE;
    }
    if digit & 0o1 != 0 {
        // FILE_TRAVERSE and FILE_EXECUTE are the same bit; this covers directories too.
        mask |= FILE_GENERIC_EXECUTE;
    }
    mask
}

/// The DACL for a Linux mode: the owner bits for this account, and the group and other bits folded
/// together onto the World.
///
/// Folding group onto World is the conservative direction. A Linux group has no Windows counterpart
/// this crate could name, and the alternative — dropping the group bits — would make a `0o640` file
/// *more* private than the caller asked for, which reads as a success and hides the fact that the
/// group grant was never honoured. Granting World the union makes the loss visible in the ACL.
fn mode_acl(mode: u32) -> io::Result<Acl> {
    let user = token_user()?;
    let shared = ((mode >> 3) | mode) & 0o7;
    if shared == 0 {
        return Acl::build(&[(&user, rights((mode >> 6) & 0o7))]);
    }
    let world = everyone()?;
    Acl::build(&[(&user, rights((mode >> 6) & 0o7)), (&world, rights(shared))])
}

/// Creates a new file no other account can read.
///
/// This is not decoration and the Windows arm is not allowed to be a no-op. `configfile.rs` writes
/// the launch configuration here and `projection.rs` writes the guest's namespace contents, both
/// into the world-writable host temporary directory. On Unix the guarantee is `open(2)`'s `0o600`.
///
/// The DACL is supplied to `CreateFileW` rather than applied afterwards, so the file never exists
/// with the temporary directory's inherited protection. `SetSecurityInfo` then re-states it as a
/// *protected* DACL, which is what strips inheritable ACEs the parent directory would otherwise
/// contribute; it runs on the open handle rather than on the path, so it cannot race with anything
/// and does not need the file to be shareable.
pub(crate) fn create_private_file(path: &Path) -> io::Result<File> {
    let mut acl = mode_acl(0o600)?;
    let mut descriptor = SecurityDescriptor([0; 10]);
    let descriptor_pointer = std::ptr::from_mut(&mut descriptor).cast::<c_void>();
    // SAFETY: the buffer is at least sizeof(SECURITY_DESCRIPTOR) and DWORD-aligned.
    if unsafe { InitializeSecurityDescriptor(descriptor_pointer, SECURITY_DESCRIPTOR_REVISION) }
        == FALSE
    {
        return Err(last_error());
    }
    // SAFETY: the descriptor was just initialised and the ACL outlives the CreateFileW call.
    if unsafe { SetSecurityDescriptorDacl(descriptor_pointer, TRUE, acl.pointer(), FALSE) } == FALSE
    {
        return Err(last_error());
    }
    let attributes = SecurityAttributes {
        length: Dword::try_from(size_of::<SecurityAttributes>()).unwrap_or(24),
        security_descriptor: descriptor_pointer,
        inherit_handle: FALSE,
    };
    let name = wide(path);
    // No sharing: CREATE_NEW plus an exclusive open is the counterpart of `create_new(true)`.
    // SAFETY: `name` is NUL-terminated and the attributes outlive the call.
    let handle = unsafe {
        CreateFileW(
            name.as_ptr(),
            GENERIC_WRITE | WRITE_DAC,
            0,
            &raw const attributes,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            std::ptr::null_mut(),
        )
    };
    if handle == INVALID_HANDLE_VALUE {
        return Err(last_error());
    }
    // SAFETY: the handle was just created by CreateFileW and is owned by this process.
    let file = unsafe { File::from_raw_handle(handle) };
    // SAFETY: the handle is open with WRITE_DAC and the ACL outlives the call.
    let status = unsafe {
        SetSecurityInfo(
            handle,
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            std::ptr::null(),
            std::ptr::null(),
            acl.pointer(),
            std::ptr::null(),
        )
    };
    if status != 0 {
        drop(file);
        let _ = std::fs::remove_file(path);
        return Err(status_error(status));
    }
    Ok(file)
}

/// Applies a Linux mode to a host file.
///
/// The DACL is written *protected*, which discards the inheritable ACEs the containing directory
/// contributed. Without that flag the call would only add rights and could never take one away,
/// which is the failure mode a "set the read-only attribute" implementation also has: the read-only
/// bit says nothing about who may read.
pub(crate) fn set_mode(path: &Path, mode: u32) -> io::Result<()> {
    let mut acl = mode_acl(mode)?;
    let mut name = wide(path);
    // SAFETY: `name` is NUL-terminated and mutable as SetNamedSecurityInfoW requires; the ACL
    // outlives the call.
    let status = unsafe {
        SetNamedSecurityInfoW(
            name.as_mut_ptr(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            std::ptr::null(),
            std::ptr::null(),
            acl.pointer(),
            std::ptr::null(),
        )
    };
    if status != 0 {
        return Err(status_error(status));
    }
    Ok(())
}

/// Whether `path` grants access to its owner and to nobody else.
///
/// Reads the *effective* DACL, so inherited ACEs are included: this cannot be satisfied by a
/// protected DACL that merely looks short. Deliberately strict — any grant to a SID other than this
/// process's user is a "no", including grants to `SYSTEM` and `Administrators`, which a file created
/// in the temporary directory inherits.
#[cfg(test)]
pub(crate) fn is_owner_only(path: &Path) -> io::Result<bool> {
    let user = token_user()?;
    let name = wide(path);
    let mut dacl: *mut c_void = std::ptr::null_mut();
    let mut descriptor: *mut c_void = std::ptr::null_mut();
    // SAFETY: `name` is NUL-terminated; every out-parameter is live; only the DACL is requested.
    let status = unsafe {
        GetNamedSecurityInfoW(
            name.as_ptr(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &raw mut dacl,
            std::ptr::null_mut(),
            &raw mut descriptor,
        )
    };
    if status != 0 {
        return Err(status_error(status));
    }
    let owner_only = (|| {
        if dacl.is_null() {
            // A NULL DACL grants everyone everything. Never owner-only.
            return Ok(false);
        }
        let mut information = AclSizeInformation {
            ace_count: 0,
            acl_bytes_in_use: 0,
            acl_bytes_free: 0,
        };
        let length = Dword::try_from(size_of::<AclSizeInformation>()).unwrap_or(12);
        // SAFETY: `dacl` is a live ACL owned by `descriptor`, and the out-parameter is live.
        let ok = unsafe {
            GetAclInformation(
                dacl,
                std::ptr::from_mut(&mut information).cast(),
                length,
                ACL_SIZE_INFORMATION,
            )
        };
        if ok == FALSE {
            return Err(last_error());
        }
        for index in 0..information.ace_count {
            let mut ace: *mut c_void = std::ptr::null_mut();
            // SAFETY: `index` is below the ACE count just read.
            if unsafe { GetAce(dacl, index, &raw mut ace) } == FALSE {
                return Err(last_error());
            }
            // SAFETY: `ace` points at a live ACE within the DACL.
            let header = unsafe { &*ace.cast::<AceHeader>() };
            if header.kind != ACCESS_ALLOWED_ACE_TYPE {
                // A deny ACE only ever removes access; it cannot make the file less private.
                continue;
            }
            // SAFETY: an ACCESS_ALLOWED_ACE stores its SID inline, starting at `sid_start`.
            let sid = unsafe { std::ptr::addr_of!((*ace.cast::<AccessAllowedAce>()).sid_start) };
            // SAFETY: both SIDs are live and well-formed.
            if unsafe { EqualSid(sid.cast(), user.pointer()) } == FALSE {
                return Ok(false);
            }
        }
        Ok(true)
    })();
    // SAFETY: GetNamedSecurityInfoW allocates the descriptor with LocalAlloc; the DACL points into
    // it and must not be touched after this.
    unsafe { LocalFree(descriptor) };
    owner_only
}
