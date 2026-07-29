use hl_engine::network::Namespace;
use hl_engine::{Config, Engine, Exit, Guest, Stdio};
use std::fs;
use std::io::{Read, Write};
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;
use std::process::Command;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

#[path = "support/engine_env.rs"]
mod engine_env;
#[path = "support/host.rs"]
mod host;

fn rootfs() -> &'static PathBuf {
    static ROOTFS: OnceLock<PathBuf> = OnceLock::new();
    ROOTFS.get_or_init(|| {
        let path = std::env::temp_dir().join(format!("hl-alpine-{}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir(&path).unwrap();
        let fixture = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("assets/alpine/alpine-minirootfs-3.24.1-aarch64.tar.gz");
        let status = Command::new("tar")
            .args(["-xzf"])
            .arg(fixture)
            .arg("-C")
            .arg(&path)
            .status()
            .unwrap();
        assert!(status.success(), "cannot extract pinned Alpine fixture");
        path
    })
}

fn alive(pid: u32) -> bool {
    Command::new("kill")
        .args(["-0", &pid.to_string()])
        .status()
        .is_ok_and(|status| status.success())
}

#[test]
fn private_udp_loopback_preserves_datagrams_and_readiness_across_fork() {
    if engine_env::skip_without_rootfs(
        "private_udp_loopback_preserves_datagrams_and_readiness_across_fork",
    ) {
        return;
    }
    let mut child = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(
            Config::new()
                .root(rootfs())
                .network(true)
                .network_namespace(Namespace::new("rust-private-udp").unwrap()),
        )
        .args([
            "-c",
            "( echo UDP_PRIVATE_OK | nc -u -l -p 19231 -w 2 ) & sleep 0.2; echo request | nc -u -w 2 127.0.0.1 19231",
        ])
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut output = String::new();
    child
        .take_stdout()
        .unwrap()
        .read_to_string(&mut output)
        .unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Code(0));
    assert!(output.lines().any(|line| line == "request"));
    assert!(output.lines().any(|line| line == "UDP_PRIVATE_OK"));
}

#[test]
fn private_udp_loopback_is_shared_by_independent_launches() {
    if engine_env::skip_without_rootfs("private_udp_loopback_is_shared_by_independent_launches") {
        return;
    }
    let config = || {
        Config::new()
            .root(rootfs())
            .network(true)
            .network_namespace(Namespace::new("rust-private-udp-launches").unwrap())
    };
    let mut server = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(config())
        .args([
            "-c",
            "echo UDP_LAUNCH_OK | nc -u -l -s 127.0.0.1 -p 19232 -w 3",
        ])
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    std::thread::sleep(Duration::from_millis(300));
    let mut client = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(config())
        .args(["-c", "echo request | nc -u -w 2 127.0.0.1 19232"])
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut reply = String::new();
    client
        .take_stdout()
        .unwrap()
        .read_to_string(&mut reply)
        .unwrap();
    assert_eq!(client.wait().unwrap(), Exit::Code(0));
    assert_eq!(reply.trim(), "UDP_LAUNCH_OK");
    let mut request = String::new();
    server
        .take_stdout()
        .unwrap()
        .read_to_string(&mut request)
        .unwrap();
    assert_eq!(server.wait().unwrap(), Exit::Code(0));
    assert_eq!(request.trim(), "request");
}

#[test]
fn process_domain_stops_double_forked_new_session_without_touching_siblings() {
    if engine_env::skip_without_rootfs(
        "process_domain_stops_double_forked_new_session_without_touching_siblings",
    ) {
        return;
    }
    let pid_file = rootfs().join("tmp/domain-daemon.pid");
    let _ = fs::remove_file(&pid_file);
    let mut sibling = Command::new("sleep").arg("30").spawn().unwrap();
    let child = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()))
        .args([
            "-c",
            "( setsid sh -c 'sleep 30 </dev/null >/dev/null 2>&1 & echo $! >/tmp/domain-daemon.pid' </dev/null >/dev/null 2>&1 ) & exit 0",
        ])
        .spawn()
        .unwrap();
    let domain = child.domain();
    assert_eq!(child.wait().unwrap(), Exit::Code(0));
    let deadline = Instant::now() + Duration::from_secs(3);
    let daemon = loop {
        if let Ok(text) = fs::read_to_string(&pid_file) {
            if let Ok(pid) = text.trim().parse::<u32>() {
                if alive(pid) {
                    break pid;
                }
            }
        }
        assert!(Instant::now() < deadline, "daemon did not publish its pid");
        std::thread::sleep(Duration::from_millis(10));
    };
    domain.terminate().unwrap();
    domain.terminate().unwrap();
    let deadline = Instant::now() + Duration::from_secs(3);
    while alive(daemon) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(!alive(daemon), "domain member survived termination");
    assert!(alive(sibling.id()), "unrelated sibling was terminated");
    sibling.kill().unwrap();
    sibling.wait().unwrap();
}

#[test]
fn process_domain_termination_tolerates_an_unreaped_init() {
    if engine_env::skip_without_rootfs("process_domain_termination_tolerates_an_unreaped_init") {
        return;
    }
    let mut child = Engine::new()
        .command(Guest::Aarch64, "/bin/sleep")
        .config(Config::new().root(rootfs()))
        .arg("30")
        .spawn()
        .unwrap();
    let domain = child.domain();
    child.force_stop().unwrap();
    domain.terminate().unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Signal(9));
}

#[test]
fn public_api_runs_real_alpine_shell_with_process_io() {
    if engine_env::skip_without_rootfs("public_api_runs_real_alpine_shell_with_process_io") {
        return;
    }
    let engine = Engine::new();
    let config = Config::new()
        .root(rootfs())
        .working_dir("/tmp")
        .env("HL_TEST", "alpine");
    let command = engine
        .command(Guest::Aarch64, "/bin/sh")
        .config(config)
        .args([
                "-c",
                "read line; printf 'out:%s:%s:%s\\n' \"$HL_TEST\" \"$PWD\" \"$line\"; printf 'err:%s\\n' \"$1\" >&2; exit 17",
                "shell",
                "argument",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.spawn().unwrap();
    assert_ne!(child.id(), 0);
    child.take_stdin().unwrap().write_all(b"input\n").unwrap();
    let mut stdout = String::new();
    let mut stderr = String::new();
    child
        .take_stdout()
        .unwrap()
        .read_to_string(&mut stdout)
        .unwrap();
    child
        .take_stderr()
        .unwrap()
        .read_to_string(&mut stderr)
        .unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Code(17));
    assert_eq!(stdout, "out:alpine:/tmp:input\n");
    assert_eq!(stderr, "err:argument\n");
}

#[test]
fn production_true_has_empty_stdout_and_stderr() {
    if engine_env::skip_without_rootfs("production_true_has_empty_stdout_and_stderr") {
        return;
    }
    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/true")
        .config(Config::new().root(rootfs()))
        .output()
        .unwrap();
    assert_eq!(output.exit, Exit::Code(0));
    assert!(
        output.stdout.is_empty(),
        "unexpected stdout: {:?}",
        output.stdout
    );
    assert!(
        output.stderr.is_empty(),
        "unexpected stderr: {:?}",
        output.stderr
    );
}

#[test]
fn translation_cache_publishes_shell_and_fork_exec_utility_epochs() {
    let cache =
        std::env::temp_dir().join(format!("hl-alpine-pcache-fork-exec-{}", std::process::id()));
    let _ = fs::remove_dir_all(&cache);

    let mut cold_entries = Vec::new();
    for round in 0..100 {
        let output = Engine::new()
            .command(Guest::Aarch64, "/bin/sh")
            .config(Config::new().root(rootfs()).translation_cache(&cache))
            .args(["-c", "awk '{print $1}' /proc/self/stat; :"])
            .output()
            .unwrap();

        assert_eq!(
            output.exit,
            Exit::Code(0),
            "cached shell fork/exec run {round} failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(
            String::from_utf8_lossy(&output.stdout)
                .trim()
                .parse::<u32>()
                .is_ok(),
            "run {round} returned an invalid process id: {:?}",
            String::from_utf8_lossy(&output.stdout)
        );
        if round == 0 {
            cold_entries = fs::read_dir(&cache)
                .unwrap()
                .map(|entry| {
                    let entry = entry.unwrap();
                    let metadata = entry.metadata().unwrap();
                    (
                        entry.file_name(),
                        metadata.len(),
                        metadata.modified().unwrap(),
                    )
                })
                .collect();
            cold_entries.sort_by(|left, right| left.0.cmp(&right.0));
            assert!(
                cold_entries.len() >= 2,
                "the cold shell and fork-exec'd utility must each publish a cache, found {cold_entries:?}"
            );
        }
    }

    let mut warm_entries = fs::read_dir(&cache)
        .unwrap()
        .map(|entry| {
            let entry = entry.unwrap();
            let metadata = entry.metadata().unwrap();
            (
                entry.file_name(),
                metadata.len(),
                metadata.modified().unwrap(),
            )
        })
        .collect::<Vec<_>>();
    warm_entries.sort_by(|left, right| left.0.cmp(&right.0));
    assert_eq!(
        warm_entries, cold_entries,
        "warm shell and utility epochs must reuse their identity-keyed caches without republishing"
    );

    fs::remove_dir_all(cache).unwrap();
}

#[test]
#[cfg(unix)]
fn translation_cache_reuses_a_direct_launch_without_republishing() {
    let cache =
        std::env::temp_dir().join(format!("hl-alpine-pcache-direct-{}", std::process::id()));
    let _ = fs::remove_dir_all(&cache);

    let launch = || {
        Engine::new()
            .command(Guest::Aarch64, "/bin/true")
            .config(Config::new().root(rootfs()).translation_cache(&cache))
            .output()
            .unwrap()
    };

    assert_eq!(launch().exit, Exit::Code(0));
    let entry = fs::read_dir(&cache).unwrap().next().unwrap().unwrap();
    assert_eq!(fs::read_dir(&cache).unwrap().count(), 1);
    let inode = entry.metadata().unwrap().ino();

    for _ in 1..100 {
        assert_eq!(launch().exit, Exit::Code(0));
        let current = fs::read_dir(&cache).unwrap().next().unwrap().unwrap();
        assert_eq!(
            current.metadata().unwrap().ino(),
            inode,
            "a warm direct launch unexpectedly republished its cache"
        );
    }

    fs::remove_dir_all(cache).unwrap();
}

#[test]
fn output_drains_large_stdout_and_stderr_concurrently() {
    if engine_env::skip_without_rootfs("output_drains_large_stdout_and_stderr_concurrently") {
        return;
    }
    let engine = Engine::new();
    let output = engine
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()))
        .args([
            "-c",
            "i=0; while [ $i -lt 6000 ]; do echo out-$i; echo err-$i >&2; i=$((i+1)); done",
        ])
        .output()
        .unwrap();
    assert_eq!(output.exit, Exit::Code(0));
    assert!(output.stdout.len() > 50_000);
    assert!(output.stderr.len() > 50_000);
    assert!(output.stdout.ends_with(b"out-5999\n"));
    assert!(output.stderr.ends_with(b"err-5999\n"));
}

#[test]
fn external_term_reaches_the_guest_handler() {
    if engine_env::skip_without_rootfs("external_term_reaches_the_guest_handler") {
        return;
    }
    let mut child = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()))
        .args([
            "-c",
            "trap 'printf GOT_TERM; exit 0' TERM; printf READY; while :; do :; done",
        ])
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let id = child.id().to_string();
    let mut stdout = child.take_stdout().unwrap();
    let mut output = [0_u8; 5];
    stdout.read_exact(&mut output).unwrap();
    assert_eq!(&output, b"READY");
    assert!(Command::new("kill")
        .args(["-TERM", &id])
        .status()
        .unwrap()
        .success());
    let mut rest = Vec::new();
    stdout.read_to_end(&mut rest).unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Code(0));
    assert_eq!(rest, b"GOT_TERM");
}

#[test]
fn piped_streams_enforce_their_direction() {
    if engine_env::skip_without_rootfs("piped_streams_enforce_their_direction") {
        return;
    }
    let engine = Engine::new();
    let mut child = engine
        .command(Guest::Aarch64, "/bin/echo")
        .config(Config::new().root(rootfs()))
        .arg("hello")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut input = child.take_stdin().unwrap();
    let mut output = child.take_stdout().unwrap();
    assert!(input.read(&mut [0]).is_err());
    assert!(output.write_all(b"wrong direction").is_err());
    drop(input);
    let mut text = String::new();
    output.read_to_string(&mut text).unwrap();
    assert_eq!(text, "hello\n");
    assert_eq!(child.wait().unwrap(), Exit::Code(0));
}

#[test]
fn guest_ownership_is_seeded_and_shared_by_inode() {
    if engine_env::skip_without_rootfs("guest_ownership_is_seeded_and_shared_by_inode") {
        return;
    }
    let name = format!("owner-{}", std::process::id());
    // A guest path, so `/`-separated and built as a string. `PathBuf::from("tmp").join(&name)`
    // renders as `tmp\owner-N` on Windows, which would be handed to `owner()` and interpolated into
    // the guest script as a path the guest has never heard of -- silently, since nothing here
    // inspects it. Every guest path in this file is written the way the guest reads it.
    let relative = format!("tmp/{name}");
    let hard_relative = format!("tmp/{name}-hard");
    let file = rootfs().join(&relative);
    let hard = rootfs().join(&hard_relative);
    fs::write(&file, b"ownership\n").unwrap();
    fs::hard_link(&file, &hard).unwrap();

    let script = format!(
        "stat -c '%u:%g' /{relative}; stat -c '%u:%g' /{hard_relative}; \
         (chown 56:78 /{hard_relative}); stat -c '%u:%g' /{relative}"
    );
    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(
            Config::new()
                .root(rootfs())
                .owner(relative.as_str(), 12, 34),
        )
        .args(["-c", &script])
        .output()
        .unwrap();

    assert_eq!(output.exit, Exit::Code(0));
    assert_eq!(output.stdout, b"12:34\n12:34\n56:78\n");
    assert!(output.stderr.is_empty());
    fs::remove_file(hard).unwrap();
    fs::remove_file(file).unwrap();
}

#[test]
fn guest_ownership_is_seeded_for_overlay_lower_entries() {
    if engine_env::skip_without_rootfs("guest_ownership_is_seeded_for_overlay_lower_entries") {
        return;
    }
    let name = format!("overlay-owner-{}", std::process::id());
    let relative = format!("tmp/{name}"); // A guest path; see the note above.
    let file = rootfs().join(&relative);
    fs::write(&file, b"ownership\n").unwrap();
    let overlay = std::env::temp_dir().join(format!("hl-{name}"));
    let _ = fs::remove_dir_all(&overlay);
    fs::create_dir(&overlay).unwrap();
    let upper = overlay.join("upper");
    let work = overlay.join("work");
    fs::create_dir(&upper).unwrap();
    fs::create_dir(&work).unwrap();

    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/stat")
        .config(
            Config::new()
                .overlay(vec![rootfs().clone()], upper, work)
                .owner(relative.as_str(), 12, 34),
        )
        .args(["-c", "%u:%g", &format!("/{relative}")])
        .output()
        .unwrap();

    assert_eq!(output.exit, Exit::Code(0));
    assert_eq!(output.stdout, b"12:34\n");
    assert!(output.stderr.is_empty());
    fs::remove_dir_all(overlay).unwrap();
    fs::remove_file(file).unwrap();
}

#[test]
fn image_symlink_chain_can_open_synthetic_standard_error() {
    if engine_env::skip_without_rootfs("image_symlink_chain_can_open_synthetic_standard_error") {
        return;
    }
    let name = format!("stderr-alias-{}", std::process::id());
    let relative = format!("tmp/{name}"); // A guest path; see the note above.
    let link = rootfs().join(&relative);
    let _ = fs::remove_file(&link);
    // The link target is a *guest* path that does not exist on the host, so this is a dangling
    // symlink on both hosts by construction. `unwrap` rather than a skip: the gate above already
    // established that this host can run a guest, and a host that can do that and cannot create a
    // symlink is a defect worth failing on.
    host::symlink(std::path::Path::new("/dev/stderr"), &link).unwrap();

    let command = format!("test -d /proc/self/fd/ && printf SYNTHETIC_STDERR_OK > /{relative}");
    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()))
        .args(["-c", &command])
        .output()
        .unwrap();

    assert_eq!(output.exit, Exit::Code(0));
    assert_eq!(output.stderr, b"SYNTHETIC_STDERR_OK");
    fs::remove_file(link).unwrap();
}

#[test]
fn overlay_image_symlink_chain_can_open_synthetic_standard_output() {
    if engine_env::skip_without_rootfs(
        "overlay_image_symlink_chain_can_open_synthetic_standard_output",
    ) {
        return;
    }
    let name = format!("stdout-alias-{}", std::process::id());
    let relative = format!("tmp/{name}"); // A guest path; see the note above.
    let link = rootfs().join(&relative);
    let _ = fs::remove_file(&link);
    host::symlink(std::path::Path::new("/dev/stdout"), &link).unwrap();
    let overlay = std::env::temp_dir().join(format!("hl-{name}"));
    let upper = overlay.join("upper");
    let work = overlay.join("work");
    fs::create_dir_all(&upper).unwrap();
    fs::create_dir_all(&work).unwrap();

    let command = format!("printf SYNTHETIC_STDOUT_OK > /{relative}");
    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().overlay(vec![rootfs().clone()], upper, work))
        .args(["-c", &command])
        .output()
        .unwrap();

    assert_eq!(output.exit, Exit::Code(0));
    assert_eq!(output.stdout, b"SYNTHETIC_STDOUT_OK");
    fs::remove_dir_all(overlay).unwrap();
    fs::remove_file(link).unwrap();
}
