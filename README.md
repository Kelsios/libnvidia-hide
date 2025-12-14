# libnvidia-hide
**Prevent Electron apps from waking NVIDIA dGPU on hybrid laptops (LD_PRELOAD)**

---

## What this is

`libnvidia-hide` is a **small LD_PRELOAD shared library** that prevents Electron / Chromium-based applications (VS Code, Slack, Discord, etc.) from **waking up an NVIDIA dGPU** on hybrid graphics systems.

It does this entirely in **userspace**, without sandboxing, cgroups, udev rules, driver removal, or permission hacks.  
Applications keep full access to files, microphone, camera, portals, etc., while transparently falling back to the iGPU.

---

## The problem

On modern hybrid GPU laptops, Electron applications frequently:

- enumerate **all DRM devices** (`/dev/dri/card*`, `renderD*`)
- load **NVIDIA GBM / GLX / Vulkan libraries**
- parse **NVIDIA Vulkan ICDs and implicit layers**
- probe **PCI configuration space** for GPUs
- open `/dev/nvidia*` character devices

Any of the above is enough to **runtime-resume the NVIDIA GPU**, causing:

- unnecessary power usage
- slow application startup
- possibly broken power management

Environment variables such as:

- `DRI_PRIME=0`
- `__GLX_VENDOR_LIBRARY_NAME=mesa`
- Vulkan loader overrides

are **not sufficient** in many real-world cases.

---

## What this project does

`libnvidia-hide` prevents NVIDIA from being considered **at discovery time**, not just at usage time.

Specifically, it:

### 1. Dynamically discovers NVIDIA devices
(Since v2, without hardcoded card numbers)

- Scans `/sys/class/drm/*/vendor` to find NVIDIA DRM nodes
- Resolves the corresponding PCI BDFs via sysfs
- **In theory**, it should work regardless of enumeration order or node numbering

### 2. Hides NVIDIA from filesystem enumeration

- Filters NVIDIA entries from:
  - `/dev`
  - `/dev/dri`
  - `/dev/dri/by-path`
- Electron never “sees” NVIDIA nodes during probing

### 3. Blocks NVIDIA device access

- Denies opens of:
  - `/dev/dri/renderD*` (NVIDIA only)
  - `/dev/nvidia*`

### 4. Blocks NVIDIA userspace stacks

- Prevents loading of:
  - `nvidia-drm_gbm.so`
  - `libGLX_nvidia.so`
  - `libnvidia-*`
- Stops Chromium/Electron from selecting NVIDIA paths early

### 5. Prevents PCI-level probing

- Blocks reads of:
  - `/sys/.../<NVIDIA_BDF>/config`
- This avoids runtime PM resume even when `/dev/nvidia*` is blocked

### 6. Works with Electron’s multi-process model

- Each Electron subprocess loads the library
- Initialization happens once per process (expected and correct)

---

## What this project deliberately does *not* do

- No sandboxing
- No cgroup device filtering
- No group / permission changes
- No driver blacklisting
- No kernel patches
- No system-wide effects unless explicitly preloaded

---

## Limitations

- **Does not work with Flatpak / Snap apps**
  - LD_PRELOAD is blocked by design in sandboxed environments

---

## How to compile

This repo already contains a pre-compiled library for you to use directly. If you want to use that, you may skip this step.

### Requirements

- `gcc`
- `glibc` (libdl is part of glibc)

### Build steps

```bash
gcc -shared -fPIC -O2 -ldl -o libnvidia-hide.so libnvidia-hide.c
```

Recommended location:

```bash
mkdir -p ~/.local/lib
mv libnvidia-hide.so ~/.local/lib/
```

---

## How to use

### One-off (per launch)

```bash
LD_PRELOAD=$HOME/.local/lib/libnvidia-hide.so code
```

### Wrapper script (recommended)

```bash
mkdir -p ~/.local/bin

cat > ~/.local/bin/code <<'EOF'
#!/usr/bin/env bash
export LD_PRELOAD="$HOME/.local/lib/libnvidia-hide.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec /usr/bin/code "$@"
EOF

chmod +x ~/.local/bin/code
```

Ensure `~/.local/bin` comes before `/usr/bin` in `$PATH`.

### Desktop entry

```ini
Exec=env LD_PRELOAD=/home/USER/.local/lib/libnvidia-hide.so code %U
```

---

## Debugging

Enable debug output:

```bash
LIBNVIDIAHIDE_DEBUG=1 LD_PRELOAD=./libnvidia-hide.so code
```

Expected output (example):

```
[libnvidia-hide] init: nvidia_nodes=2 nvidia_bdfs=1
[libnvidia-hide]   node: card1
[libnvidia-hide]   node: renderD129
[libnvidia-hide]   bdf:  0000:01:00.0
```

Repeated output is normal as Electron spawns multiple processes.

---

## Verifying that the dGPU stays asleep

### Runtime PM status

```bash
watch -n0.5 cat /sys/bus/pci/devices/0000:01:00.0/power/runtime_status
```
- Here, `0000:01:00.0` is used as an example (NVIDIA card on my machine).

Should remain:

```
suspended
```

### Audit / tracing

```bash
sudo bpftrace -e '
tracepoint:syscalls:sys_enter_openat
/str(args->filename) ~ "nvidia|renderD"/
{ printf("%s %d %s\n", comm, pid, str(args->filename)); }'
```

With `libnvidia-hide` active, no NVIDIA opens should appear.

## Final notes

This is a **userspace-only workaround**:
- no kernel patches
- no driver removal
- no power-management hacks
  
You essentially force electron to behave like a good citizen.
