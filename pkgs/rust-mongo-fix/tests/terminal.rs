use hl_engine::{Config, Engine, Exit, Guest, MachineSpec, ProcessIo, Size, TreeSource};
use std::{
    fs,
    io::{Read, Write},
    path::PathBuf,
    process::Command,
    sync::{mpsc, Mutex, OnceLock},
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

fn terminal_lock() -> std::sync::MutexGuard<'static, ()> {
    static LOCK: Mutex<()> = Mutex::new(());
    LOCK.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[test]
fn typed_terminal_preserves_explicit_environment() {
    let _terminal_lock = terminal_lock();
    let mut spec = MachineSpec::new(Guest::Aarch64, "/bin/sh");
    spec.process
        .argv
        .extend(["-c".into(), "printf 'TERM=[%s]\\n' \"$TERM\"".into()]);
    spec.process.env.push(("TERM".into(), "screen".into()));
    spec.process.terminal = Some(Size::new(24, 80).unwrap());
    spec.filesystem.root = Some(TreeSource::HostDirectory(rootfs().clone()));
    let mut machine = Engine::new().spawn(spec, ProcessIo::default()).unwrap();
    let mut terminal = machine.take_terminal().unwrap();
    let mut reader = terminal.try_clone().unwrap();
    let output = std::thread::spawn(move || {
        let mut output = Vec::new();
        reader.read_to_end(&mut output).unwrap();
        output
    });
    assert_eq!(machine.wait().unwrap(), Exit::Code(0));
    drop(terminal);
    let text = String::from_utf8_lossy(&output.join().unwrap()).replace('\r', "");
    assert!(text.contains("TERM=[screen]"), "{text}");
}

#[test]
fn terminal_is_controlling_merged_resizable_and_reaped() {
    let _terminal_lock = terminal_lock();
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
fn terminal_identity_and_environment_survive_fork_and_exec() {
    const LIFECYCLE: &str = r#"
t0=0; t1=0; t2=0
[ -t 0 ] && t0=1
[ -t 1 ] && t1=1
[ -t 2 ] && t2=1
printf 'PARENT TERM=%s TTY=%s%s%s\n' "$TERM" "$t0" "$t1" "$t2"
printf 'FD0='; readlink /proc/self/fd/0
printf 'FD1='; readlink /proc/self/fd/1
printf 'FD2='; readlink /proc/self/fd/2
sh -c '
    t=0
    [ -t 0 ] && t=1
    printf "CHILD TERM=%s TTY=%s\n" "$TERM" "$t"
    printf "CHILD_FD0="; readlink /proc/self/fd/0
'
exec sh -c '
    exec 3<>/dev/tty
    t=0; r=0
    [ -t 0 ] && t=1
    [ -t 3 ] && r=1
    printf "EXEC TERM=%s TTY=%s REOPEN=%s\n" "$TERM" "$t" "$r"
'
"#;
    let _terminal_lock = terminal_lock();
    let mut child = Engine::new()
        .command(Guest::Aarch64, "/bin/sh")
        .config(Config::new().root(rootfs()).env("TERM", "screen"))
        .args(["-c", LIFECYCLE])
        .terminal(Size::new(31, 97).unwrap())
        .spawn()
        .unwrap();
    let terminal = child.take_terminal().unwrap();
    let mut reader = terminal.try_clone().unwrap();
    let output = std::thread::spawn(move || {
        let mut output = Vec::new();
        reader.read_to_end(&mut output).unwrap();
        output
    });
    assert_eq!(child.wait().unwrap(), Exit::Code(0));
    let text = String::from_utf8_lossy(&output.join().unwrap()).replace('\r', "");
    assert!(text.contains("PARENT TERM=screen TTY=111"), "{text}");
    assert!(
        text.contains("FD0=/dev/pts/0\nFD1=/dev/pts/0\nFD2=/dev/pts/0"),
        "{text}"
    );
    assert!(
        text.contains("CHILD TERM=screen TTY=1\nCHILD_FD0=/dev/pts/0"),
        "{text}"
    );
    assert!(text.contains("EXEC TERM=screen TTY=1 REOPEN=1"), "{text}");
    drop(terminal);
}

#[test]
fn terminal_size_rejects_empty_dimensions() {
    assert!(Size::new(0, 80).is_err());
    assert!(Size::new(24, 0).is_err());
}
