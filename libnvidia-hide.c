#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

static int is_nvidia_path(const char *p) {
    if (!p) return 0;

    // Device nodes
    if (!strcmp(p, "/dev/dri/renderD129")) return 1;
    if (!strncmp(p, "/dev/nvidia", 10)) return 1;

    // NVIDIA GBM/GL/Vulkan assets
    if (strstr(p, "nvidia-drm_gbm.so")) return 1;
    if (strstr(p, "libGLX_nvidia.so")) return 1;
    if (strstr(p, "/usr/share/vulkan/implicit_layer.d/nvidia_layers.json")) return 1;
    if (strstr(p, "/usr/share/vulkan/icd.d/nvidia_icd.json")) return 1;

    return 0;
}

static int is_nvidia_dirent(const char *name) {
    if (!name) return 0;

    // Hide NVIDIA DRM nodes so Electron doesn't even attempt to open them
    if (!strcmp(name, "renderD129")) return 1;
    // Often NVIDIA card node is card1; harmless to hide card1 too.
    if (!strcmp(name, "card1")) return 1;

    // If Electron scans /dev/dri/by-path, hide dGPU symlinks
    if (strstr(name, "01:00.0")) return 1;

    // If it scans /dev, hide /dev/nvidia* names
    if (!strncmp(name, "nvidia", 5)) return 1;

    return 0;
}

static int deny_ret(void) { errno = ENOENT; return -1; }

typedef int (*openat_f)(int, const char*, int, ...);

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

/* ---- Block dlopen of NVIDIA libs (prevent loading NVIDIA GBM/GL stacks) ---- */
typedef void* (*dlopen_f)(const char*, int);

void *dlopen(const char *filename, int flags) {
    static dlopen_f real_dlopen = NULL;
    if (!real_dlopen) real_dlopen = (dlopen_f)dlsym(RTLD_NEXT, "dlopen");

    if (filename && (
        strstr(filename, "nvidia") ||               // broad, but effective
        strstr(filename, "libGLX_nvidia") ||
        strstr(filename, "nvidia-drm_gbm.so") ||
        strstr(filename, "libnvidia-")
    )) {
        errno = ENOENT;
        return NULL;
    }
    return real_dlopen(filename, flags);
}

/* ---- Hide NVIDIA entries from directory enumeration ---- */
typedef struct dirent *(*readdir_f)(DIR*);
typedef struct dirent64 *(*readdir64_f)(DIR*);

struct dirent *readdir(DIR *dirp) {
    static readdir_f real_readdir = NULL;
    if (!real_readdir) real_readdir = (readdir_f)dlsym(RTLD_NEXT, "readdir");

    struct dirent *ent;
    while ((ent = real_readdir(dirp)) != NULL) {
        if (!is_nvidia_dirent(ent->d_name)) return ent;
    }
    return NULL;
}

struct dirent64 *readdir64(DIR *dirp) {
    static readdir64_f real_readdir64 = NULL;
    if (!real_readdir64) real_readdir64 = (readdir64_f)dlsym(RTLD_NEXT, "readdir64");

    struct dirent64 *ent;
    while ((ent = real_readdir64(dirp)) != NULL) {
        if (!is_nvidia_dirent(ent->d_name)) return ent;
    }
    return NULL;
}
