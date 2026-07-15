use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const CORE: &[&str] = &[
    "src/core/cli.c",
    "src/core/config.c",
    "src/core/engine.c",
    "src/core/fatal.c",
    "src/core/host_services.c",
    "src/core/launch.c",
    "src/core/log.c",
    "src/core/options.c",
];
const TRANSLATOR: &[&str] = &[
    "src/translator/arena.c",
    "src/translator/codegen.c",
    "src/translator/digest.c",
    "src/translator/identity.c",
    "src/translator/persist.c",
    "src/translator/reloc.c",
    "src/translator/window.c",
    "src/translator/guest/x86_64/decode.c",
    "src/translator/host/aarch64/codegen.c",
    "src/translator/host/x86_64/codegen.c",
    "src/translator/ir/interpreter.c",
    "src/translator/ir/ir.c",
];
const LINUX_ABI: &[&str] = &[
    "src/linux_abi/affinity.c",
    "src/linux_abi/container/vfs/gmap.c",
    "src/linux_abi/device.c",
    "src/linux_abi/fdcache.c",
    "src/linux_abi/epoll.c",
    "src/linux_abi/eventfd.c",
    "src/linux_abi/fork_wire.c",
    "src/linux_abi/inotify.c",
    "src/linux_abi/pipe.c",
    "src/linux_abi/placement.c",
    "src/linux_abi/errno.c",
    "src/linux_abi/limits.c",
    "src/linux_abi/linux_abi.c",
    "src/linux_abi/number.c",
    "src/linux_abi/open_plan.c",
    "src/linux_abi/parse.c",
    "src/linux_abi/readonly.c",
    "src/linux_abi/seccomp_vm.c",
    "src/linux_abi/stat.c",
    "src/linux_abi/watch.c",
    "src/linux_abi/xattr.c",
];
const COMMON_HOST: &[&str] = &[
    "src/host/child.c",
    "src/host/private.c",
    "src/host/range.c",
    "src/host/resolve.c",
    "src/host/sync.c",
    "src/host/clock.c",
    "src/host/file.c",
];
const LINUX_HOST: &[&str] = &[
    "src/host/linux/directory.c",
    "src/host/linux/host.c",
    "src/host/linux/process.c",
    "src/host/linux/range.c",
    "src/host/linux/system.c",
];
const MACOS_HOST: &[&str] = &[
    "src/host/macos/directory.c",
    "src/host/macos/host.c",
    "src/host/macos/process.c",
    "src/host/macos/range.c",
    "src/host/macos/system.c",
];

fn run(mut command: Command, purpose: &str) {
    command.stdout(Stdio::inherit()).stderr(Stdio::inherit());
    let status = command
        .status()
        .unwrap_or_else(|error| panic!("cannot {purpose}: {error}"));
    assert!(status.success(), "failed to {purpose}: {status}");
}

fn compile_archive(root: &Path, out: &Path, name: &str, sources: &[&str]) -> PathBuf {
    let mut build = cc::Build::new();
    build
        .cargo_metadata(false)
        .out_dir(out)
        .include(root.join("include"))
        .opt_level(2);
    for source in sources {
        build.file(root.join(source));
        println!("cargo:rerun-if-changed=native/{source}");
    }
    build.compile(name);
    out.join(format!("lib{name}.a"))
}

fn compile_object(
    root: &Path,
    out: &Path,
    target: &str,
    source: &str,
    object: &str,
    define: Option<&str>,
) -> PathBuf {
    let compiler = cc::Build::new().target(target).get_compiler();
    let output = out.join(object);
    let mut command = compiler.to_command();
    command
        .arg("-I")
        .arg(root.join("include"))
        .arg("-O2")
        .arg("-c")
        .arg(root.join(source))
        .arg("-o")
        .arg(&output);
    if target == "aarch64-unknown-linux-gnu" {
        command.arg("-D_GNU_SOURCE");
    }
    if let Some(value) = define {
        command.arg(value);
    }
    run(command, &format!("compile {source}"));
    println!("cargo:rerun-if-changed=native/{source}");
    output
}

fn link_engine(root: &Path, out: &Path, target: &str, isa: &str, archives: &[PathBuf]) {
    let target_object = compile_object(
        root,
        out,
        target,
        &format!("src/core/target/{isa}.c"),
        &format!("target-{isa}.o"),
        None,
    );
    let isa_define = if isa == "aarch64" {
        "-DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_AARCH64"
    } else {
        "-DHL_PRODUCTION_GUEST_ISA=HL_GUEST_ISA_X86_64"
    };
    let lifecycle = compile_object(
        root,
        out,
        target,
        "src/core/lifecycle.c",
        &format!("lifecycle-{isa}.o"),
        Some(isa_define),
    );
    let compiler = cc::Build::new().target(target).get_compiler();
    let executable = out.join(format!("hl-engine-linux-{isa}"));
    let mut command = compiler.to_command();
    command
        .arg("-o")
        .arg(&executable)
        .arg(target_object)
        .arg(lifecycle);
    for archive in archives {
        command.arg(archive);
    }
    command.arg("-pthread");
    if target == "aarch64-unknown-linux-gnu" {
        command.args(["-lm", "-ldl", "-latomic"]);
    }
    run(command, &format!("link Linux {isa} guest engine"));
    if target == "aarch64-apple-darwin" {
        let mut codesign = Command::new("codesign");
        codesign
            .args([
                OsStr::new("-s"),
                OsStr::new("-"),
                OsStr::new("--entitlements"),
            ])
            .arg(root.join("packaging/jit.entitlements"))
            .args([OsStr::new("-f")])
            .arg(&executable);
        run(codesign, &format!("codesign Linux {isa} guest engine"));
    }
}

fn main() {
    let target = env::var("TARGET").expect("Cargo TARGET is set");
    assert!(
        target == "aarch64-apple-darwin" || target == "aarch64-unknown-linux-gnu",
        "hl-engine supports only aarch64-apple-darwin and aarch64-unknown-linux-gnu; got {target}"
    );
    let root =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("Cargo manifest directory is set"))
            .join("native");
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR is set"));
    let mut host = COMMON_HOST.to_vec();
    host.extend_from_slice(if target == "aarch64-apple-darwin" {
        MACOS_HOST
    } else {
        LINUX_HOST
    });
    let archives = vec![
        compile_archive(&root, &out, "hl_engine", CORE),
        compile_archive(&root, &out, "hl_translator", TRANSLATOR),
        compile_archive(&root, &out, "hl_linux_abi", LINUX_ABI),
        compile_archive(&root, &out, "hl_host", &host),
    ];
    link_engine(&root, &out, &target, "aarch64", &archives);
    link_engine(&root, &out, &target, "x86_64", &archives);
    println!("cargo:rustc-env=HL_ENGINE_NATIVE_DIR={}", out.display());
    println!("cargo:rerun-if-changed=native/include");
    println!("cargo:rerun-if-changed=native/packaging/jit.entitlements");
    let _ = fs::metadata(root.join("include/hl/engine.h")).expect("vendored public engine headers");
}
