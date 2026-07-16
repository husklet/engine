use hl_engine::{Config, Engine, Exit, Guest, Stdio};
use std::fs;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::Command;
use std::sync::OnceLock;

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

#[test]
fn public_api_runs_real_alpine_shell_with_process_io() {
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
fn output_drains_large_stdout_and_stderr_concurrently() {
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
    let name = format!("owner-{}", std::process::id());
    let relative = PathBuf::from("tmp").join(&name);
    let hard_relative = PathBuf::from("tmp").join(format!("{name}-hard"));
    let file = rootfs().join(&relative);
    let hard = rootfs().join(&hard_relative);
    fs::write(&file, b"ownership\n").unwrap();
    fs::hard_link(&file, &hard).unwrap();

    let script = format!(
        "stat -c '%u:%g' /{0}; stat -c '%u:%g' /{1}; (chown 56:78 /{1}); stat -c '%u:%g' /{0}",
        relative.display(),
        hard_relative.display()
    );
    let output = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()).owner(&relative, 12, 34))
        .args(["-c", &script])
        .output()
        .unwrap();

    assert_eq!(output.exit, Exit::Code(0));
    assert_eq!(output.stdout, b"12:34\n12:34\n56:78\n");
    assert!(output.stderr.is_empty());
    fs::remove_file(hard).unwrap();
    fs::remove_file(file).unwrap();
}
