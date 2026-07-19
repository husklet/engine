use hl_engine_api::extension::ServiceId;

use super::{
    input::{linux, protocol, put_u16, put_u32, put_u64, Input},
    ServiceFailure,
};

#[derive(Clone, Debug, Eq, PartialEq)]

pub struct ServiceProjection {
    pub path: std::path::PathBuf,
    pub service: ServiceId,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
}

/// Encodes one validated namespace installation transaction.
///
/// # Errors
/// Returns a typed failure for conflicts, invalid paths, or exceeded limits.
pub fn encode_namespace_install(
    entries: &[ServiceProjection],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<Vec<u8>, ServiceFailure> {
    validate_projections(entries, maximum_entries, maximum_path)?;
    let mut out = Vec::new();
    put_u32(
        &mut out,
        u32::try_from(entries.len()).map_err(|_| protocol())?,
    );
    for entry in entries {
        use std::os::unix::ffi::OsStrExt;
        let path = entry.path.as_os_str().as_bytes();
        put_u64(&mut out, entry.service.0);
        put_u32(&mut out, entry.mode);
        put_u32(&mut out, entry.uid);
        put_u32(&mut out, entry.gid);
        put_u16(&mut out, u16::try_from(path.len()).map_err(|_| protocol())?);
        out.extend(path);
    }
    Ok(out)
}

/// Decodes and validates one namespace installation transaction.
///
/// # Errors
/// Returns a typed failure for malformed input, conflicts, invalid paths, or exceeded limits.
pub fn decode_namespace_install(
    bytes: &[u8],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<Vec<ServiceProjection>, ServiceFailure> {
    use std::os::unix::ffi::OsStringExt;
    let mut input = Input::new(bytes);
    let count = input.u32()?;
    if count > maximum_entries {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut entries = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let service = ServiceId(input.u64()?);
        let mode = input.u32()?;
        let uid = input.u32()?;
        let gid = input.u32()?;
        let length = u32::from(input.u16()?);
        if length == 0 || length > maximum_path {
            return Err(linux(36, "service projection path exceeds launch bound"));
        }
        let path = std::ffi::OsString::from_vec(input.bytes(length as usize)?.to_vec()).into();
        entries.push(ServiceProjection {
            path,
            service,
            mode,
            uid,
            gid,
        });
    }
    input.finish()?;
    validate_projections(&entries, maximum_entries, maximum_path)?;
    Ok(entries)
}

fn validate_projections(
    entries: &[ServiceProjection],
    maximum_entries: u32,
    maximum_path: u32,
) -> Result<(), ServiceFailure> {
    use std::os::unix::ffi::OsStrExt;
    if entries.len() > maximum_entries as usize {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut paths = std::collections::BTreeSet::new();
    for entry in entries {
        let path = entry.path.as_os_str().as_bytes();
        if entry.service.0 == 0
            || entry.mode & !0o7777 != 0
            || path.is_empty()
            || path == b"/"
            || path.len() > maximum_path as usize
            || path.len() > u16::MAX as usize
            || path.contains(&0)
            || !entry.path.is_absolute()
            || entry.path.components().any(|component| {
                matches!(
                    component,
                    std::path::Component::ParentDir | std::path::Component::CurDir
                )
            })
            || !paths.insert(entry.path.clone())
        {
            return Err(linux(22, "invalid or conflicting service projection"));
        }
    }
    for path in &paths {
        if path
            .ancestors()
            .skip(1)
            .any(|ancestor| ancestor != std::path::Path::new("/") && paths.contains(ancestor))
        {
            return Err(linux(20, "service projection cannot contain descendants"));
        }
    }
    Ok(())
}
