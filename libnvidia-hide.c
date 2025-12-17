#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <linux/limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void dbg(const char *fmt, ...);
static void trim(char *s);


static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s+i, strlen(s+i)+1);
}


#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how { uint64_t flags, mode, resolve; };
#endif

// --------- config ---------
// LIBNVIDIAHIDE_DEBUG=1 prints one-time init info

static int g_debug = 0;

// --------- init guards ---------
static volatile int g_inited = 0;
static volatile int g_initializing = 0;

// --------- detected NVIDIA DRM nodes ---------
// store basenames like "card1", "renderD129"
// --------- policy (allow/deny) ----------
// If allowlist is non-empty, the library is active only when /proc/self/exe matches.
// If denylist matches, the library is disabled for that process.
static int g_active = 1;


static const char *base_name(const char *p) {
    if (!p) return p;
    const char *s = strrchr(p, '/');
    return s ? s+1 : p;
}

static int read_self_exe(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return 0;
}

// Match a single pattern against either full exe path (if pattern has '/')
// or basename (if pattern has no '/').
static int match_pat(const char *pat, const char *exe_full, const char *exe_base) {
    if (!pat || !*pat) return 0;
    const char *target = (strchr(pat, '/') != NULL) ? exe_full : exe_base;
    if (!target) return 0;
    // FNM_PATHNAME would make '*' not cross '/', but we want typical shell-glob semantics.
    return fnmatch(pat, target, 0) == 0;
}

// Env list is colon-separated patterns.
static int env_list_has_match(const char *envval, const char *exe_full, const char *exe_base) {
    if (!envval || !*envval) return 0;
    const char *p = envval;
    while (*p) {
        const char *q = strchr(p, ':');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len) {
            char buf[PATH_MAX];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            trim(buf);
            if (match_pat(buf, exe_full, exe_base)) return 1;
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static int file_list_has_match(const char *path, const char *exe_full, const char *exe_base, int *out_had_entries) {
    if (out_had_entries) *out_had_entries = 0;
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (out_had_entries) *out_had_entries = 1;
        if (match_pat(line, exe_full, exe_base)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static void build_xdg_path(char *out, size_t out_sz, const char *leaf) {
    if (!out || out_sz == 0) return;
    out[0] = 0;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(out, out_sz, "%s/nvidia-hide/%s", xdg, leaf);
    } else if (home && *home) {
        snprintf(out, out_sz, "%s/.config/nvidia-hide/%s", home, leaf);
    } else {
        snprintf(out, out_sz, "/nonexistent/%s", leaf);
    }
}

static void apply_policy_from_exe(void) {
    char exe_full[PATH_MAX];
    if (read_self_exe(exe_full, sizeof(exe_full)) < 0) {
        // If we cannot read /proc/self/exe, keep active (fail open).
        return;
    }
    const char *exe_base = base_name(exe_full);

    const char *env_allow = getenv("LIBNVIDIAHIDE_ALLOWLIST");
    const char *env_deny  = getenv("LIBNVIDIAHIDE_DENYLIST");

    char allow_path[PATH_MAX], deny_path[PATH_MAX];
    build_xdg_path(allow_path, sizeof(allow_path), "allowlist");
    build_xdg_path(deny_path, sizeof(deny_path), "denylist");

    int file_allow_had = 0;
    int file_deny_had  = 0;

    int allow_match_env = env_list_has_match(env_allow, exe_full, exe_base);
    int deny_match_env  = env_list_has_match(env_deny,  exe_full, exe_base);

    int allow_match_file = file_list_has_match(allow_path, exe_full, exe_base, &file_allow_had);
    int deny_match_file  = file_list_has_match(deny_path,  exe_full, exe_base, &file_deny_had);

    int has_allow = (env_allow && *env_allow) || file_allow_had;
    int allow_match = allow_match_env || allow_match_file;

    // If allowlist exists and we don't match it => disable.
    if (has_allow && !allow_match) g_active = 0;

    // Denylist always wins if matched.
    if (deny_match_env || deny_match_file) g_active = 0;

    if (g_debug) {
        dbg("policy: exe=%s", exe_full);
        dbg("policy: active=%d (has_allow=%d allow_match=%d deny_match=%d)",
            g_active, has_allow, allow_match, (deny_match_env || deny_match_file));
    }
}

#define MAX_NODES 64
static char g_nodes[MAX_NODES][NAME_MAX];
static int  g_nodes_n = 0;

// --------- detected NVIDIA PCI BDFs ---------
// store "0000:01:00.0" etc (used only to hide by-path entries and optionally sysfs config)
#define MAX_BDFS 8
static char g_bdfs[MAX_BDFS][32];
static int  g_bdfs_n = 0;

// linux_dirent64 for getdents64
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char d_name[];
};

static void dbg(const char *fmt, ...) {
    if (!g_debug) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[libnvidia-hide] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}


static int read_file_raw(const char *path, char *buf, size_t bufsz) {
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    trim(buf);
    return 0;
}

static int parse_hex(const char *s, unsigned *out) {
    unsigned v = 0;
    if (sscanf(s, "0x%x", &v) == 1 || sscanf(s, "%x", &v) == 1) { *out = v; return 0; }
    return -1;
}

static void add_node(const char *name) {
    if (!name || !*name) return;
    for (int i=0;i<g_nodes_n;i++) if (!strcmp(g_nodes[i], name)) return;
    if (g_nodes_n >= MAX_NODES) return;
    snprintf(g_nodes[g_nodes_n++], NAME_MAX, "%s", name);
}

static int is_node(const char *name) {
    if (!name) return 0;
    for (int i=0;i<g_nodes_n;i++) if (!strcmp(g_nodes[i], name)) return 1;
    return 0;
}

static void add_bdf(const char *bdf) {
    if (!bdf || !*bdf) return;
    for (int i=0;i<g_bdfs_n;i++) if (!strcmp(g_bdfs[i], bdf)) return;
    if (g_bdfs_n >= MAX_BDFS) return;
    snprintf(g_bdfs[g_bdfs_n++], sizeof(g_bdfs[g_bdfs_n-1]), "%s", bdf);
}

static int drm_entry_is_nvidia(const char *entry) {
    char vendor_path[PATH_MAX];
    snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/%s/device/vendor", entry);
    char buf[64];
    if (read_file_raw(vendor_path, buf, sizeof(buf)) != 0) return 0;
    unsigned v=0;
    if (parse_hex(buf, &v) != 0) return 0;
    return (v == 0x10de);
}

// scan /sys/class/drm via raw getdents64 (so we do NOT depend on libc readdir while initializing)
static void scan_nodes_raw(void) {
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/sys/class/drm", O_RDONLY|O_DIRECTORY|O_CLOEXEC, 0);
    if (fd < 0) return;

    char buf[8192];
    for (;;) {
        int nread = (int)syscall(SYS_getdents64, fd, buf, (int)sizeof(buf));
        if (nread <= 0) break;

        int bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64 *d = (struct linux_dirent64*)(buf + bpos);
            const char *n = d->d_name;
            if (n[0] != '.') {
                if (!strncmp(n, "card", 4) || !strncmp(n, "renderD", 7)) {
                    if (drm_entry_is_nvidia(n)) add_node(n);
                }
            }
            bpos += d->d_reclen;
        }
    }
    close(fd);
}

static void discover_bdfs_from_nodes(void) {
    // resolve /sys/class/drm/<node>/device -> .../<BDF>
    for (int i=0;i<g_nodes_n;i++) {
        char linkpath[PATH_MAX];
        snprintf(linkpath, sizeof(linkpath), "/sys/class/drm/%s/device", g_nodes[i]);

        char target[PATH_MAX];
        ssize_t n = readlink(linkpath, target, sizeof(target)-1);
        if (n <= 0) continue;
        target[n] = 0;

        const char *base = strrchr(target, '/');
        base = base ? base+1 : target;

        if (strchr(base, ':') && strchr(base, '.')) add_bdf(base);
    }
}

static void nh_init(void) {
    if (__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) return;

    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_initializing, &expected, 1, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        while (!__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) sched_yield();
        return;
        }

        const char *dbg_env = getenv("LIBNVIDIAHIDE_DEBUG");
    if (dbg_env && strcmp(dbg_env, "0") != 0) g_debug = 1;

    g_active = 1;
    apply_policy_from_exe();


    if (!g_active) {
        if (g_debug) dbg("init: inactive for this process; skipping discovery/hooks");
        return;
    }
    if (!g_active) {
        if (g_debug) dbg("init: inactive for this process; skipping discovery");
        return;
    }


    scan_nodes_raw();
    discover_bdfs_from_nodes();

    dbg("init: nvidia_nodes=%d nvidia_bdfs=%d", g_nodes_n, g_bdfs_n);
    for (int i=0;i<g_nodes_n;i++) dbg("  node: %s", g_nodes[i]);
    for (int i=0;i<g_bdfs_n;i++) dbg("  bdf:  %s", g_bdfs[i]);

    __atomic_store_n(&g_inited, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_initializing, 0, __ATOMIC_RELEASE);
}

static inline void ensure_init(void) { if (!__atomic_load_n(&g_inited, __ATOMIC_ACQUIRE)) nh_init(); }

// ---------- deny logic ----------

static int is_nvidia_path(const char *p) {
    if (!g_active) return 0;
    if (!p) return 0;
    ensure_init();

    // Device nodes
    if (!strncmp(p, "/dev/nvidia", 10)) return 1;

    if (!strncmp(p, "/dev/dri/", 9)) {
        const char *base = p + 9;
        if (is_node(base)) return 1;
    }

    // NVIDIA GBM/GL/Vulkan assets
    if (strstr(p, "nvidia-drm_gbm.so")) return 1;
    if (strstr(p, "libGLX_nvidia.so")) return 1;
    if (strstr(p, "/usr/share/vulkan/implicit_layer.d/nvidia")) return 1;
    if (strstr(p, "/usr/share/vulkan/icd.d/nvidia")) return 1;

    // Extra: block libnvidia-* opens (still only via open/openat, no dlopen dependency)
    if (strstr(p, "/usr/lib/libnvidia-")) return 1;

    // Block PCI config reads through ANY sysfs path (bus or devices)
    // matches ".../<BDF>/config" anywhere under /sys/
    if (strstr(p, "/sys/") && strstr(p, "/config")) {
        for (int i=0;i<g_bdfs_n;i++) {
            char needle[64];
            snprintf(needle, sizeof(needle), "/%s/config", g_bdfs[i]);
            if (strstr(p, needle)) return 1;
        }
    }

    return 0;
}

static int is_nvidia_dirent(DIR *dirp, const char *name) {
    if (!g_active) return 0;
    if (!name) return 0;
    ensure_init();

    // If it scans /dev, hide /dev/nvidia* names
    if (!strncmp(name, "nvidia", 5)) return 1;

    // Hide discovered DRM nodes (cardX/renderD*)
    if (is_node(name)) return 1;

    // If scanning /dev/dri/by-path, often includes BDF in symlink name
    // hide if matches
    for (int i=0;i<g_bdfs_n;i++) {
        if (strstr(name, g_bdfs[i])) return 1;
        // also hide without domain "01:00.0" style
        const char *colon = strchr(g_bdfs[i], ':');
        if (colon && strstr(name, colon+1)) return 1;
    }

    return 0;
}

static int deny_ret(void) { errno = ENOENT; return -1; }

// ---------- hooks ----------

typedef int (*openat_f)(int, const char*, int, ...);
typedef int (*open_f)(const char*, int, ...);

int openat(int dirfd, const char *pathname, int flags, ...) {
    static openat_f real_openat = NULL;
    if (!real_openat) real_openat = (openat_f)dlsym(RTLD_NEXT, "openat");

    if (is_nvidia_path(pathname)) return deny_ret();

    va_list ap;
    va_start(ap, flags);
    int fd;
    if (flags & O_CREAT) {
        mode_t mode = va_arg(ap, mode_t);
        fd = real_openat(dirfd, pathname, flags, mode);
    } else {
        fd = real_openat(dirfd, pathname, flags);
    }
    va_end(ap);
    return fd;
}

// Also hook open/open64 for completeness (some paths use these)
int open(const char *pathname, int flags, ...) {
    static open_f real_open = NULL;
    if (!real_open) real_open = (open_f)dlsym(RTLD_NEXT, "open");

    if (is_nvidia_path(pathname)) return deny_ret();

    va_list ap;
    va_start(ap, flags);
    int fd;
    if (flags & O_CREAT) {
        mode_t mode = va_arg(ap, mode_t);
        fd = real_open(pathname, flags, mode);
    } else {
        fd = real_open(pathname, flags);
    }
    va_end(ap);
    return fd;
}

int open64(const char *pathname, int flags, ...) {
    static open_f real_open64 = NULL;
    if (!real_open64) real_open64 = (open_f)dlsym(RTLD_NEXT, "open64");

    if (is_nvidia_path(pathname)) return deny_ret();

    va_list ap;
    va_start(ap, flags);
    int fd;
    if (flags & O_CREAT) {
        mode_t mode = va_arg(ap, mode_t);
        fd = real_open64(pathname, flags, mode);
    } else {
        fd = real_open64(pathname, flags);
    }
    va_end(ap);
    return fd;
}

// Hook openat2 if present
typedef int (*openat2_f)(int, const char*, const struct open_how*, size_t);
int openat2(int dirfd, const char *pathname, const struct open_how *how, size_t size) {
    static openat2_f real_openat2 = NULL;
    if (!real_openat2) real_openat2 = (openat2_f)dlsym(RTLD_NEXT, "openat2");

    if (is_nvidia_path(pathname)) return deny_ret();

    if (real_openat2) return real_openat2(dirfd, pathname, how, size);
    #ifdef SYS_openat2
    return (int)syscall(SYS_openat2, dirfd, pathname, how, size);
    #else
    errno = ENOSYS;
    return -1;
    #endif
}

/* ---- Block dlopen of NVIDIA libs ---- */
typedef void* (*dlopen_f)(const char*, int);

void *dlopen(const char *filename, int flags) {
    static dlopen_f real_dlopen = NULL;
    static __thread int in_hook = 0;

    if (!real_dlopen) {
        if (in_hook) { errno = ENOENT; return NULL; }
        in_hook = 1;
        real_dlopen = (dlopen_f)dlsym(RTLD_NEXT, "dlopen");
        in_hook = 0;
    }

    if (filename && (
        strstr(filename, "nvidia") ||
        strstr(filename, "libGLX_nvidia") ||
        strstr(filename, "nvidia-drm_gbm.so") ||
        strstr(filename, "libnvidia-")
    )) {
        errno = ENOENT;
        return NULL;
    }

    return real_dlopen ? real_dlopen(filename, flags) : NULL;
}

/* ---- Hide NVIDIA entries from directory enumeration ---- */
typedef struct dirent *(*readdir_f)(DIR*);
typedef struct dirent64 *(*readdir64_f)(DIR*);

struct dirent *readdir(DIR *dirp) {
    static readdir_f real_readdir = NULL;
    if (!real_readdir) real_readdir = (readdir_f)dlsym(RTLD_NEXT, "readdir");

    struct dirent *ent;
    while ((ent = real_readdir(dirp)) != NULL) {
        if (!is_nvidia_dirent(dirp, ent->d_name)) return ent;
    }
    return NULL;
}

struct dirent64 *readdir64(DIR *dirp) {
    static readdir64_f real_readdir64 = NULL;
    if (!real_readdir64) real_readdir64 = (readdir64_f)dlsym(RTLD_NEXT, "readdir64");

    struct dirent64 *ent;
    while ((ent = real_readdir64(dirp)) != NULL) {
        if (!is_nvidia_dirent(dirp, ent->d_name)) return ent;
    }
    return NULL;
}
