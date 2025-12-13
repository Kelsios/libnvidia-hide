# libnvidia-hide (LD_PRELOAD workaround for Electron dGPU wakeups)

## What this is

This project provides a **small `LD_PRELOAD` shared library** that prevents Electron / Chromium-based applications (VS Code, Slack, Discord, etc.) from **waking up an NVIDIA dGPU** on hybrid graphics laptops.

### The problem

On hybrid GPU systems (Intel/AMD iGPU + NVIDIA dGPU), Electron apps often:

- enumerate **all DRM render nodes** (`/dev/dri/renderD*`)
- load **NVIDIA GBM / GLX / Vulkan libraries**
- probe **NVIDIA Vulkan ICDs and implicit layers**
- open `/dev/nvidia*` character devices

Even *probing* these resources is enough to **runtime-resume the dGPU**, causing:
- unnecessary power usage
- slow application startup
- possibly broken power management

Environment variables alone (`DRI_PRIME=0`, `__GLX_VENDOR_LIBRARY_NAME=mesa`, Vulkan overrides, etc.) are **not sufficient** in many cases.

### What this project does

This library **intercepts key libc functions at runtime** and:

1. **Hides NVIDIA devices from enumeration**
   - Filters out NVIDIA entries from `readdir()` / `readdir64()`
   - Electron never “sees” the NVIDIA render node

2. **Blocks opening NVIDIA device nodes**
   - `/dev/dri/renderD*` (NVIDIA)
   - `/dev/nvidia*`

3. **Blocks loading NVIDIA userspace libraries**
   - `libGLX_nvidia.so`
   - `nvidia-drm_gbm.so`
   - `libnvidia-*`

4. **Prevents Vulkan from discovering NVIDIA**
   - Blocks NVIDIA Vulkan ICDs and implicit layers

Crucially, this is done **without**:
- sandboxing
- permission/group changes
- udev rules
- disabling the NVIDIA driver
- breaking file/mic/camera access for applications that need it (such as MS Teams)

The application simply **falls back to the Intel iGPU**, and the dGPU stays suspended.

---

## How to compile

This repo already contains a pre-compiled library for you to use directly. If you want to use that, you may skip this step.

### Requirements

- `gcc`
- `glibc`
- `libdl` (part of glibc)

### Build steps

```bash
mkdir -p ~/.local/lib
gcc -shared -fPIC -O2 -ldl -o libnvidia-hide.so libnvidia-hide.c
```

This produces:

```
libnvidia-hide.so
```

You can place it anywhere, but `~/.local/lib/` is recommended.

---

## OPTIONAL: Customizing / identifying GPU device paths

Different systems may have different render node numbers.
Before modifying the code, **identify what your Electron app actually touches**.

### Step 1: Trace GPU-related syscalls

```bash
LD_PRELOAD=./libnvidia-hide.so \
strace -f -e openat,openat2,ioctl \
-o /tmp/electron.trace \
slack
```

(Replace `slack` with `discord`, `code`, etc., depending on your use case)

### Step 2: Inspect GPU-related accesses

```bash
grep -E "renderD[0-9]+|/dev/nvidia|nvidia-drm_gbm|libGLX_nvidia|nvidia_icd|nvidia_layers" \
/tmp/electron.trace | head -n 120
```

Typical output before fixing might include:

```
openat(..., "/dev/dri/renderD129", ...)
openat(..., "/dev/nvidia0", ...)
openat(..., "/usr/lib/gbm/nvidia-drm_gbm.so", ...)
openat(..., "/usr/share/vulkan/icd.d/nvidia_icd.json", ...)
```

### Step 3: Adjust the code if needed

In `libnvidia-hide.c`, look for:

```c
if (!strcmp(p, "/dev/dri/renderD129")) return 1;
```

If, for example, your NVIDIA render node is different (e.g. `renderD130`), update it accordingly.

---

## How to use it

### A) One-off usage (per app)

```bash
LD_PRELOAD=/full/path/to/libnvidia-hide.so discord
```

### B) Wrapper script (recommended)

```bash
mkdir -p ~/.local/bin

cat > ~/.local/bin/slack <<'EOF'
#!/usr/bin/env bash
export LD_PRELOAD="$HOME/.local/lib/libnvidia-hide.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec /usr/bin/slack "$@"
EOF

chmod +x ~/.local/bin/slack
```

### C) Desktop entry (GUI launchers)

```ini
Exec=env LD_PRELOAD=/full/path/to/libnvidia-hide.so slack %U
```

---

## Scope and limitations

-  **Tested and confirmed working with some popular Electron apps**
-  **Not yet tested with non-Electron applications** (though may work similarly)
-  May need adjustment if render node numbers or PCI layout differ

---

## Final notes

This is a **userspace-only workaround**:
- no kernel patches
- no driver removal
- no power-management hacks
  
You essentially force electron to behave like a good citizen.
