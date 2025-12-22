#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]
#![cfg(target_os = "linux")]
#![feature(c_variadic)]

// A Rust port of libnvidia-hide.c: LD_PRELOAD library that hides NVIDIA DRM + /dev/nvidia*
// from process probing, to prevent needless dGPU runtime resume on hybrid laptops.

use libc::{
    c_char, c_int, c_long, c_void, dirent, dirent64, mode_t, size_t, DIR, ENOENT, O_CREAT,
    O_TMPFILE, AT_FDCWD,
};
use std::ffi::{CStr, CString};
use std::mem;
use std::ptr;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

static DEBUG: AtomicBool = AtomicBool::new(false);
static ACTIVE: AtomicBool = AtomicBool::new(true);

static NVIDIA_NODES: OnceLock<Vec<String>> = OnceLock::new();
static NVIDIA_BDFS: OnceLock<Vec<String>> = OnceLock::new();

fn dbg(msg: &str) {
    if DEBUG.load(Ordering::Relaxed) {
        eprintln!("[libnvidia-hide] {msg}");
    }
}

fn env_is_truthy(name: &str) -> bool {
    std::env::var(name).map(|v| v != "0" && !v.is_empty()).unwrap_or(false)
}

fn read_self_exe() -> Option<String> {
    std::fs::read_link("/proc/self/exe").ok().and_then(|p| p.to_str().map(|s| s.to_string()))
}

fn basename(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

fn trim(s: &str) -> &str {
    s.trim_matches(|c: char| c == ' ' || c == '\t' || c == '\n' || c == '\r')
}

fn xdg_path(leaf: &str) -> String {
    if let Ok(xdg) = std::env::var("XDG_CONFIG_HOME") {
        format!("{xdg}/nvidia-hide/{leaf}")
    } else if let Ok(home) = std::env::var("HOME") {
        format!("{home}/.config/nvidia-hide/{leaf}")
    } else {
        format!("/nonexistent/{leaf}")
    }
}

// shell-glob matching using libc fnmatch
fn glob_match(pat: &str, exe_full: &str, exe_base: &str) -> bool {
    let target = if pat.contains('/') { exe_full } else { exe_base };
    let cpat = CString::new(pat).ok();
    let ctar = CString::new(target).ok();
    if cpat.is_none() || ctar.is_none() { return false; }
    unsafe { libc::fnmatch(cpat.unwrap().as_ptr(), ctar.unwrap().as_ptr(), 0) == 0 }
}

fn env_list_has_match(envval: Option<String>, exe_full: &str, exe_base: &str) -> bool {
    let Some(v) = envval else { return false; };
    if v.is_empty() { return false; }
    for tok in v.split(':') {
        let tok = trim(tok);
        if tok.is_empty() { continue; }
        if glob_match(tok, exe_full, exe_base) { return true; }
    }
    false
}

fn file_list_has_match(path: &str, exe_full: &str, exe_base: &str) -> (bool, bool) {
    // returns (matched, had_entries)
    let data = std::fs::read_to_string(path).ok();
    let Some(data) = data else { return (false, false); };
    let mut had = false;
    for line in data.lines() {
        let line = trim(line);
        if line.is_empty() || line.starts_with('#') { continue; }
        had = true;
        if glob_match(line, exe_full, exe_base) { return (true, had); }
    }
    (false, had)
}

fn apply_policy_from_exe() {
    let exe_full = match read_self_exe() {
        Some(p) => p,
        None => return, // fail-open
    };
    let exe_base = basename(&exe_full).to_string();

    let env_allow = std::env::var("LIBNVIDIAHIDE_ALLOWLIST").ok();
    let env_deny  = std::env::var("LIBNVIDIAHIDE_DENYLIST").ok();

    let allow_path = xdg_path("allowlist");
    let deny_path  = xdg_path("denylist");

    let allow_match_env = env_list_has_match(env_allow.clone(), &exe_full, &exe_base);
    let deny_match_env  = env_list_has_match(env_deny.clone(),  &exe_full, &exe_base);

    let (allow_match_file, allow_had_entries) = file_list_has_match(&allow_path, &exe_full, &exe_base);
    let (deny_match_file,  _deny_had_entries)  = file_list_has_match(&deny_path,  &exe_full, &exe_base);

    let has_allow = env_allow.as_deref().map(|s| !s.is_empty()).unwrap_or(false) || allow_had_entries;
    let allow_match = allow_match_env || allow_match_file;
    let deny_match = deny_match_env || deny_match_file;

    if has_allow && !allow_match {
        ACTIVE.store(false, Ordering::Relaxed);
    }
    if deny_match {
        ACTIVE.store(false, Ordering::Relaxed);
    }

    if DEBUG.load(Ordering::Relaxed) {
        dbg(&format!("policy: exe={exe_full}"));
        dbg(&format!(
            "policy: active={} (has_allow={} allow_match={} deny_match={})",
            if ACTIVE.load(Ordering::Relaxed) {1} else {0},
            if has_allow {1} else {0},
            if allow_match {1} else {0},
            if deny_match {1} else {0},
        ));
    }
}

fn read_u32_hex_file(path: &str) -> Option<u32> {
    let s = std::fs::read_to_string(path).ok()?;
    let s = trim(&s);
    let s = s.strip_prefix("0x").unwrap_or(s);
    u32::from_str_radix(s, 16).ok()
}

fn sysfs_drm_node_vendor_is_nvidia(node: &str) -> bool {
    let path = format!("/sys/class/drm/{node}/device/vendor");
    read_u32_hex_file(&path).map(|v| v == 0x10de).unwrap_or(false)
}

fn sysfs_drm_node_bdf(node: &str) -> Option<String> {
    // /sys/class/drm/<node>/device -> .../<BDF>
    let link = std::fs::read_link(format!("/sys/class/drm/{node}/device")).ok()?;
    let s = link.file_name()?.to_str()?.to_string();
    if s.len() >= 7 && s.contains(':') { Some(s) } else { None }
}

fn is_drm_devnode_name(name: &str) -> bool {
    if name.starts_with("renderD") { return true; }
    if name.starts_with("card") {
        return name[4..].chars().all(|c| c.is_ascii_digit());
    }
    false
}

fn discover_nvidia() {
    let mut nodes = Vec::new();
    let mut bdfs = Vec::new();

    if let Ok(rd) = std::fs::read_dir("/sys/class/drm") {
        for ent in rd.flatten() {
            let name = ent.file_name();
            let name = name.to_string_lossy().to_string();
            if !is_drm_devnode_name(&name) { continue; }
            if !sysfs_drm_node_vendor_is_nvidia(&name) { continue; }
            nodes.push(name.clone());
            if let Some(bdf) = sysfs_drm_node_bdf(&name) {
                if !bdfs.contains(&bdf) { bdfs.push(bdf); }
            }
        }
    }

    NVIDIA_NODES.set(nodes).ok();
    NVIDIA_BDFS.set(bdfs).ok();

    if DEBUG.load(Ordering::Relaxed) {
        let nodes = NVIDIA_NODES.get().map(|v| v.len()).unwrap_or(0);
        let bdfs = NVIDIA_BDFS.get().map(|v| v.len()).unwrap_or(0);
        dbg(&format!("init: nvidia_nodes={nodes} nvidia_bdfs={bdfs}"));
        if let Some(v) = NVIDIA_NODES.get() {
            for n in v { dbg(&format!("  node: {n}")); }
        }
        if let Some(v) = NVIDIA_BDFS.get() {
            for b in v { dbg(&format!("  bdf:  {b}")); }
        }
    }
}

fn nvidia_nodes() -> &'static [String] {
    NVIDIA_NODES.get().map(|v| v.as_slice()).unwrap_or(&[])
}
fn nvidia_bdfs() -> &'static [String] {
    NVIDIA_BDFS.get().map(|v| v.as_slice()).unwrap_or(&[])
}

fn starts_with_nvidia_dev(path: &str) -> bool {
    path.starts_with("/dev/nvidia")
}

fn is_blocked_pci_config(path: &str) -> bool {
    // Block reads of sysfs PCI config for NVIDIA BDFs via any sysfs path containing "/<bdf>/config"
    if !path.contains("/config") { return false; }
    for bdf in nvidia_bdfs() {
        let needle = format!("/{bdf}/config");
        if path.contains(&needle) { return true; }
    }
    false
}

fn is_nvidia_dri_path(path: &str) -> bool {
    // block direct NVIDIA nodes
    for n in nvidia_nodes() {
        if path == format!("/dev/dri/{n}") { return true; }
    }
    // hide by-path entries that mention NVIDIA bdf
    if path.starts_with("/dev/dri/by-path/") {
        for bdf in nvidia_bdfs() {
            if path.contains(bdf) { return true; }
        }
    }
    false
}

fn should_block_open(path: &str) -> bool {
    if !ACTIVE.load(Ordering::Relaxed) { return false; }
    if starts_with_nvidia_dev(path) { return true; }
    if is_nvidia_dri_path(path) { return true; }
    if path.starts_with("/usr/share/vulkan/icd.d/nvidia") { return true; }
    if path.starts_with("/usr/share/vulkan/implicit_layer.d/nvidia") { return true; }
    if path.contains("nvidia-drm_gbm.so") { return true; }
    if path.contains("libGLX_nvidia.so") { return true; }
    if path.starts_with("/usr/lib/libnvidia-") { return true; }
    if is_blocked_pci_config(path) { return true; }
    false
}

unsafe fn set_errno(e: c_int) {
    *libc::__errno_location() = e;
}

// --- constructor ---
extern "C" fn nh_init() {
    DEBUG.store(env_is_truthy("LIBNVIDIAHIDE_DEBUG"), Ordering::Relaxed);

    ACTIVE.store(true, Ordering::Relaxed);
    apply_policy_from_exe();

    if !ACTIVE.load(Ordering::Relaxed) {
        dbg("init: inactive for this process; skipping discovery/hooks");
        return;
    }
    discover_nvidia();
}

#[used]
#[cfg_attr(target_os = "linux", link_section = ".init_array")]
static INIT: extern "C" fn() = nh_init;

// --- dlopen hook (blocks nvidia userspace stack selection) ---
type dlopen_fn = unsafe extern "C" fn(*const c_char, c_int) -> *mut c_void;

unsafe fn real_dlopen() -> dlopen_fn {
    static REAL: OnceLock<dlopen_fn> = OnceLock::new();
    *REAL.get_or_init(|| {
        let sym = libc::dlsym(libc::RTLD_NEXT, b"dlopen\0".as_ptr() as *const c_char);
        mem::transmute::<*mut c_void, dlopen_fn>(sym)
    })
}

fn should_block_dlopen(filename: &str) -> bool {
    if !ACTIVE.load(Ordering::Relaxed) { return false; }
    // conservative substring blocks
    let f = filename;
    f.contains("libGLX_nvidia") ||
        f.contains("nvidia-drm_gbm.so") ||
        f.contains("libnvidia-") ||
        f.contains("/usr/lib/libnvidia-")
}

#[no_mangle]
pub unsafe extern "C" fn dlopen(filename: *const c_char, flags: c_int) -> *mut c_void {
    if filename.is_null() {
        return real_dlopen()(filename, flags);
    }
    let s = CStr::from_ptr(filename).to_string_lossy().to_string();
    if should_block_dlopen(&s) {
        dbg(&format!("dlopen: blocked: {s}"));
        set_errno(ENOENT);
        return ptr::null_mut();
    }
    real_dlopen()(filename, flags)
}

// --- readdir hooks (hide NVIDIA nodes from enumeration) ---
type readdir_fn = unsafe extern "C" fn(*mut DIR) -> *mut dirent;
type readdir64_fn = unsafe extern "C" fn(*mut DIR) -> *mut dirent64;

unsafe fn real_readdir() -> readdir_fn {
    static REAL: OnceLock<readdir_fn> = OnceLock::new();
    *REAL.get_or_init(|| {
        let sym = libc::dlsym(libc::RTLD_NEXT, b"readdir\0".as_ptr() as *const c_char);
        mem::transmute::<*mut c_void, readdir_fn>(sym)
    })
}
unsafe fn real_readdir64() -> readdir64_fn {
    static REAL: OnceLock<readdir64_fn> = OnceLock::new();
    *REAL.get_or_init(|| {
        let sym = libc::dlsym(libc::RTLD_NEXT, b"readdir64\0".as_ptr() as *const c_char);
        mem::transmute::<*mut c_void, readdir64_fn>(sym)
    })
}

fn dir_path(dirp: *mut DIR) -> Option<String> {
    if dirp.is_null() { return None; }
    unsafe {
        let fd = libc::dirfd(dirp);
        if fd < 0 { return None; }
        let link = format!("/proc/self/fd/{fd}");
        std::fs::read_link(link).ok().and_then(|p| p.to_str().map(|s| s.to_string()))
    }
}


fn is_hidden_entry(_dir: &str, name: &str) -> bool {
    if !ACTIVE.load(Ordering::Relaxed) { return false; }
    if name.starts_with("nvidia") { return true; }
    if nvidia_nodes().iter().any(|n| n == name) { return true; }

    // Hide entries containing NVIDIA BDFs (common in /dev/dri/by-path)
    for bdf in nvidia_bdfs() {
        if name.contains(bdf) { return true; }
        // also hide without domain, e.g. "01:00.0"
        if let Some(colon) = bdf.find(':') {
            let short = &bdf[colon+1..];
            if !short.is_empty() && name.contains(short) { return true; }
        }
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn readdir(dirp: *mut DIR) -> *mut dirent {
    let real = real_readdir();
    if !ACTIVE.load(Ordering::Relaxed) {
        return real(dirp);
    }
    let dir = dir_path(dirp).unwrap_or_default();
    loop {
        let ent = real(dirp);
        if ent.is_null() { return ent; }
        if dir.is_empty() { return ent; }
        let name = CStr::from_ptr((*ent).d_name.as_ptr()).to_string_lossy().to_string();
        if is_hidden_entry(&dir, &name) {
            continue;
        }
        return ent;
    }
}

#[no_mangle]
pub unsafe extern "C" fn readdir64(dirp: *mut DIR) -> *mut dirent64 {
    let real = real_readdir64();
    if !ACTIVE.load(Ordering::Relaxed) {
        return real(dirp);
    }
    let dir = dir_path(dirp).unwrap_or_default();
    loop {
        let ent = real(dirp);
        if ent.is_null() { return ent; }
        if dir.is_empty() { return ent; }
        let name = CStr::from_ptr((*ent).d_name.as_ptr()).to_string_lossy().to_string();
        if is_hidden_entry(&dir, &name) {
            continue;
        }
        return ent;
    }
}

// --- open/openat hooks ---
// Implemented via syscalls to avoid RTLD_NEXT recursion and to support varargs without calling a vararg fn pointer.

// Helper: read C string path safely
unsafe fn c_path(p: *const c_char) -> Option<String> {
    if p.is_null() { return None; }
    Some(CStr::from_ptr(p).to_string_lossy().to_string())
}

unsafe fn sys_openat(dirfd: c_int, pathname: *const c_char, flags: c_int, mode: mode_t) -> c_int {
    libc::syscall(libc::SYS_openat as c_long, dirfd as c_long, pathname as c_long, flags as c_long, mode as c_long) as c_int
}

unsafe fn sys_openat2(dirfd: c_int, pathname: *const c_char, how: *const c_void, size: usize) -> c_int {
    libc::syscall(libc::SYS_openat2 as c_long, dirfd as c_long, pathname as c_long, how as c_long, size as c_long) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn openat(dirfd: c_int, pathname: *const c_char, flags: c_int, mut args: ...) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("openat: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    let mut mode: mode_t = 0;
    if (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE {
        mode = args.arg::<mode_t>();
    }
    sys_openat(dirfd, pathname, flags, mode)
}

#[no_mangle]
pub unsafe extern "C" fn open(pathname: *const c_char, flags: c_int, mut args: ...) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("open: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    let mut mode: mode_t = 0;
    if (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE {
        mode = args.arg::<mode_t>();
    }
    sys_openat(AT_FDCWD, pathname, flags, mode)
}

#[no_mangle]
pub unsafe extern "C" fn open64(pathname: *const c_char, flags: c_int, mut args: ...) -> c_int {
    // open64 is the same as open for our purposes (64-bit off_t)
    open(pathname, flags, args)
}

#[no_mangle]
pub unsafe extern "C" fn __open_2(pathname: *const c_char, flags: c_int) -> c_int {
    // glibc fortify variant (no O_CREAT)
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("__open_2: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    sys_openat(AT_FDCWD, pathname, flags, 0)
}

#[no_mangle]
pub unsafe extern "C" fn __open64_2(pathname: *const c_char, flags: c_int) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("__open64_2: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    sys_openat(AT_FDCWD, pathname, flags, 0)
}

#[no_mangle]
pub unsafe extern "C" fn __openat_2(dirfd: c_int, pathname: *const c_char, flags: c_int) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("__openat_2: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    sys_openat(dirfd, pathname, flags, 0)
}

#[no_mangle]
pub unsafe extern "C" fn __openat64_2(dirfd: c_int, pathname: *const c_char, flags: c_int) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("__openat64_2: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    sys_openat(dirfd, pathname, flags, 0)
}

#[repr(C)]
pub struct open_how {
    pub flags: u64,
    pub mode: u64,
    pub resolve: u64,
}

#[no_mangle]
pub unsafe extern "C" fn openat2(dirfd: c_int, pathname: *const c_char, how: *const open_how, size: size_t) -> c_int {
    let path = c_path(pathname).unwrap_or_default();
    if should_block_open(&path) {
        dbg(&format!("openat2: blocked: {path}"));
        set_errno(ENOENT);
        return -1;
    }
    sys_openat2(dirfd, pathname, how as *const c_void, size as usize)
}
