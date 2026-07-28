use crate::model::extension::ServiceId;

use super::{
    input::{protocol, Input, Output as _},
    ServiceFailure,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProjectionKind {
    Service,
    CharacterDevice { major: u32, minor: u32 },
    BlockDevice { major: u32, minor: u32 },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServiceProjection {
    pub path: std::path::PathBuf,
    pub service: ServiceId,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub kind: ProjectionKind,
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
    let count = u32::try_from(entries.len()).map_err(|_| protocol())?;
    out.u32(count | 0xc000_0000);
    for entry in entries {
        use std::os::unix::ffi::OsStrExt;
        let path = entry.path.as_os_str().as_bytes();
        let (kind, major, minor) = match entry.kind {
            ProjectionKind::Service => (1, 0, 0),
            ProjectionKind::CharacterDevice { major, minor } => (4, major, minor),
            ProjectionKind::BlockDevice { major, minor } => (5, major, minor),
        };
        out.push(kind);
        out.u64(entry.service.0);
        out.u32(entry.mode);
        out.u32(entry.uid);
        out.u32(entry.gid);
        out.u16(u16::try_from(path.len()).map_err(|_| protocol())?);
        out.extend(path);
        out.u16(0);
        out.u32(major);
        out.u32(minor);
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
    let encoded_count = input.u32()?;
    if encoded_count & 0xc000_0000 != 0xc000_0000 {
        return Err(protocol());
    }
    let count = encoded_count & 0x3fff_ffff;
    if count > maximum_entries {
        return Err(ServiceFailure::linux(
            7,
            "service projection count exceeds launch bound",
        ));
    }
    let mut entries = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let kind = input.bytes(1)?[0];
        let service = ServiceId(input.u64()?);
        let mode = input.u32()?;
        let uid = input.u32()?;
        let gid = input.u32()?;
        let length = u32::from(input.u16()?);
        if length == 0 || length > maximum_path {
            return Err(ServiceFailure::linux(
                36,
                "service projection path exceeds launch bound",
            ));
        }
        let path = std::ffi::OsString::from_vec(input.bytes(length as usize)?.to_vec()).into();
        if input.u16()? != 0 {
            return Err(ServiceFailure::linux(
                22,
                "service projection symlink target is invalid",
            ));
        }
        let major = input.u32()?;
        let minor = input.u32()?;
        let kind = match kind {
            1 => ProjectionKind::Service,
            4 => ProjectionKind::CharacterDevice { major, minor },
            5 => ProjectionKind::BlockDevice { major, minor },
            _ => {
                return Err(ServiceFailure::linux(
                    22,
                    "service projection kind is invalid",
                ))
            }
        };
        entries.push(ServiceProjection {
            path,
            service,
            mode,
            uid,
            gid,
            kind,
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
        return Err(ServiceFailure::linux(
            7,
            "service projection count exceeds launch bound",
        ));
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
            || matches!(entry.kind, ProjectionKind::CharacterDevice { major, minor } | ProjectionKind::BlockDevice { major, minor } if major >= 4096 || minor >= (1 << 20))
        {
            return Err(ServiceFailure::linux(
                22,
                "invalid or conflicting service projection",
            ));
        }
    }
    for path in &paths {
        if path
            .ancestors()
            .skip(1)
            .any(|ancestor| ancestor != std::path::Path::new("/") && paths.contains(ancestor))
        {
            return Err(ServiceFailure::linux(
                20,
                "service projection cannot contain descendants",
            ));
        }
    }
    Ok(())
}
