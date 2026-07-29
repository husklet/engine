use std::{ffi::OsString, net::Ipv4Addr};

use crate::network::{Bridge, Interface, Namespace, Rule};

use super::*;

fn word(wire: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(wire[offset..offset + 4].try_into().unwrap())
}

fn string(wire: &[u8], field: usize) -> Option<&str> {
    let offset = word(wire, field) as usize;
    if offset == 0 {
        return None;
    }
    let pool = &wire[HEADER_SIZE..];
    let end = pool[offset..]
        .iter()
        .position(|byte| *byte == 0)
        .map(|length| offset + length)
        .unwrap();
    Some(std::str::from_utf8(&pool[offset..end]).unwrap())
}

#[test]
fn network_fields_use_the_exact_launch_abi_five_offsets() {
    let config = Config::new()
        .network_namespace(Namespace::new("container-alpha").unwrap())
        .network_bridge(Bridge::new("bridge-alpha").unwrap())
        .network_ipv4(Ipv4Addr::new(172, 18, 0, 2))
        .publish(Rule::new(8_080, 80).unwrap().address(Ipv4Addr::LOCALHOST))
        .publish(Rule::new(8_443, 443).unwrap())
        .publish_external(true);
    let wire = LaunchWire::encode(&config, &[OsString::from("/bin/true")], None).unwrap();

    assert_eq!(word(&wire, MAGIC_OFFSET), MAGIC);
    assert_eq!(word(&wire, HEADER_SIZE_OFFSET), HEADER_SIZE_U32);
    assert_eq!(word(&wire, ABI_OFFSET), ABI);
    assert_eq!(word(&wire, NETWORK_ISOLATED_OFFSET), 0);
    assert_eq!(word(&wire, PUBLISH_EXTERNAL_OFFSET), 1);
    assert_eq!(
        string(&wire, NETWORK_NAMESPACE_OFFSET),
        Some("container-alpha")
    );
    let offset = word(&wire, PUBLISH_OFFSET) as usize;
    let pool = &wire[HEADER_SIZE..];
    assert_eq!(
        &pool[offset..offset + 8],
        &[127, 0, 0, 1, 0x90, 0x1f, 80, 0]
    );
    assert_eq!(
        &pool[offset + 8..offset + 16],
        &[0, 0, 0, 0, 0xfb, 0x20, 0xbb, 1]
    );
    assert_eq!(word(&wire, PUBLISH_COUNT_OFFSET), 2);
    assert_eq!(string(&wire, NETWORK_BRIDGE_OFFSET), Some("bridge-alpha"));
    assert_eq!(string(&wire, IP_OFFSET), Some("172.18.0.2"));
    assert_eq!(word(&wire, INTERFACES_OFFSET), 0);
}

#[test]
fn multiple_interfaces_are_one_validated_launch_record() {
    let config = Config::new()
        .network_namespace(Namespace::new("container-multi").unwrap())
        .interface(
            Interface::new(
                Bridge::new("front").unwrap(),
                Ipv4Addr::new(172, 29, 0, 2),
                24,
            )
            .unwrap(),
        )
        .interface(
            Interface::new(
                Bridge::new("back").unwrap(),
                Ipv4Addr::new(172, 29, 1, 2),
                24,
            )
            .unwrap(),
        );
    let wire = LaunchWire::encode(&config, &[OsString::from("/bin/true")], None).unwrap();
    assert_eq!(
        string(&wire, INTERFACES_OFFSET),
        Some("front=172.29.0.2/24\nback=172.29.1.2/24")
    );
    assert_eq!(string(&wire, NETWORK_BRIDGE_OFFSET), None);
    assert_eq!(string(&wire, IP_OFFSET), None);
}

#[test]
fn existing_network_isolation_encoding_is_preserved() {
    let wire = LaunchWire::encode(
        &Config::new()
            .network(true)
            .network_namespace(Namespace::new("isolated-namespace").unwrap()),
        &[OsString::from("/bin/true")],
        None,
    )
    .unwrap();
    assert_eq!(word(&wire, NETWORK_ISOLATED_OFFSET), 1);
    assert_eq!(word(&wire, PUBLISH_EXTERNAL_OFFSET), 0);
    assert_eq!(
        string(&wire, NETWORK_NAMESPACE_OFFSET),
        Some("isolated-namespace")
    );
    assert_eq!(string(&wire, PUBLISH_OFFSET), None);
    assert_eq!(word(&wire, PUBLISH_COUNT_OFFSET), 0);
    assert_eq!(string(&wire, NETWORK_BRIDGE_OFFSET), None);
    assert_eq!(string(&wire, IP_OFFSET), None);
}

#[test]
fn host_network_has_a_distinct_typed_wire_encoding() {
    let wire = LaunchWire::encode(
        &Config::new().host_network(true),
        &[OsString::from("/bin/true")],
        None,
    )
    .unwrap();
    assert_eq!(word(&wire, NETWORK_ISOLATED_OFFSET), 0);
    assert_eq!(word(&wire, NETWORK_TRANSPORT_OFFSET), 2);
    assert_eq!(word(&wire, RESERVED_OFFSET), 0);
    assert_eq!(string(&wire, NETWORK_NAMESPACE_OFFSET), None);
}

#[test]
fn overlay_paths_are_ordered_nul_records() {
    let config = Config::new().overlay(
        vec!["/lower/high".into(), "/lower/low".into()],
        "/overlay/upper",
        "/overlay/work",
    );
    let wire = LaunchWire::encode(&config, &[OsString::from("/bin/true")], None).unwrap();
    assert_eq!(word(&wire, ABI_OFFSET), ABI);
    assert_eq!(word(&wire, HEADER_SIZE_OFFSET), HEADER_SIZE_U32);
    assert_eq!(word(&wire, LOWER_COUNT_OFFSET), 2);
    assert_eq!(string(&wire, ROOTFS_OFFSET), Some("/overlay/upper"));
    assert_eq!(string(&wire, OVERLAY_WORK_OFFSET), Some("/overlay/work"));
    let offset = word(&wire, LOWER_LAYERS_OFFSET) as usize;
    let pool = &wire[HEADER_SIZE..];
    assert_eq!(
        &pool[offset..offset + b"/lower/high\0/lower/low\0".len()],
        b"/lower/high\0/lower/low\0"
    );
    assert_eq!(word(&wire, RESERVED_OFFSET), 0);
}

#[test]
fn volume_records_escape_valid_unix_path_delimiters() {
    let mut config = Config::new();
    config.mounts.push(crate::Mount::read_only(
        "/host/with,comma",
        "/guest/with:colon",
    ));
    config.namespace_links.push((
        "/projection/sys/dev/char/226:128".into(),
        "/sys/dev/char/226:128".into(),
    ));

    let wire = LaunchWire::encode(&config, &[OsString::from("/bin/true")], None).unwrap();
    assert_eq!(
        string(&wire, VOLUMES_OFFSET),
        Some(
            "v2:ro:/guest/with%3Acolon:/host/with%2Ccomma,\
v2:link:/sys/dev/char/226%3A128:/projection/sys/dev/char/226%3A128"
        )
    );
}

#[test]
fn filesystem_generation_uses_the_c_abi_offset_and_rejects_nul() {
    use std::os::unix::ffi::OsStringExt;

    let wire = LaunchWire::encode(
        &Config::new().filesystem_generation("/run/hl/filesystem-generation"),
        &[OsString::from("/bin/true")],
        None,
    )
    .unwrap();
    assert_eq!(
        string(&wire, FILESYSTEM_GENERATION_OFFSET),
        Some("/run/hl/filesystem-generation")
    );

    let invalid = std::path::PathBuf::from(OsString::from_vec(b"/run/bad\0path".to_vec()));
    assert!(LaunchWire::encode(
        &Config::new().filesystem_generation(invalid),
        &[OsString::from("/bin/true")],
        None,
    )
    .is_err());
}

#[test]
fn checkpoint_mode_uses_the_c_abi_offset() {
    let capture = LaunchWire::encode(
        &Config::new().checkpoint_mode(CHECKPOINT_CAPTURE),
        &[OsString::from("/bin/true")],
        None,
    )
    .unwrap();
    assert_eq!(word(&capture, CHECKPOINT_MODE_OFFSET), CHECKPOINT_CAPTURE);

    let both = LaunchWire::encode(
        &Config::new().checkpoint_mode(CHECKPOINT_CAPTURE | CHECKPOINT_RESTORE),
        &[OsString::from("/bin/true")],
        None,
    )
    .unwrap();
    assert_eq!(
        word(&both, CHECKPOINT_MODE_OFFSET),
        CHECKPOINT_CAPTURE | CHECKPOINT_RESTORE
    );

    let none = LaunchWire::encode(&Config::new(), &[OsString::from("/bin/true")], None).unwrap();
    assert_eq!(word(&none, CHECKPOINT_MODE_OFFSET), 0);
}

#[test]
fn invalid_network_combinations_fail_before_launch() {
    let arguments = [OsString::from("/bin/true")];
    assert!(LaunchWire::encode(
        &Config::new()
            .network(true)
            .network_bridge(Bridge::new("isolated").unwrap()),
        &arguments,
        None,
    )
    .is_err());
    assert!(LaunchWire::encode(
        &Config::new().network_ipv4(Ipv4Addr::LOCALHOST),
        &arguments,
        None,
    )
    .is_err());
    assert!(LaunchWire::encode(&Config::new().publish_external(true), &arguments, None,).is_err());

    let mut config = Config::new();
    for port in 1..=33 {
        config = config.publish(Rule::new(port, port).unwrap());
    }
    assert!(LaunchWire::encode(&config, &arguments, None).is_err());
}

#[test]
fn file_owners_are_sorted_validated_records() {
    let config = Config::new()
        .owner("z/file", 12, 34)
        .owner("a/file", 56, 78);
    let wire = LaunchWire::encode(&config, &["/bin/true".into()], None).unwrap();
    let offset = u32::from_le_bytes(
        wire[FILE_OWNERS_OFFSET..FILE_OWNERS_OFFSET + 4]
            .try_into()
            .unwrap(),
    ) as usize;
    let pool = &wire[HEADER_SIZE..];
    let end = pool[offset..].iter().position(|byte| *byte == 0).unwrap() + offset;
    assert_eq!(&pool[offset..end], b"a/file\t56\t78\nz/file\t12\t34");
    assert!(LaunchWire::encode(
        &Config::new().owner("../escape", 1, 2),
        &["/bin/true".into()],
        None
    )
    .is_err());
}
