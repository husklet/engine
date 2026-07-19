use std::{
    collections::BTreeSet,
    fs,
    io::{Read, Write},
    net::TcpListener,
    path::PathBuf,
    process::Command,
    sync::{Arc, OnceLock},
};

use hl_engine::{
    extension::{
        AllocationRequest, Authorities, BindAccess, Coherency, DeviceEntry, DeviceKind,
        DirectoryEntry, ExtensionConfig, ExtensionSpec, Feature, FileEntry, FileSource,
        HandleOperation, Handles, HostBindEntry, HostResource, Inheritance, Interest, LinuxError,
        Memory, MemoryRequirement, Metadata, NamespaceEntry, OpenHandle, OpenRequest, Protections,
        ProviderAuthority, ProviderId, ReadRequest, Readiness, ReadyState, Region,
        ResourceDescriptor, ResourceError, ResourceId, ServiceEntry, ServiceId,
        ServiceRegistration, Sharing, SocketEntry, SymlinkEntry, WriteRequest,
    },
    network::Namespace,
    spec::{NetworkMode, SpecErrorCategory, TreeSource, Version},
    Engine, Exit, Guest, HandlesAuthority, MachineSpec, ProcessIo,
};

#[test]
fn isolated_network_accepts_an_opaque_shared_namespace_identity() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.network.mode = NetworkMode::None;
    spec.network.namespace = Some(Namespace::new("container-shared-loopback").unwrap());

    assert!(Engine::new().validate(&spec).is_ok());
}

#[test]
fn isolated_network_rejects_transport_configuration() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.network.mode = NetworkMode::None;
    spec.network.namespace = Some(Namespace::new("container-shared-loopback").unwrap());
    spec.network.external_listeners = true;

    let error = Engine::new().validate(&spec).unwrap_err();
    assert_eq!(error.category, SpecErrorCategory::Conflict);
    assert_eq!(error.field, "network");
}

fn rootfs() -> &'static PathBuf {
    static ROOTFS: OnceLock<PathBuf> = OnceLock::new();
    ROOTFS.get_or_init(|| {
        let path = std::env::temp_dir().join(format!("hl-typed-spec-{}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir(&path).unwrap();
        let fixture = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("assets/alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz");
        assert!(Command::new("tar")
            .args(["-xzf"])
            .arg(fixture)
            .arg("-C")
            .arg(&path)
            .status()
            .unwrap()
            .success());
        path
    })
}

fn mutable_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = std::env::temp_dir().join(format!("hl-mutable-probe-{}.c", std::process::id()));
        fs::write(&source, r#"
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int same(int fd, const char *s, size_t n, off_t at) {
    char b[32] = {0}; return pread(fd, b, n, at) == (ssize_t)n && !memcmp(b, s, n);
}
int main(int argc, char **argv) {
    if (argc != 2) return 10;
    int a = open(argv[1], O_RDWR), b = open(argv[1], O_RDWR);
    if (a < 0) return 21; if (b < 0) return 22; if (!same(a, "abcdef", 6, 0)) return 23;
    if (pwrite(b, "XY", 2, 1) != 2 || !same(a, "aXYdef", 6, 0)) return 12;
    if (ftruncate(a, 4096)) return 13;
    struct stat st; if (fstat(b, &st) || st.st_size != 4096 || (st.st_mode & 0777) != 0660) return 14;
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, a, 0);
    if (p == MAP_FAILED) return 15;
    memcpy(p + 8, "mapped", 6); if (msync(p, 4096, MS_SYNC) || !same(b, "mapped", 6, 8)) return 16;
    pid_t child = fork(); if (child < 0) return 17;
    if (!child) { memcpy(p + 16, "child", 5); _exit(0); }
    int status; if (waitpid(child, &status, 0) != child || status || !same(b, "child", 5, 16)) return 18;
    if (lseek(b, 0, SEEK_END) != 4096 || write(b, "tail", 4) != 4) return 19;
    if (fstat(a, &st) || st.st_size != 4100) return 20;
    return munmap(p, 4096) != 0;
}
"#).unwrap();
        let output = rootfs().join("tmp/mutable-probe");
        assert!(Command::new("cc").args(["-static", "-O2", "-o"]).arg(&output).arg(&source).status().unwrap().success());
        let _ = fs::remove_file(source);
    });
    "/tmp/mutable-probe"
}

fn procfd_fork_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../tests/compat/syscall_edges/sentry_cloexec_exec.c");
        let output = rootfs().join("tmp/procfd-fork-probe");
        assert!(Command::new("cc")
            .args(["-static", "-O2", "-std=gnu11", "-pthread", "-o"])
            .arg(&output)
            .arg(source)
            .status()
            .unwrap()
            .success());
    });
    "/tmp/procfd-fork-probe"
}

fn descriptor_pressure_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = std::env::temp_dir().join(format!(
            "hl-descriptor-pressure-probe-{}.c",
            std::process::id()
        ));
        fs::write(
            &source,
            r#"
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    int descriptors[4097];
    for (int i = 0; i < 4097; ++i) {
        descriptors[i] = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
        if (descriptors[i] < 0) return 20;
    }
    for (int i = 0; i < 4097; ++i) if (close(descriptors[i]) != 0) return 21;
    return 0;
}

"#,
        )
        .unwrap();
        let output = rootfs().join("tmp/descriptor-pressure-probe");
        assert!(Command::new("cc")
            .args(["-static", "-O2", "-o"])
            .arg(&output)
            .arg(&source)
            .status()
            .unwrap()
            .success());
        let _ = fs::remove_file(source);
    });
    "/tmp/descriptor-pressure-probe"
}

fn projected_socket_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = std::env::temp_dir().join(format!(
            "hl-projected-socket-probe-{}.c",
            std::process::id()
        ));
        fs::write(
            &source,
            r#"
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2) return 10;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return 11;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(argv[1]) >= sizeof(address.sun_path)) return 12;
    strcpy(address.sun_path, argv[1]);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) return 13;
    char reply[4];
    if (write(fd, "ping", 4) != 4 || read(fd, reply, 4) != 4 || memcmp(reply, "pong", 4)) return 14;
    return close(fd) != 0;
}
"#,
        )
        .unwrap();
        let output = rootfs().join("tmp/projected-socket-probe");
        assert!(Command::new("cc")
            .args(["-static", "-O2", "-o"])
            .arg(&output)
            .arg(&source)
            .status()
            .unwrap()
            .success());
        let _ = fs::remove_file(source);
    });
    "/tmp/projected-socket-probe"
}

fn unix_stream_reuse_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = std::env::temp_dir().join(format!(
            "hl-unix-stream-reuse-probe-{}.c",
            std::process::id()
        ));
        fs::write(
            &source,
            r#"
#define _GNU_SOURCE
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    int stale = eventfd(1, EFD_CLOEXEC);
    if (stale < 0 || close(stale) != 0) return 10;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener != stale) return 11;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, "/tmp/hl-pg-auth.sock");
    unlink(address.sun_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) return 12;
    pid_t child = fork();
    if (child < 0) return 13;
    if (child == 0) {
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        uint8_t startup[8] = {0,0,0,8,0,3,0,0};
        if (client < 0 || connect(client, (struct sockaddr *)&address, sizeof(address)) ||
            write(client, startup, sizeof(startup)) != sizeof(startup)) _exit(20);
        uint8_t first = 0, peek = 0, tail[12] = {0};
        if (recv(client, &peek, 1, MSG_PEEK) != 1 || peek != 'R' || read(client, &first, 1) != 1 || first != 'R')
            _exit(21);
        struct iovec parts[2] = {{tail, 4}, {tail + 4, 8}};
        if (readv(client, parts, 2) != 12 || memcmp(tail, "\0\0\0\b\0\0\0\0S\0\0\0", 12) != 0) _exit(22);
        _exit(0);
    }
    int peer = accept4(listener, 0, 0, SOCK_CLOEXEC);
    uint8_t startup[8];
    if (peer < 0 || recv(peer, startup, sizeof(startup), MSG_WAITALL) != sizeof(startup)) return 14;
    static const uint8_t auth[] = {'R',0,0,0,8,0,0,0,0};
    static const uint8_t status[] = {'S',0,0,0};
    struct iovec frames[2] = {{(void *)auth, sizeof(auth)}, {(void *)status, sizeof(status)}};
    if (writev(peer, frames, 2) != sizeof(auth) + sizeof(status)) return 15;
    int status_code = 0;
    if (waitpid(child, &status_code, 0) != child || !WIFEXITED(status_code)) return 16;
    unlink(address.sun_path);
    return WEXITSTATUS(status_code);
}

"#,
        )
        .unwrap();
        let output = rootfs().join("tmp/unix-stream-reuse-probe");
        assert!(Command::new("cc")
            .args(["-static", "-O2", "-o"])
            .arg(&output)
            .arg(&source)
            .status()
            .unwrap()
            .success());
        let _ = fs::remove_file(source);
    });
    "/tmp/unix-stream-reuse-probe"
}

fn epoll_wake_socket_collision_probe() -> &'static str {
    static PROBE: OnceLock<()> = OnceLock::new();
    PROBE.get_or_init(|| {
        let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../tests/compat/ipc/ipc_epoll_wake_socket_collision.c");
        let output = rootfs().join("tmp/epoll-wake-socket-collision-probe");
        assert!(Command::new("cc")
            .args(["-static", "-O2", "-std=gnu11", "-pthread", "-o"])
            .arg(&output)
            .arg(source)
            .status()
            .unwrap()
            .success());
    });
    "/tmp/epoll-wake-socket-collision-probe"
}

fn extension(required: bool) -> ExtensionSpec {
    ExtensionSpec {
        provider: ProviderId::new("example.device").unwrap(),
        version: Version::new(1, 0),
        required,
        required_features: BTreeSet::new(),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig::empty("example.device/v1"),
        namespace: Vec::new(),
        services: Vec::new(),
        memory: Vec::new(),
        environment: Vec::new(),
    }
}

fn namespace_extension(bytes: &'static [u8]) -> ExtensionSpec {
    ExtensionSpec {
        provider: ProviderId::new("engine.namespace").unwrap(),
        version: Version::new(1, 0),
        required: true,
        required_features: BTreeSet::new(),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig::empty("engine.namespace/v1"),
        namespace: vec![
            NamespaceEntry::Directory(DirectoryEntry {
                path: "/opt/husklet".into(),
                metadata: Metadata {
                    mode: 0o755,
                    uid: 0,
                    gid: 0,
                },
            }),
            NamespaceEntry::File(FileEntry {
                path: "/opt/husklet/config".into(),
                metadata: Metadata {
                    mode: 0o440,
                    uid: 0,
                    gid: 0,
                },
                source: FileSource::Immutable(Arc::from(bytes)),
            }),
            NamespaceEntry::Symlink(SymlinkEntry {
                path: "/opt/husklet/link".into(),
                target: "config".into(),
                uid: 0,
                gid: 0,
            }),
        ],
        services: Vec::new(),
        memory: Vec::new(),
        environment: Vec::new(),
    }
}

fn host_bind_extension(entries: Vec<HostBindEntry>) -> ExtensionSpec {
    ExtensionSpec {
        provider: ProviderId::new("engine.namespace").unwrap(),
        version: Version::new(1, 0),
        required: true,
        required_features: BTreeSet::from([Feature::new("host-bind-read-only").unwrap()]),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig::empty("engine.namespace/v1"),
        namespace: entries.into_iter().map(NamespaceEntry::HostBind).collect(),
        services: Vec::new(),
        memory: Vec::new(),
        environment: Vec::new(),
    }
}

fn host_fixture(name: &str) -> PathBuf {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let path = std::env::temp_dir().join(format!(
        "hl-host-bind-{}-{}-{name}",
        std::process::id(),
        NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    ));
    fs::create_dir(&path).unwrap();
    path
}

#[test]
fn socket_projection_accepts_only_existing_unix_sockets() {
    use std::os::unix::net::UnixListener;
    let directory = host_fixture("socket-projection");
    let socket = directory.join("provider.sock");
    let _listener = UnixListener::bind(&socket).unwrap();
    let mut extension = namespace_extension(b"unused");
    extension.required_features = BTreeSet::from([Feature::new("unix-sockets").unwrap()]);
    extension.namespace = vec![NamespaceEntry::Socket(SocketEntry {
        path: "/run/provider.sock".into(),
        host: socket,
    })];
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(extension);
    assert!(Engine::new().validate(&spec).is_ok());

    let file = directory.join("not-a-socket");
    fs::write(&file, b"no").unwrap();
    if let NamespaceEntry::Socket(entry) = &mut spec.extensions[0].namespace[0] {
        entry.host = file;
    }
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Invalid
    );
}

#[test]
fn projected_socket_connects_to_the_granted_host_endpoint() {
    use std::os::unix::net::UnixListener;
    let directory = host_fixture("socket-execution");
    let socket = directory.join("provider.sock");
    let listener = UnixListener::bind(&socket).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = [0; 4];
        stream.read_exact(&mut request).unwrap();
        assert_eq!(&request, b"ping");
        stream.write_all(b"pong").unwrap();
    });
    let mut extension = namespace_extension(b"unused");
    extension.required_features = BTreeSet::from([Feature::new("unix-sockets").unwrap()]);
    extension.namespace = vec![NamespaceEntry::Socket(SocketEntry {
        path: "/run/provider.sock".into(),
        host: socket,
    })];
    let mut spec = MachineSpec::new(Guest::Aarch64, projected_socket_probe());
    spec.process.argv.push("/run/provider.sock".into());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions.push(extension);
    assert_eq!(
        Engine::new()
            .spawn(spec, ProcessIo::default())
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    server.join().unwrap();
}

#[test]
fn device_projection_requires_and_accepts_a_registered_service() {
    let mut extension = handles_extension();
    extension
        .required_features
        .insert(Feature::new("devices").unwrap());
    extension.namespace = vec![NamespaceEntry::Device(DeviceEntry {
        path: "/dev/provider".into(),
        metadata: Metadata {
            mode: 0o660,
            uid: 10,
            gid: 20,
        },
        kind: DeviceKind::Character,
        major: 226,
        minor: 128,
        service: Some(ServiceId(77)),
    })];
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(extension);
    assert!(Engine::new().validate(&spec).is_ok());

    if let NamespaceEntry::Device(entry) = &mut spec.extensions[0].namespace[0] {
        entry.service = None;
    }
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Unsupported
    );
}

#[test]
fn facade_accepts_projected_endpoints_memory_terminal_and_live_signal_together() {
    use std::os::unix::net::UnixListener;

    let directory = host_fixture("combined-capabilities");
    let socket = directory.join("provider.sock");
    let _listener = UnixListener::bind(&socket).unwrap();

    let mut namespace = namespace_extension(b"unused");
    namespace.required_features = BTreeSet::from([Feature::new("unix-sockets").unwrap()]);
    namespace.namespace = vec![NamespaceEntry::Socket(SocketEntry {
        path: "/run/provider.sock".into(),
        host: socket,
    })];

    let mut handles = handles_extension();
    handles
        .required_features
        .extend([Feature::new("devices").unwrap(), Feature::new("memory-allocation").unwrap()]);
    handles.namespace.push(NamespaceEntry::Device(DeviceEntry {
        path: "/dev/provider".into(),
        metadata: Metadata {
            mode: 0o660,
            uid: 0,
            gid: 0,
        },
        kind: DeviceKind::Character,
        major: 226,
        minor: 128,
        service: Some(ServiceId(77)),
    }));
    handles.memory = memory_extension().memory;

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.terminal = Some(hl_engine::Size::new(24, 80).unwrap());
    spec.extensions.extend([namespace, handles]);

    assert!(Engine::new().validate(&spec).is_ok());
    assert!(Engine::new()
        .capabilities()
        .control
        .operations
        .contains(&hl_engine::spec::ControlOperation::Signal));
}

#[test]
fn discovery_reports_models_and_limits_instead_of_architecture_booleans() {
    let capabilities = Engine::new().capabilities();
    assert!(capabilities
        .guests
        .iter()
        .any(|guest| guest.architecture == Guest::Aarch64));
    assert!(capabilities
        .guests
        .iter()
        .any(|guest| guest.architecture == Guest::X86_64));
    assert_eq!(capabilities.cpu.page_sizes, [4096]);
    assert!(capabilities.networking.modes.contains(&NetworkMode::None));
    assert!(capabilities.limits.path_bytes >= 4096);
    assert!(capabilities
        .filesystems
        .features
        .contains(&hl_engine::spec::FilesystemFeature::ProjectedNamespace));
    let namespace = capabilities
        .extensions
        .iter()
        .find(|extension| extension.provider.as_str() == "engine.namespace")
        .unwrap();
    assert_eq!(namespace.versions, [Version::new(1, 0)]);
    assert_eq!(namespace.limits.mappings, 0);
    assert_eq!(
        namespace
            .features
            .iter()
            .map(Feature::as_str)
            .collect::<Vec<_>>(),
        [
            "directories",
            "host-bind-read-only",
            "immutable-files",
            "mutable-files",
            "symlinks",
            "unix-sockets"
        ]
    );
    let handles = capabilities
        .extensions
        .iter()
        .find(|extension| extension.provider.as_str() == "engine.handles")
        .unwrap();
    assert!(handles
        .features
        .iter()
        .any(|feature| feature.as_str() == "memory-allocation"));
    assert_eq!(handles.limits.mappings, 64);
    assert!(!capabilities.checkpoint.supported);
}

#[test]
fn open_file_limit_cannot_enter_the_engine_private_descriptor_range() {
    let engine = Engine::new();
    let ceiling = engine.capabilities().limits.handles;
    assert!(ceiling > 20_480);
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.resources.open_files = ceiling.checked_add(1);
    let error = engine.validate(&spec).unwrap_err();
    assert_eq!(error.category, SpecErrorCategory::Limit);
    assert_eq!(error.field, "resources.open_files");
}

#[cfg(target_os = "linux")]
#[test]
fn capabilities_track_the_host_limit_and_preserve_a_private_interval() {
    const CHILD: &str = "HL_FD_LIMIT_CHILD";
    if let Ok(expected) = std::env::var(CHILD) {
        assert_eq!(
            Engine::new().capabilities().limits.handles,
            expected.parse::<u32>().unwrap()
        );
        return;
    }
    let executable = std::env::current_exe().unwrap();
    for (limit, expected) in [(30_000, 25_904), (24_576, 0)] {
        assert!(Command::new("prlimit")
            .arg(format!("--nofile={limit}:{limit}"))
            .arg("--")
            .arg(&executable)
            .args([
                "--exact",
                "capabilities_track_the_host_limit_and_preserve_a_private_interval",
                "--nocapture",
            ])
            .env(CHILD, expected.to_string())
            .status()
            .unwrap()
            .success());
    }
}

#[test]
fn projected_directory_file_and_symlink_share_the_guest_vfs() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process.argv.extend([
        "-c".into(),
        "test \"$(cat /opt/husklet/config)\" = projected && \
         test \"$(readlink /opt/husklet/link)\" = config && \
         test \"$(cat /opt/husklet/link)\" = projected && \
         test \"$(stat -c %a /opt/husklet/config)\" = 440 && \
         test \"$(ls /opt/husklet)\" = 'config
link'"
            .into(),
    ]);
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions.push(namespace_extension(b"projected\n"));
    let validation = Engine::new().validate(&spec).unwrap();
    assert_eq!(validation.selected_extensions.len(), 1);
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
}

#[test]
fn typed_overlay_merges_copies_up_and_whiteouts_without_materializing_the_lower() {
    let base = std::env::temp_dir().join(format!("hl-typed-overlay-{}", std::process::id()));
    let upper = base.join("upper");
    let work = base.join("work");
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&upper).unwrap();
    fs::create_dir_all(&work).unwrap();

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process.argv.extend([
        "-c".into(),
        "grep -q '^ID=alpine' /etc/os-release && \
         test -n \"$(readlink /bin/sh)\" && \
         ls /etc | grep -q '^os-release$' && \
         printf upper >/tmp/overlay-created && \
         printf changed >/etc/hostname && \
         mv /etc/hostname /etc/hostname.moved && \
         test ! -e /etc/hostname && test \"$(cat /etc/hostname.moved)\" = changed && \
         rm /etc/os-release && test ! -e /etc/os-release && \
         test \"$(cat /tmp/overlay-created)\" = upper"
            .into(),
    ]);
    spec.filesystem.root = Some(TreeSource::Overlay {
        lower: vec![TreeSource::HostDirectory(rootfs().clone())],
        upper: upper.clone(),
        work,
    });
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(
        fs::read_to_string(upper.join("tmp/overlay-created")).unwrap(),
        "upper"
    );
    assert_eq!(
        fs::read_to_string(upper.join("etc/hostname.moved")).unwrap(),
        "changed"
    );
    assert!(upper.join("etc/.wh.hostname").exists());
    assert!(upper.join("etc/.wh.os-release").exists());
}

#[test]
fn typed_overlay_executes_a_final_symlink_from_the_merged_namespace() {
    let base = std::env::temp_dir().join(format!(
        "hl-overlay-executable-symlink-{}",
        std::process::id()
    ));
    let links = base.join("links");
    let upper = base.join("upper");
    let work = base.join("work");
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&links).unwrap();
    fs::create_dir_all(&upper).unwrap();
    fs::create_dir_all(&work).unwrap();
    std::os::unix::fs::symlink("/bin/busybox", links.join("echo")).unwrap();

    let mut spec = MachineSpec::new(Guest::Aarch64, "/echo");
    spec.process.argv.push("OVERLAY_EXEC_LINK_OK".into());
    spec.filesystem.root = Some(TreeSource::Overlay {
        lower: vec![
            TreeSource::HostDirectory(links),
            TreeSource::HostDirectory(rootfs().clone()),
        ],
        upper,
        work,
    });
    let mut machine = Engine::new()
        .spawn(
            spec,
            ProcessIo {
                stdout: hl_engine::Stdio::piped(),
                ..ProcessIo::default()
            },
        )
        .unwrap();
    let mut output = String::new();
    machine
        .take_stdout()
        .unwrap()
        .read_to_string(&mut output)
        .unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(output.trim(), "OVERLAY_EXEC_LINK_OK");
}

#[test]
fn concurrent_overlays_isolate_upper_state_and_confine_lower_symlinks() {
    let base = std::env::temp_dir().join(format!("hl-overlay-isolation-{}", std::process::id()));
    let lower = base.join("lower");
    let secret = base.join("host-secret");
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&lower).unwrap();
    fs::write(&secret, "HOST_SECRET").unwrap();
    std::os::unix::fs::symlink(&secret, lower.join("escape")).unwrap();

    let launch = |name: &'static str| {
        let base = base.clone();
        let lower = lower.clone();
        std::thread::spawn(move || {
            let upper = base.join(format!("upper-{name}"));
            let work = base.join(format!("work-{name}"));
            fs::create_dir_all(&upper).unwrap();
            fs::create_dir_all(&work).unwrap();
            let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
            spec.process.argv.extend([
                "-c".into(),
                format!(
                    "test \"$(cat /escape 2>/dev/null)\" != HOST_SECRET && printf {name} >/tmp/value"
                )
                .into(),
            ]);
            spec.filesystem.root = Some(TreeSource::Overlay {
                lower: vec![
                    TreeSource::HostDirectory(lower),
                    TreeSource::HostDirectory(rootfs().clone()),
                ],
                upper: upper.clone(),
                work,
            });
            let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
            assert_eq!(machine.wait().unwrap(), Exit::Code(0));
            assert_eq!(fs::read_to_string(upper.join("tmp/value")).unwrap(), name);
        })
    };
    let alpha = launch("alpha");
    let beta = launch("beta");
    alpha.join().unwrap();
    beta.join().unwrap();
    assert_eq!(fs::read_to_string(&secret).unwrap(), "HOST_SECRET");
}

#[test]
fn overlay_mmap_truncate_and_rename_share_one_copied_up_file() {
    let base = std::env::temp_dir().join(format!("hl-overlay-coherence-{}", std::process::id()));
    let tools = base.join("tools");
    let upper = base.join("upper");
    let work = base.join("work");
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&tools).unwrap();
    fs::create_dir_all(&upper).unwrap();
    fs::create_dir_all(&work).unwrap();
    let source =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/compat/overlay/coherence.c");
    assert!(Command::new("cc")
        .args(["-static", "-O2", "-std=c11", "-o"])
        .arg(tools.join("probe"))
        .arg(source)
        .status()
        .unwrap()
        .success());
    let mut spec = MachineSpec::new(Guest::Aarch64, "/probe");
    spec.filesystem.root = Some(TreeSource::Overlay {
        lower: vec![
            TreeSource::HostDirectory(tools),
            TreeSource::HostDirectory(rootfs().clone()),
        ],
        upper: upper.clone(),
        work,
    });
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(fs::read(upper.join("etc/hostname.moved")).unwrap(), b"MMca");
    assert!(upper.join("etc/.wh.hostname").exists());
}

#[test]
fn host_network_reaches_a_real_host_loopback_listener() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = std::thread::spawn(move || {
        let (mut stream, peer) = listener.accept().unwrap();
        assert!(peer.ip().is_loopback());
        stream.write_all(b"host-network-ok\n").unwrap();
    });

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process.argv.extend([
        "-c".into(),
        format!("nc 127.0.0.1 {port} | grep -qx host-network-ok").into(),
    ]);
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.network.mode = NetworkMode::Host;
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    server.join().unwrap();
}

#[test]
fn namespace_projection_rejects_escape_and_symlink_parent_conflicts() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    let mut extension = namespace_extension(b"value");
    match &mut extension.namespace[1] {
        NamespaceEntry::File(file) => file.path = "/opt/husklet/../escape".into(),
        _ => unreachable!(),
    }
    spec.extensions.push(extension);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Invalid
    );

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    let mut extension = namespace_extension(b"value");
    extension.namespace = vec![
        NamespaceEntry::Symlink(SymlinkEntry {
            path: "/opt/provider".into(),
            target: "/outside".into(),
            uid: 0,
            gid: 0,
        }),
        NamespaceEntry::File(FileEntry {
            path: "/opt/provider/child".into(),
            metadata: Metadata {
                mode: 0o444,
                uid: 0,
                gid: 0,
            },
            source: FileSource::Immutable(Arc::from(&b"value"[..])),
        }),
    ];
    spec.extensions.push(extension);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Conflict
    );
}

#[test]
fn concurrent_namespace_launches_are_isolated_and_revoked() {
    fn launch(bytes: &'static [u8]) -> std::thread::JoinHandle<Exit> {
        std::thread::spawn(move || {
            let expected = std::str::from_utf8(bytes).unwrap();
            let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
            spec.process.argv.extend([
                "-c".into(),
                format!("sleep .1; test \"$(cat /opt/husklet/config)\" = {expected}").into(),
            ]);
            spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
            spec.extensions.push(namespace_extension(bytes));
            Engine::new()
                .spawn(spec, ProcessIo::default())
                .unwrap()
                .wait()
                .unwrap()
        })
    }
    let first = launch(b"first");
    let second = launch(b"second");
    assert_eq!(first.join().unwrap(), Exit::Code(0));
    assert_eq!(second.join().unwrap(), Exit::Code(0));
}

#[test]
fn read_only_host_file_and_directory_use_coherent_guest_path_operations() {
    use std::os::unix::fs::symlink;

    let host = host_fixture("coherent");
    let directory = host.join("directory");
    fs::create_dir(&directory).unwrap();
    fs::write(directory.join("alpha"), b"alpha").unwrap();
    symlink("alpha", directory.join("link")).unwrap();
    let file = host.join("single");
    fs::write(&file, b"single").unwrap();

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process.argv.extend([
        "-c".into(),
        "test \"$(cat /srv/provider/alpha)\" = alpha && \
         test \"$(readlink /srv/provider/link)\" = alpha && \
         test \"$(cat /srv/provider/link)\" = alpha && \
         test \"$(ls /srv/provider)\" = 'alpha
link' && \
         test \"$(stat -c %F /srv/provider/alpha)\" = 'regular file' && \
         test \"$(cat /etc/provider-value)\" = single && \
         ! echo denied >> /etc/provider-value"
            .into(),
    ]);
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions.push(host_bind_extension(vec![
        HostBindEntry {
            path: "/srv/provider".into(),
            host: directory,
            access: BindAccess::ReadOnly,
        },
        HostBindEntry {
            path: "/etc/provider-value".into(),
            host: file,
            access: BindAccess::ReadOnly,
        },
    ]));
    assert_eq!(
        Engine::new()
            .spawn(spec, ProcessIo::default())
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    fs::remove_dir_all(host).unwrap();
}

#[test]
fn host_bind_validation_rejects_writable_symlink_and_special_authority() {
    use std::os::unix::{fs::symlink, net::UnixListener};

    let host = host_fixture("rejected");
    let file = host.join("file");
    fs::write(&file, b"value").unwrap();
    let link = host.join("link");
    symlink(&file, &link).unwrap();
    let socket = host.join("socket");
    let listener = UnixListener::bind(&socket).unwrap();
    for (path, access, category) in [
        (&file, BindAccess::ReadWrite, SpecErrorCategory::Unsupported),
        (&link, BindAccess::ReadOnly, SpecErrorCategory::Unsupported),
        (
            &socket,
            BindAccess::ReadOnly,
            SpecErrorCategory::Unsupported,
        ),
    ] {
        let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
        spec.extensions
            .push(host_bind_extension(vec![HostBindEntry {
                path: "/run/provider".into(),
                host: path.clone(),
                access,
            }]));
        assert_eq!(
            Engine::new().validate(&spec).unwrap_err().category,
            category
        );
    }
    drop(listener);
    fs::remove_dir_all(host).unwrap();
}

#[test]
fn host_directory_bind_cannot_escape_granted_authority_through_a_symlink() {
    use std::os::unix::fs::symlink;

    let host = host_fixture("authority");
    let granted = host.join("granted");
    fs::create_dir(&granted).unwrap();
    fs::write(host.join("secret"), b"must-not-be-visible").unwrap();
    symlink("../secret", granted.join("escape")).unwrap();
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process.argv.extend([
        "-c".into(),
        "test \"$(readlink /run/provider/escape)\" = ../secret && \
         ! cat /run/provider/escape >/dev/null 2>&1"
            .into(),
    ]);
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions
        .push(host_bind_extension(vec![HostBindEntry {
            path: "/run/provider".into(),
            host: granted,
            access: BindAccess::ReadOnly,
        }]));
    assert_eq!(
        Engine::new()
            .spawn(spec, ProcessIo::default())
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
    fs::remove_dir_all(host).unwrap();
}

#[test]
fn host_bind_observes_host_changes_and_concurrent_launches_remain_isolated() {
    let first = host_fixture("first");
    let second = host_fixture("second");
    fs::write(first.join("value"), b"before").unwrap();
    fs::write(second.join("value"), b"second").unwrap();
    let launch = |host: PathBuf, expected: &'static str| {
        std::thread::spawn(move || {
            let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
            spec.process.argv.extend([
                "-c".into(),
                format!("sleep .2; test \"$(cat /run/provider/value)\" = {expected}").into(),
            ]);
            spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
            spec.extensions
                .push(host_bind_extension(vec![HostBindEntry {
                    path: "/run/provider".into(),
                    host,
                    access: BindAccess::ReadOnly,
                }]));
            Engine::new()
                .spawn(spec, ProcessIo::default())
                .unwrap()
                .wait()
                .unwrap()
        })
    };
    let one = launch(first.clone(), "after");
    let two = launch(second.clone(), "second");
    std::thread::sleep(std::time::Duration::from_millis(50));
    fs::write(first.join("value"), b"after").unwrap();
    assert_eq!(one.join().unwrap(), Exit::Code(0));
    assert_eq!(two.join().unwrap(), Exit::Code(0));
    fs::remove_dir_all(first).unwrap();
    fs::remove_dir_all(second).unwrap();
}

#[test]
fn mutable_file_is_coherent_across_opens_fork_truncate_and_mmap() {
    let mut spec = MachineSpec::new(Guest::Aarch64, mutable_probe());
    spec.process.argv.push("/run/provider/mutable".into());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let mut extension = namespace_extension(b"unused");
    extension.required_features = BTreeSet::from([Feature::new("mutable-files").unwrap()]);
    extension.namespace = vec![
        NamespaceEntry::Directory(DirectoryEntry {
            path: "/run/provider".into(),
            metadata: Metadata {
                mode: 0o755,
                uid: 0,
                gid: 0,
            },
        }),
        NamespaceEntry::File(FileEntry {
            path: "/run/provider/mutable".into(),
            metadata: Metadata {
                mode: 0o660,
                uid: 0,
                gid: 0,
            },
            source: FileSource::Mutable(Arc::from(&b"abcdef"[..])),
        }),
    ];
    spec.extensions.push(extension);
    assert_eq!(
        Engine::new()
            .spawn(spec, ProcessIo::default())
            .unwrap()
            .wait()
            .unwrap(),
        Exit::Code(0)
    );
}

#[test]
fn mutable_files_with_the_same_guest_path_are_isolated_between_launches() {
    fn launch(initial: &'static [u8], expected: &'static str) -> std::thread::JoinHandle<Exit> {
        std::thread::spawn(move || {
            let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
            spec.process.argv.extend([
                "-c".into(),
                format!(
                    "printf=-changed; echo -n \"$printf\" >> /run/shared; sleep .1; \
                     test \"$(cat /run/shared)\" = {expected}"
                )
                .into(),
            ]);
            spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
            let mut extension = namespace_extension(b"unused");
            extension.required_features = BTreeSet::from([Feature::new("mutable-files").unwrap()]);
            extension.namespace = vec![NamespaceEntry::File(FileEntry {
                path: "/run/shared".into(),
                metadata: Metadata {
                    mode: 0o660,
                    uid: 0,
                    gid: 0,
                },
                source: FileSource::Mutable(Arc::from(initial)),
            })];
            spec.extensions.push(extension);
            Engine::new()
                .spawn(spec, ProcessIo::default())
                .unwrap()
                .wait()
                .unwrap()
        })
    }
    let first = launch(b"first", "first-changed");
    let second = launch(b"second", "second-changed");
    assert_eq!(first.join().unwrap(), Exit::Code(0));
    assert_eq!(second.join().unwrap(), Exit::Code(0));
}

#[test]
fn combined_mutable_initial_bytes_are_bounded() {
    let bytes: Arc<[u8]> = vec![0_u8; 33 * 1024 * 1024].into();
    let mut extension = namespace_extension(b"unused");
    extension.required_features = BTreeSet::from([Feature::new("mutable-files").unwrap()]);
    extension.namespace = ["/run/first", "/run/second"]
        .into_iter()
        .map(|path| {
            NamespaceEntry::File(FileEntry {
                path: path.into(),
                metadata: Metadata {
                    mode: 0o600,
                    uid: 0,
                    gid: 0,
                },
                source: FileSource::Mutable(bytes.clone()),
            })
        })
        .collect();
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(extension);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Limit
    );
}

#[test]
fn validation_rejects_unknown_required_extensions_without_host_state() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(extension(true));
    let error = Engine::new().validate(&spec).unwrap_err();
    assert_eq!(error.category, SpecErrorCategory::Unsupported);
    assert_eq!(error.field, "extensions");
    assert!(matches!(
        error.resource,
        Some(hl_engine::spec::SpecResource::Provider(_))
    ));
}

#[test]
fn validation_reports_unknown_optional_extensions_as_unavailable() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    let provider = ProviderId::new("example.device").unwrap();
    spec.extensions.push(extension(false));
    let validation = Engine::new().validate(&spec).unwrap();
    assert_eq!(validation.unavailable_optional_extensions, [provider]);
}

#[test]
fn meaningful_unsupported_launch_policy_is_never_silently_ignored() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.umask = 0o077;
    let error = Engine::new().validate(&spec).unwrap_err();
    assert_eq!(error.field, "process.umask");

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.checkpoint.enabled = true;
    let error = Engine::new().validate(&spec).unwrap_err();
    assert_eq!(error.field, "checkpoint.enabled");

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.cpu.features.insert("example-feature".into());
    assert_eq!(Engine::new().validate(&spec).unwrap_err().field, "cpu");

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.cpu.model = Some("example-model".into());
    assert_eq!(Engine::new().validate(&spec).unwrap_err().field, "cpu");
}

#[test]
fn invalid_guest_process_data_fails_preflight() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.cwd = "relative".into();
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "process.cwd"
    );

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.env = vec![("BAD=NAME".into(), "value".into())];
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "process.env"
    );

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.argv[0] = "custom-argv-zero".into();
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "process.argv[0]"
    );

    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.cache.directory = Some("relative-cache".into());
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "cache.directory"
    );
}

#[test]
fn typed_machine_spec_launches_through_the_existing_backend() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
}

#[test]
fn typed_rootfs_preserves_delayed_process_substitution_across_fork_and_exec() {
    let mut spec = MachineSpec::new(Guest::Aarch64, procfd_fork_probe());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
}

#[test]
fn typed_machines_cross_the_old_private_descriptor_ceiling_and_release_files() {
    for _ in 0..2 {
        let mut spec = MachineSpec::new(Guest::Aarch64, descriptor_pressure_probe());
        spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
        let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
        assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    }
}

#[test]
fn typed_close_clears_emulation_state_before_unix_stream_fd_reuse() {
    let mut spec = MachineSpec::new(Guest::Aarch64, unix_stream_reuse_probe());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
}

#[test]
fn internal_epoll_wake_never_overlaps_a_guest_unix_stream() {
    let mut spec = MachineSpec::new(Guest::Aarch64, epoll_wake_socket_collision_probe());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
}

#[test]
fn unavailable_optional_extensions_still_receive_complete_shape_validation() {
    let mut valid = extension(false);
    valid
        .namespace
        .push(NamespaceEntry::Directory(DirectoryEntry {
            path: "/opt/example".into(),
            metadata: Metadata {
                mode: 0o755,
                uid: 0,
                gid: 0,
            },
        }));
    valid.services.push(ServiceRegistration {
        id: ServiceId(7),
        operations: BTreeSet::from([HandleOperation::Read]),
        max_request_bytes: 4096,
    });
    valid.memory.push(MemoryRequirement {
        size: 4096,
        alignment: 4096,
        protections: Protections {
            read: true,
            write: true,
            execute: false,
        },
        sharing: Sharing::Shared,
        inheritance: Inheritance::Retain,
    });
    valid
        .environment
        .push(("EXAMPLE_SOCKET".into(), "/run/example".into()));
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(valid.clone());
    assert_eq!(
        Engine::new()
            .validate(&spec)
            .unwrap()
            .unavailable_optional_extensions
            .len(),
        1
    );

    let mut malformed = valid.clone();
    match &mut malformed.namespace[0] {
        NamespaceEntry::Directory(entry) => entry.path = "relative".into(),
        _ => unreachable!(),
    }
    spec.extensions = vec![malformed];
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Invalid
    );

    let mut malformed = valid.clone();
    malformed.services[0].max_request_bytes = 0;
    spec.extensions = vec![malformed];
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "extensions[0].services"
    );

    let mut malformed = valid;
    malformed.memory[0].alignment = 3;
    spec.extensions = vec![malformed];
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "extensions[0].memory"
    );
}

#[test]
fn extension_features_and_environment_are_conflict_checked() {
    let feature = Feature::new("example.feature").unwrap();
    let mut value = extension(false);
    value.required_features.insert(feature.clone());
    value.optional_features.insert(feature);
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(value);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "extensions[0].features"
    );

    let mut value = extension(false);
    value
        .environment
        .push(("PATH".into(), "/provider/bin".into()));
    spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.process.env.push(("PATH".into(), "/usr/bin".into()));
    spec.extensions.push(value);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().field,
        "extensions[0].environment"
    );
}

#[test]
fn unavailable_optional_namespace_conflicts_are_reported_not_installed() {
    let mut first = extension(false);
    first
        .namespace
        .push(NamespaceEntry::Directory(DirectoryEntry {
            path: "/opt/shared".into(),
            metadata: Metadata {
                mode: 0o755,
                uid: 0,
                gid: 0,
            },
        }));
    let mut second = first.clone();
    second.provider = ProviderId::new("example.second").unwrap();
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions = vec![first, second];
    let validation = Engine::new().validate(&spec).unwrap();
    assert_eq!(validation.unavailable_optional_extensions.len(), 2);
    assert_eq!(validation.namespace_conflicts.len(), 1);
    assert_eq!(
        validation.namespace_conflicts[0].path,
        std::path::Path::new("/opt/shared")
    );
    assert_eq!(
        validation.namespace_conflicts[0].disposition,
        hl_engine::spec::ConflictDisposition::InactiveOptional
    );
}

#[test]
fn validation_estimates_only_resources_that_can_be_selected() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.resources.memory_bytes = Some(16 * 1024 * 1024);
    spec.resources.process_limit = Some(12);
    spec.resources.cpu_limit = Some(3);
    let mut optional = extension(false);
    optional.memory.push(MemoryRequirement {
        size: 4096,
        alignment: 4096,
        protections: Protections {
            read: true,
            write: false,
            execute: false,
        },
        sharing: Sharing::Private,
        inheritance: Inheritance::Invalidate,
    });
    spec.extensions.push(optional);
    let validation = Engine::new().validate(&spec).unwrap();
    assert_eq!(validation.resources.memory_bytes, 16 * 1024 * 1024);
    assert_eq!(validation.resources.extension_memory_bytes, 0);
    assert_eq!(validation.resources.processes, 12);
    assert_eq!(validation.resources.cpus, 3);
    assert_eq!(validation.estimated_memory_bytes, 16 * 1024 * 1024);
}

#[test]
fn validate_has_no_host_mutation_and_spawn_reuses_its_preflight() {
    let base = std::env::temp_dir().join(format!("hl-validation-no-state-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(base.join("root")).unwrap();
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.filesystem.root = Some(TreeSource::HostDirectory(base.join("root")));
    spec.cache.directory = Some(base.join("missing-cache"));
    spec.cache.policy = hl_engine::spec::CachePolicy::ReadWrite;
    Engine::new().validate(&spec).unwrap();
    assert!(
        !base.join("missing-cache").exists(),
        "validation created cache state"
    );

    spec.identity.uid = Some(u32::MAX);
    let validation = Engine::new().validate(&spec).unwrap_err();
    let spawn = Engine::new().spawn(spec, ProcessIo::default()).unwrap_err();
    let hl_engine::SpawnError::Spec(spawn) = spawn else {
        panic!("spawn bypassed typed preflight")
    };
    assert_eq!(spawn.category, validation.category);
    assert_eq!(spawn.field, validation.field);
    assert_eq!(spawn.resource, validation.resource);
}

struct BasicHandles {
    closes: Arc<std::sync::atomic::AtomicUsize>,
}
struct BasicHandle {
    state: std::sync::Mutex<(Vec<u8>, usize)>,
    closes: Arc<std::sync::atomic::AtomicUsize>,
}

impl Handles for BasicHandles {
    fn open(&self, request: OpenRequest) -> Result<Box<dyn OpenHandle>, LinuxError> {
        if request.service != ServiceId(77) {
            return Err(LinuxError {
                errno: 19,
                context: "unknown test service".into(),
            });
        }
        Ok(Box::new(BasicHandle {
            state: std::sync::Mutex::new((b"hello".to_vec(), 0)),
            closes: self.closes.clone(),
        }))
    }
}

impl OpenHandle for BasicHandle {
    fn read(&self, request: ReadRequest) -> Result<Vec<u8>, LinuxError> {
        let mut state = self.state.lock().map_err(|_| LinuxError {
            errno: 5,
            context: "poisoned test service".into(),
        })?;
        let count = usize::try_from(request.length)
            .unwrap_or(usize::MAX)
            .min(state.0.len().saturating_sub(state.1));
        let bytes = state.0[state.1..state.1 + count].to_vec();
        state.1 += count;
        Ok(bytes)
    }

    fn write(&self, request: WriteRequest) -> Result<usize, LinuxError> {
        let mut state = self.state.lock().map_err(|_| LinuxError {
            errno: 5,
            context: "poisoned test service".into(),
        })?;
        state.0.extend_from_slice(&request.bytes);
        Ok(request.bytes.len())
    }

    fn readiness(&self, _interest: Interest) -> Result<Readiness, LinuxError> {
        Ok(Readiness {
            states: BTreeSet::from([ReadyState::Readable, ReadyState::Writable]),
        })
    }

    fn flush(&self) -> Result<(), LinuxError> {
        Ok(())
    }

    fn close(self: Box<Self>) {
        self.closes
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
}

fn handles_extension() -> ExtensionSpec {
    ExtensionSpec {
        provider: ProviderId::new("engine.handles").unwrap(),
        version: Version::new(1, 0),
        required: true,
        required_features: BTreeSet::from([
            Feature::new("read").unwrap(),
            Feature::new("write").unwrap(),
            Feature::new("poll").unwrap(),
            Feature::new("ofd-lifecycle").unwrap(),
        ]),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig {
            schema: "engine.handles/v1".into(),
            bytes: b"HOST_SECRET_MUST_NOT_LEAK".to_vec(),
        },
        namespace: vec![NamespaceEntry::Service(ServiceEntry {
            path: "/run/provider/basic".into(),
            metadata: Metadata {
                mode: 0o660,
                uid: 0,
                gid: 0,
            },
            service: ServiceId(77),
        })],
        services: vec![ServiceRegistration {
            id: ServiceId(77),
            operations: BTreeSet::from([
                HandleOperation::Read,
                HandleOperation::Write,
                HandleOperation::Poll,
            ]),
            max_request_bytes: 4096,
        }],
        memory: Vec::new(),
        environment: Vec::new(),
    }
}

fn memory_extension() -> ExtensionSpec {
    ExtensionSpec {
        provider: ProviderId::new("engine.handles").unwrap(),
        version: Version::new(1, 0),
        required: true,
        required_features: BTreeSet::from([Feature::new("memory-allocation").unwrap()]),
        optional_features: BTreeSet::new(),
        config: ExtensionConfig::empty("engine.handles/v1"),
        namespace: Vec::new(),
        services: Vec::new(),
        memory: vec![MemoryRequirement {
            size: 4096,
            alignment: 4096,
            protections: Protections {
                read: true,
                write: true,
                execute: false,
            },
            sharing: Sharing::Shared,
            inheritance: Inheritance::Retain,
        }],
        environment: Vec::new(),
    }
}

struct TestMemory {
    allocated: Arc<std::sync::atomic::AtomicUsize>,
    released: Arc<std::sync::atomic::AtomicUsize>,
}

struct InvalidMemory {
    released: Arc<std::sync::atomic::AtomicUsize>,
}

impl Memory for InvalidMemory {
    fn allocate(&self, request: AllocationRequest) -> Result<HostResource, ResourceError> {
        Ok(HostResource {
            id: ResourceId(42),
            regions: vec![Region {
                offset: 1,
                size: request.size,
                protections: request.protections,
            }],
            handles: Vec::new(),
            coherency: Coherency::Coherent,
            inheritance: Inheritance::Retain,
        })
    }

    fn import(&self, _descriptor: &ResourceDescriptor) -> Result<HostResource, ResourceError> {
        unreachable!("test does not import resources")
    }

    fn release(&self, resource: ResourceId) {
        assert_eq!(resource, ResourceId(42));
        self.released
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }
}

impl Memory for TestMemory {
    fn allocate(&self, request: AllocationRequest) -> Result<HostResource, ResourceError> {
        self.allocated
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        Ok(HostResource {
            id: ResourceId(41),
            regions: vec![Region {
                offset: 0,
                size: request.size,
                protections: request.protections,
            }],
            handles: Vec::new(),
            coherency: Coherency::Coherent,
            inheritance: Inheritance::Retain,
        })
    }

    fn import(&self, _descriptor: &ResourceDescriptor) -> Result<HostResource, ResourceError> {
        unreachable!("test does not import resources")
    }

    fn release(&self, resource: ResourceId) {
        assert_eq!(resource, ResourceId(41));
        self.released
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }
}

#[test]
fn provider_memory_is_allocated_with_launch_authority_and_released_after_exit() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions.push(memory_extension());
    let validation = Engine::new().validate(&spec).unwrap();
    assert_eq!(validation.resources.extension_memory_bytes, 4096);

    let error = Engine::new()
        .spawn(spec.clone(), ProcessIo::default())
        .unwrap_err();
    let hl_engine::SpawnError::Spec(error) = error else {
        panic!("missing memory authority reached launch")
    };
    assert_eq!(error.field, "extensions.authority");

    let allocated = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let released = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut authorities = Authorities::new();
    authorities
        .grant(
            ProviderId::new("engine.handles").unwrap(),
            ProviderAuthority {
                handles: None,
                memory: Some(Arc::new(TestMemory {
                    allocated: allocated.clone(),
                    released: released.clone(),
                })),
            },
        )
        .unwrap();
    let machine = Engine::new()
        .spawn_with_authorities(spec, ProcessIo::default(), authorities)
        .unwrap();
    assert_eq!(allocated.load(std::sync::atomic::Ordering::SeqCst), 1);
    assert_eq!(released.load(std::sync::atomic::Ordering::SeqCst), 0);
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    assert_eq!(released.load(std::sync::atomic::Ordering::SeqCst), 1);
}

#[test]
fn provider_memory_result_is_validated_and_rolled_back_before_process_start() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/does/not/run");
    spec.extensions.push(memory_extension());
    let released = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut authorities = Authorities::new();
    authorities
        .grant(
            ProviderId::new("engine.handles").unwrap(),
            ProviderAuthority {
                handles: None,
                memory: Some(Arc::new(InvalidMemory {
                    released: released.clone(),
                })),
            },
        )
        .unwrap();
    let error = Engine::new()
        .spawn_with_authorities(spec, ProcessIo::default(), authorities)
        .unwrap_err();
    let hl_engine::SpawnError::Spec(error) = error else {
        panic!("invalid provider resource reached process start")
    };
    assert_eq!(error.field, "extensions.memory");
    assert_eq!(released.load(std::sync::atomic::Ordering::SeqCst), 1);
}

#[test]
fn provider_memory_enforces_its_discovered_contract_limit() {
    let mut extension = memory_extension();
    let requirement = extension.memory[0].clone();
    extension.memory = vec![requirement; 65];
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(extension);
    let error = Engine::new().validate(&spec).unwrap_err();
    assert_eq!(error.category, SpecErrorCategory::Limit);
    assert_eq!(error.field, "extensions[0].memory");
}

#[test]
fn handle_services_require_launch_scoped_authority_and_reject_unadvertised_operations() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    spec.extensions.push(handles_extension());
    assert!(Engine::new().validate(&spec).is_ok());
    let error = Engine::new()
        .spawn(spec.clone(), ProcessIo::default())
        .unwrap_err();
    let hl_engine::SpawnError::Spec(error) = error else {
        panic!("missing authority reached activation")
    };
    assert_eq!(error.field, "extensions.authority");

    let mut mismatch = HandlesAuthority::new();
    mismatch
        .grant(
            ProviderId::new("unrelated.provider").unwrap(),
            Arc::new(BasicHandles {
                closes: Arc::new(std::sync::atomic::AtomicUsize::new(0)),
            }),
        )
        .unwrap();
    let error = Engine::new()
        .spawn_with_authority(spec.clone(), ProcessIo::default(), mismatch)
        .unwrap_err();
    let hl_engine::SpawnError::Spec(error) = error else {
        panic!("mismatched authority reached activation")
    };
    assert_eq!(error.field, "extensions.authority");

    spec.extensions[0].services[0]
        .operations
        .insert(HandleOperation::Ioctl);
    assert_eq!(
        Engine::new().validate(&spec).unwrap_err().category,
        SpecErrorCategory::Unsupported
    );

    let mut quota = handles_extension();
    quota.services = (1..=65)
        .map(|id| ServiceRegistration {
            id: ServiceId(id),
            operations: BTreeSet::from([HandleOperation::Read]),
            max_request_bytes: 4096,
        })
        .collect();
    let mut quota_spec = MachineSpec::new(Guest::Aarch64, "/bin/true");
    quota_spec.extensions.push(quota);
    let error = Engine::new().validate(&quota_spec).unwrap_err();
    assert_eq!(error.category, SpecErrorCategory::Limit);
    assert_eq!(error.field, "extensions[0].services");
}

#[test]
fn typed_handles_authority_runs_a_real_projected_service_guest() {
    let source = std::env::temp_dir().join(format!("hl-provider-basic-{}.c", std::process::id()));
    fs::write(&source, r#"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "cloexec")) {
    int fd = atoi(argv[2]); return fcntl(fd, F_GETFD) == -1 ? 0 : 20;
  }
  char bytes[6] = {0};
  int fd = open("/run/provider/basic", O_RDWR | O_CLOEXEC);
  struct pollfd p = {.fd = fd, .events = POLLIN | POLLOUT};
  if (fd < 0 || poll(&p, 1, 1000) != 1 || (p.revents & (POLLIN | POLLOUT)) != (POLLIN | POLLOUT)) return 10;
  int copy = dup(fd); if (copy < 0 || read(fd, bytes, 2) != 2 || memcmp(bytes, "he", 2)) return 11;
  pid_t child = fork(); if (child < 0) return 12;
  if (!child) _exit(read(copy, bytes, 3) == 3 && !memcmp(bytes, "llo", 3) ? 0 : 13);
  int status; if (waitpid(child, &status, 0) != child || status || write(copy, "!", 1) != 1) return 14;
  if (getenv("HOST_SECRET_MUST_NOT_LEAK")) return 15;
  close(copy);
  char number[32]; snprintf(number, sizeof(number), "%d", fd);
  execl("/tmp/provider-basic", "/tmp/provider-basic", "cloexec", number, NULL);
  return 16;
}
"#).unwrap();
    let guest = rootfs().join("tmp/provider-basic");
    assert!(Command::new("cc")
        .args(["-static", "-O2", "-o"])
        .arg(&guest)
        .arg(&source)
        .status()
        .unwrap()
        .success());
    let _ = fs::remove_file(source);

    let mut spec = MachineSpec::new(Guest::Aarch64, "/tmp/provider-basic");
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.extensions.push(handles_extension());
    let closes = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut authority = HandlesAuthority::new();
    authority
        .grant(
            ProviderId::new("engine.handles").unwrap(),
            Arc::new(BasicHandles {
                closes: closes.clone(),
            }),
        )
        .unwrap();
    let machine = Engine::new()
        .spawn_with_authority(spec, ProcessIo::default(), authority)
        .unwrap();
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    for _ in 0..100 {
        if closes.load(std::sync::atomic::Ordering::Relaxed) == 1 {
            break;
        }
        std::thread::yield_now();
    }
    assert_eq!(closes.load(std::sync::atomic::Ordering::Relaxed), 1);
}

#[test]
fn controlling_terminal_and_projected_service_run_together() {
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    spec.process.argv.extend([
        "-c".into(),
        "exec 3</run/provider/basic; IFS= read -r -n 5 value <&3; test \"$value\" = hello".into(),
    ]);
    spec.process.terminal = Some(hl_engine::Size::new(24, 80).unwrap());
    spec.extensions.push(handles_extension());
    let closes = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut authority = HandlesAuthority::new();
    authority
        .grant(
            ProviderId::new("engine.handles").unwrap(),
            Arc::new(BasicHandles {
                closes: closes.clone(),
            }),
        )
        .unwrap();

    let mut machine = Engine::new()
        .spawn_with_authority(spec, ProcessIo::default(), authority)
        .unwrap();
    let mut terminal = machine.take_terminal().expect("controlling terminal");
    let exit = machine.wait().unwrap();
    let mut output = String::new();
    terminal.read_to_string(&mut output).unwrap();
    assert_eq!(exit, Exit::Code(0), "terminal output: {output:?}");
    for _ in 0..100 {
        if closes.load(std::sync::atomic::Ordering::Relaxed) == 1 {
            break;
        }
        std::thread::yield_now();
    }
    assert_eq!(closes.load(std::sync::atomic::Ordering::Relaxed), 1);
}
