use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::process;

fn file_exists(p: &Path) -> bool {
    fs::metadata(p).map(|m| m.is_file()).unwrap_or(false)
}

fn resolve_so_path() -> Option<PathBuf> {
    // 1) explicit env
    if let Ok(p) = env::var("LIBNVIDIAHIDE_SO") {
        let pb = PathBuf::from(p);
        if file_exists(&pb) { return Some(pb); }
    }

    // 2) alongside this binary (or ../lib)
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            let cand = dir.join("libnvidia_hide.so");
            if file_exists(&cand) { return Some(cand); }
            let cand = dir.join("libnvidia-hide.so");
            if file_exists(&cand) { return Some(cand); }
            let cand = dir.join("../lib").join("libnvidia_hide.so");
            if file_exists(&cand) { return Some(cand); }
            let cand = dir.join("../lib").join("libnvidia-hide.so");
            if file_exists(&cand) { return Some(cand); }
        }
    }

    // 3) common system paths
    for s in [
        "/usr/lib/libnvidia_hide.so",
        "/usr/local/lib/libnvidia_hide.so",
        "/lib/libnvidia_hide.so",
        "/usr/lib/libnvidia-hide.so",
        "/usr/local/lib/libnvidia-hide.so",
        "/lib/libnvidia-hide.so",
    ] {
        let pb = PathBuf::from(s);
        if file_exists(&pb) { return Some(pb); }
    }

    None
}

fn set_ld_preload(so: &Path) {
    let so_s: OsString = so.as_os_str().to_owned();
    let prev = env::var_os("LD_PRELOAD");
    match prev {
        None => env::set_var("LD_PRELOAD", &so_s),
        Some(p) => {
            let mut newv = p;
            // glibc reliably accepts space-separated list
            newv.push(" ");
            newv.push(so_s);
            env::set_var("LD_PRELOAD", newv);
        }
    }
}

fn usage() -> ! {
    eprintln!(
r#"Usage:
  nvidia-hide run -- <command> [args...]
  nvidia-hide run <command> [args...]

Notes:
  - For native apps, this sets LD_PRELOAD only for the launched process tree.
  - Flatpak/Snap are not supported (LD_PRELOAD is blocked by design).

Environment:
  LIBNVIDIAHIDE_SO=/path/to/libnvidia_hide.so     (override library path)
  LIBNVIDIAHIDE_DEBUG=1                          (enable library logs)
  LIBNVIDIAHIDE_ALLOWLIST=pat1:pat2:...          (optional)
  LIBNVIDIAHIDE_DENYLIST=pat1:pat2:...           (optional)

Config files (optional):
  $XDG_CONFIG_HOME/nvidia-hide/allowlist  (or ~/.config/nvidia-hide/allowlist)
  $XDG_CONFIG_HOME/nvidia-hide/denylist   (or ~/.config/nvidia-hide/denylist)
"#);
    process::exit(2);
}

fn main() {
    let mut args: Vec<OsString> = env::args_os().collect();
    if args.len() < 2 { usage(); }

    let sub = args[1].clone();
    if sub == "-h" || sub == "--help" { usage(); }

    if sub != "run" {
        eprintln!("nvidia-hide: unknown subcommand");
        usage();
    }

    let mut cmd_i = 2;
    if cmd_i < args.len() && args[cmd_i] == "--" { cmd_i += 1; }
    if cmd_i >= args.len() { usage(); }

    let so = match resolve_so_path() {
        Some(p) => p,
        None => {
            eprintln!("nvidia-hide: could not find libnvidia_hide.so");
            eprintln!("  Set LIBNVIDIAHIDE_SO=/full/path/to/libnvidia_hide.so");
            process::exit(1);
        }
    };
    set_ld_preload(&so);

    // exec the command
    let cmd = args[cmd_i].clone();
    let cmd_args = args[cmd_i..].to_vec();

    // Use execvp via Command + exec (Unix only)
    #[cfg(target_family="unix")]
    {
        use std::os::unix::process::CommandExt;
        let mut c = process::Command::new(&cmd);
        c.args(&cmd_args[1..]);
        let err = c.exec();
        eprintln!("nvidia-hide: exec failed: {err}");
        process::exit(127);
    }
    #[cfg(not(target_family="unix"))]
    {
        eprintln!("nvidia-hide: unsupported platform");
        process::exit(1);
    }
}
