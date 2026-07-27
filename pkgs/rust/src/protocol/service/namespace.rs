use crate::api::extension::ServiceId;

use super::{
    input::{linux, protocol, put_u16, put_u32, put_u64, Input},
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
    let version_three = entries
        .iter()
        .any(|entry| !matches!(entry.kind, ProjectionKind::Service));
    put_u32(
        &mut out,
        if version_three {
            count | 0xc000_0000
        } else {
            count
        },
    );
    for entry in entries {
        let path = crate::sys::guest_path::bytes(&entry.path);
        let (kind, major, minor) = match entry.kind {
            ProjectionKind::Service => (1, 0, 0),
            ProjectionKind::CharacterDevice { major, minor } => (4, major, minor),
            ProjectionKind::BlockDevice { major, minor } => (5, major, minor),
        };
        if version_three {
            out.push(kind);
        }
        put_u64(&mut out, entry.service.0);
        put_u32(&mut out, entry.mode);
        put_u32(&mut out, entry.uid);
        put_u32(&mut out, entry.gid);
        put_u16(&mut out, u16::try_from(path.len()).map_err(|_| protocol())?);
        out.extend(path);
        if version_three {
            put_u16(&mut out, 0);
            put_u32(&mut out, major);
            put_u32(&mut out, minor);
        }
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
    let mut input = Input::new(bytes);
    let encoded_count = input.u32()?;
    let version_three = encoded_count & 0xc000_0000 == 0xc000_0000;
    let count = encoded_count & 0x3fff_ffff;
    if count > maximum_entries {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut entries = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let kind = if version_three { input.bytes(1)?[0] } else { 1 };
        let service = ServiceId(input.u64()?);
        let mode = input.u32()?;
        let uid = input.u32()?;
        let gid = input.u32()?;
        let length = u32::from(input.u16()?);
        if length == 0 || length > maximum_path {
            return Err(linux(36, "service projection path exceeds launch bound"));
        }
        // This is the *receive* path: these bytes came from the engine, not from a round trip of
        // this crate's own output. A projected path this host cannot represent is a protocol error,
        // which is the same class of answer the surrounding decoder already gives a malformed
        // transaction.
        let path: std::path::PathBuf =
            crate::sys::os_string(input.bytes(length as usize)?.to_vec())
                .ok_or_else(|| linux(22, "service projection path is not representable"))?
                .into();
        if version_three && input.u16()? != 0 {
            return Err(linux(22, "service projection symlink target is invalid"));
        }
        let major = if version_three { input.u32()? } else { 0 };
        let minor = if version_three { input.u32()? } else { 0 };
        let kind = match kind {
            1 => ProjectionKind::Service,
            4 => ProjectionKind::CharacterDevice { major, minor },
            5 => ProjectionKind::BlockDevice { major, minor },
            _ => return Err(linux(22, "service projection kind is invalid")),
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
    use crate::sys::guest_path;
    if entries.len() > maximum_entries as usize {
        return Err(linux(7, "service projection count exceeds launch bound"));
    }
    let mut paths = std::collections::BTreeSet::new();
    for entry in entries {
        let path = guest_path::bytes(&entry.path);
        // A projected path is a guest path, so it is classified by its bytes rather than by the
        // host's path parser: `is_absolute` and the component sweep both answer differently per host
        // and neither difference produces a diagnostic. `is_normalized_absolute` subsumes the
        // rooted-ness check, the `.`/`..` sweep and the NUL check in one byte-level pass.
        if entry.service.0 == 0
            || entry.mode & !0o7777 != 0
            || path.is_empty()
            || guest_path::is_root(path)
            || path.len() > maximum_path as usize
            || path.len() > u16::MAX as usize
            || !guest_path::is_normalized_absolute(path)
            || !paths.insert(entry.path.clone())
            || matches!(entry.kind, ProjectionKind::CharacterDevice { major, minor } | ProjectionKind::BlockDevice { major, minor } if major >= 4096 || minor >= (1 << 20))
        {
            return Err(linux(22, "invalid or conflicting service projection"));
        }
    }
    // Every path in the set is now known to be `/`-rooted with no `.`, `..`, empty or `\` segment,
    // and that is exactly the condition under which `Path`'s *structural* operations agree on every
    // host. So the containment sweep stays as it was.
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
