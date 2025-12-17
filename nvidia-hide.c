#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int file_exists(const char *p) {
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int build_path(char *out, size_t out_sz, const char *dir, const char *leaf) {
    if (!out || out_sz == 0) return -1;
    if (!dir || !*dir) return -1;
    int n = snprintf(out, out_sz, "%s/%s", dir, leaf);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int dirname_of_argv0(char *out, size_t out_sz, const char *argv0) {
    if (!out || out_sz == 0) return -1;
    out[0] = 0;
    if (!argv0 || !*argv0) return -1;

    char tmp[PATH_MAX];
    if (argv0[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", argv0);
    } else {
        // If argv0 is relative, try resolve via /proc/self/exe
        ssize_t n = readlink("/proc/self/exe", tmp, sizeof(tmp)-1);
        if (n < 0) return -1;
        tmp[n] = 0;
    }
    char *slash = strrchr(tmp, '/');
    if (!slash) return -1;
    *slash = 0;
    snprintf(out, out_sz, "%s", tmp);
    return 0;
}

static const char *default_so_name(void) {
    return "libnvidia-hide.so";
}

static int resolve_so_path(char *out, size_t out_sz, const char *argv0) {
    // 1) env override
    const char *env = getenv("LIBNVIDIAHIDE_SO");
    if (env && *env && file_exists(env)) {
        snprintf(out, out_sz, "%s", env);
        return 0;
    }

    // 2) next to this binary (common for local installs)
    char d[PATH_MAX], p[PATH_MAX];
    if (dirname_of_argv0(d, sizeof(d), argv0) == 0) {
        if (build_path(p, sizeof(p), d, default_so_name()) == 0 && file_exists(p)) {
            snprintf(out, out_sz, "%s", p);
            return 0;
        }
        // also try ../lib
        if (build_path(p, sizeof(p), d, "../lib") == 0) {
            char p2[PATH_MAX];
            if (build_path(p2, sizeof(p2), p, default_so_name()) == 0 && file_exists(p2)) {
                snprintf(out, out_sz, "%s", p2);
                return 0;
            }
        }
    }

    // 3) typical system paths
    const char *cands[] = {
        "/usr/lib/libnvidia-hide.so",
        "/usr/local/lib/libnvidia-hide.so",
        "/lib/libnvidia-hide.so",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        if (file_exists(cands[i])) {
            snprintf(out, out_sz, "%s", cands[i]);
            return 0;
        }
    }

    return -1;
}

static void usage(FILE *f) {
    fprintf(f,
        "Usage:\n"
        "  nvidia-hide run -- <command> [args...]\n"
        "  nvidia-hide run <command> [args...]\n"
        "\n"
        "Environment:\n"
        "  LIBNVIDIAHIDE_SO=/path/to/libnvidia-hide.so\n"
        "  LIBNVIDIAHIDE_ALLOWLIST=pat1:pat2:...   (optional; evaluated inside the .so)\n"
        "  LIBNVIDIAHIDE_DENYLIST=pat1:pat2:...    (optional; evaluated inside the .so)\n"
        "\n"
        "Config files (optional; evaluated inside the .so):\n"
        "  $XDG_CONFIG_HOME/nvidia-hide/allowlist (or ~/.config/nvidia-hide/allowlist)\n"
        "  $XDG_CONFIG_HOME/nvidia-hide/denylist  (or ~/.config/nvidia-hide/denylist)\n"
        "\n"
        "Notes:\n"
        "  - This launcher sets LD_PRELOAD only for the launched process (native apps).\n"
        "  - Flatpak/Snap sandboxing typically blocks LD_PRELOAD; this tool does not handle sandboxed apps.\n"
    );
}

static int set_preload(const char *so_path) {
    const char *prev = getenv("LD_PRELOAD");
    if (!prev || !*prev) {
        return setenv("LD_PRELOAD", so_path, 1);
    }
    // Avoid duplicating
    if (strstr(prev, so_path)) return 0;

    // glibc reliably supports space-separated entries.
    size_t need = strlen(prev) + 1 + strlen(so_path) + 1;
    char *buf = (char*)malloc(need);
    if (!buf) return -1;
    snprintf(buf, need, "%s %s", prev, so_path);
    int rc = setenv("LD_PRELOAD", buf, 1);
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0) {
        usage(stdout);
        return 0;
    }

    if (strcmp(sub, "run") != 0) {
        fprintf(stderr, "nvidia-hide: unknown subcommand '%s'\n\n", sub);
        usage(stderr);
        return 2;
    }

    int cmd_i = 2;
    if (cmd_i < argc && strcmp(argv[cmd_i], "--") == 0) cmd_i++;

    if (cmd_i >= argc) {
        fprintf(stderr, "nvidia-hide: missing command\n\n");
        usage(stderr);
        return 2;
    }

    // Set per-process LD_PRELOAD and exec
    char so_path[PATH_MAX];
    if (resolve_so_path(so_path, sizeof(so_path), argv[0]) != 0) {
        fprintf(stderr, "nvidia-hide: could not find libnvidia-hide.so.\n");
        fprintf(stderr, "  Set LIBNVIDIAHIDE_SO=/full/path/to/libnvidia-hide.so\n");
        return 1;
    }
    if (set_preload(so_path) != 0) {
        fprintf(stderr, "nvidia-hide: failed to set LD_PRELOAD: %s\n", strerror(errno));
        return 1;
    }

    execvp(argv[cmd_i], &argv[cmd_i]);
    fprintf(stderr, "nvidia-hide: execvp(%s) failed: %s\n", argv[cmd_i], strerror(errno));
    return 127;
}
