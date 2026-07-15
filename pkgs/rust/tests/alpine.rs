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
