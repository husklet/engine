use hl_engine::{Config, Engine, Exit, Guest, Size};
use std::{
    fs,
    io::{Read, Write},
    path::PathBuf,
    process::Command,
    sync::OnceLock,
};

fn rootfs() -> &'static PathBuf {
    static ROOTFS: OnceLock<PathBuf> = OnceLock::new();
    ROOTFS.get_or_init(|| {
        let path = std::env::temp_dir().join(format!("hl-terminal-alpine-{}", std::process::id()));
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

fn read_until(terminal: &mut hl_engine::Terminal, marker: &[u8]) -> Vec<u8> {
    let mut output = Vec::new();
    let mut buffer = [0_u8; 512];
    while !output.windows(marker.len()).any(|window| window == marker) {
        let count = terminal.read(&mut buffer).unwrap();
        assert_ne!(count, 0, "terminal closed before marker");
        output.extend_from_slice(&buffer[..count]);
    }
    output
}

#[test]
fn terminal_is_controlling_merged_resizable_and_reaped() {
    let initial = Size::new(24, 80).unwrap();
    let mut child = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()).env("TERM", "xterm"))
        .args([
            "-c",
            "t0=0; t1=0; t2=0; [ -t 0 ] && t0=1; [ -t 1 ] && t1=1; [ -t 2 ] && t2=1; printf 'TTY=%s%s%s TERM=%s\\n' \"$t0\" \"$t1\" \"$t2\" \"$TERM\"; stty size; printf 'OUT\\n'; printf 'ERR\\n' >&2; printf 'READY\\n'; read line; stty size; printf 'LINE=%s\\n' \"$line\"; exit 23",
        ])
        .terminal(initial)
        .spawn()
        .unwrap();
    let mut terminal = child.take_terminal().unwrap();
    assert!(child.take_stdin().is_none());
    assert!(child.take_stdout().is_none());
    assert!(child.take_stderr().is_none());
    let mut output = read_until(&mut terminal, b"READY");
    terminal.resize(Size::new(41, 109).unwrap()).unwrap();
    terminal.write_all(b"hello\n").unwrap();
    terminal.read_to_end(&mut output).unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Code(23));
    let text = String::from_utf8_lossy(&output).replace('\r', "");
    assert!(text.contains("TTY=111 TERM=xterm"), "{text}");
    assert!(text.contains("24 80"), "{text}");
    assert!(text.contains("OUT\nERR\n"), "{text}");
    assert!(text.contains("41 109"), "{text}");
    assert!(text.contains("LINE=hello"), "{text}");
}

#[test]
fn terminal_size_rejects_empty_dimensions() {
    assert!(Size::new(0, 80).is_err());
    assert!(Size::new(24, 0).is_err());
}
