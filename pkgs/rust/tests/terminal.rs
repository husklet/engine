use hl_engine::{Config, Engine, Exit, Guest, Size};
use std::{
    fs,
    io::{Read, Write},
    path::PathBuf,
    process::Command,
    sync::{mpsc, OnceLock},
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

fn open_files() -> usize {
    #[cfg(target_os = "linux")]
    let directory = "/proc/self/fd";
    #[cfg(target_os = "macos")]
    let directory = "/dev/fd";
    fs::read_dir(directory).unwrap().count()
}

#[test]
fn terminal_is_controlling_merged_resizable_and_reaped() {
    let files = open_files();
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

    let mut reader = terminal.try_clone().unwrap();
    let (ready, waiting) = mpsc::channel();
    let output = std::thread::spawn(move || {
        let mut output = Vec::new();
        let mut buffer = [0_u8; 512];
        let mut announced = false;
        loop {
            let count = reader.read(&mut buffer).unwrap();
            if count == 0 {
                break;
            }
            output.extend_from_slice(&buffer[..count]);
            if !announced && output.windows(5).any(|window| window == b"READY") {
                ready.send(()).unwrap();
                announced = true;
            }
        }
        output
    });
    waiting.recv().unwrap();
    terminal.resize(Size::new(41, 109).unwrap()).unwrap();
    terminal.write_all(b"hello\n").unwrap();
    assert_eq!(child.wait().unwrap(), Exit::Code(23));
    let output = output.join().unwrap();
    let text = String::from_utf8_lossy(&output).replace('\r', "");
    assert!(text.contains("TTY=111 TERM=xterm"), "{text}");
    assert!(text.contains("24 80"), "{text}");
    assert!(text.contains("OUT\nERR\n"), "{text}");
    assert!(text.contains("41 109"), "{text}");
    assert!(text.contains("LINE=hello"), "{text}");
    drop(terminal);
    assert_eq!(
        open_files(),
        files,
        "terminal launch leaked a host descriptor"
    );
}

#[test]
fn terminal_size_rejects_empty_dimensions() {
    assert!(Size::new(0, 80).is_err());
    assert!(Size::new(24, 0).is_err());
}
