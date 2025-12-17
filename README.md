# libnvidia-hide
**Prevent Electron apps from waking NVIDIA dGPU on hybrid laptops (LD_PRELOAD)**

---

## What this is

`libnvidia-hide` is a **userspace NVIDIA-hiding solution** for hybrid GPU laptops.

It consists of:

- a **shared library** (`libnvidia-hide.so`) that hides NVIDIA devices from a process using `LD_PRELOAD`
- a **launcher binary** (`nvidia-hide`) that applies the library *per application*, without requiring a global preload

The goal is to prevent Electron / Chromium-based applications (VS Code, Discord, Slack, etc.) from **waking the NVIDIA dGPU unnecessarily**, while keeping:

- full filesystem access
- portals, camera, microphone, clipboard
- normal desktop integration

No sandboxing, cgroups, udev rules, kernel patches, or driver removal are involved.

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

### 1. Dynamic NVIDIA detection

- Scans `/sys/class/drm/*/device/vendor`
- Identifies NVIDIA DRM nodes by vendor ID (`0x10de`)
- Resolves corresponding PCI BDFs dynamically
- No hardcoded card numbers or assumptions

### 2. Hides NVIDIA from filesystem enumeration

Filters NVIDIA-related entries from:

- `/dev`
- `/dev/dri`
- `/dev/dri/by-path`

As a result, Electron never “sees” NVIDIA devices during probing.

### 3. Blocks NVIDIA device access

Prevents access to:

- NVIDIA `renderD*` nodes
- `/dev/nvidia*` character devices

### 4. Blocks NVIDIA userspace stacks

Prevents loading of:

- `libGLX_nvidia.so`
- `nvidia-drm_gbm.so`
- `libnvidia-*`

This stops Chromium / Electron from selecting NVIDIA paths early.

### 5. Prevents PCI-level probing

Blocks reads of:

- `/sys/.../<NVIDIA_BDF>/config`

This avoids runtime PM wakeups even when character devices are blocked.

### 6. Works with Electron’s multi-process model

- Every Electron subprocess loads the library
- Initialization happens once per process
- Repeated debug output is expected and correct

---

## What this project deliberately does *not* do

- No sandboxing
- No cgroup device filtering
- No permission or group changes
- No driver blacklisting
- No kernel patches
- No system-wide preload unless you explicitly choose to do so

---

## Limitations

- **Does not work with Flatpak / Snap applications**
  - `LD_PRELOAD` is blocked by design in sandboxed environments

---

## Building

This repository from now on ships **source only**. You are expected to build locally.

### Requirements

- `gcc`
- `glibc` (for `libdl`)

### Build commands

```bash
gcc -O2 -fPIC -Wall -Wextra -std=c11 \
  -shared -ldl \
  -o libnvidia-hide.so libnvidia-hide.c


gcc -O2 -Wall -Wextra -std=c11 \
  -o nvidia-hide nvidia-hide.c
```

(Optional) install locations:

```bash
install -Dm755 libnvidia-hide.so /usr/local/lib/libnvidia-hide.so
install -Dm755 nvidia-hide /usr/local/bin/nvidia-hide
```

**Alternatively**, you can use the included makefile, and run:

```bash
make
sudo make install
```

---

## How to use

### Recommended: launcher-based usage (no global preload)

```bash
nvidia-hide run -- code
```

This:

- sets `LD_PRELOAD` only for that process tree
- automatically applies policy (allowlist / denylist)
- avoids polluting your entire desktop session

---

### Optional: manual LD_PRELOAD usage

If you want to preload manually:

```bash
LD_PRELOAD=/path/to/libnvidia-hide.so code
```

---

## Configuration: allowlist / denylist

The library decides whether it should be **active** per process by inspecting `/proc/self/exe`.

### Config files (recommended)

Location:

```text
~/.config/nvidia-hide/allowlist
~/.config/nvidia-hide/denylist
```

Format:

- one glob pattern per line
- `#` comments supported
- empty lines ignored

Matching rules:

- patterns **without `/`** match the executable basename
- patterns **with `/`** match the full executable path

Examples:

```text
# allow only VS Code
code
*/visual-studio-code/code
```

```text
# never hide NVIDIA from these
grep
bash
```

```text
# allow discord
echo "discord" > ~/.config/nvidia-hide/allowlist

# deny discord
echo "discord" > ~/.config/nvidia-hide/denylist 
```

### Precedence rules

1. If an allowlist exists, the library is **inactive unless matched**
2. Denylist always wins

If policy results in `active=0`, the library becomes a **true no-op**:

- no DRM probing
- no sysfs scanning
- no side effects

---

## Environment-based configuration (optional)

Instead of files, you may use env vars (colon-separated globs):

```bash
LIBNVIDIAHIDE_ALLOWLIST="code:electron*"
LIBNVIDIAHIDE_DENYLIST="bash:grep"
```

---

## Debugging

Enable verbose logging:

```bash
LIBNVIDIAHIDE_DEBUG=1 nvidia-hide run -- code
```

Example output:

```text
[libnvidia-hide] policy: exe=/opt/visual-studio-code/code
[libnvidia-hide] policy: active=1 (has_allow=1 allow_match=1 deny_match=0)
[libnvidia-hide] init: nvidia_nodes=2 nvidia_bdfs=1
[libnvidia-hide]   node: card1
[libnvidia-hide]   node: renderD129
[libnvidia-hide]   bdf:  0000:01:00.0
```

---

## Verifying that the dGPU stays asleep

### Runtime PM status

```bash
watch -n0.5 cat /sys/bus/pci/devices/0000:01:00.0/power/runtime_status
```

Expected:

```text
suspended
```

### Tracing NVIDIA opens

```bash
sudo bpftrace -e '
tracepoint:syscalls:sys_enter_openat
/str(args->filename) ~ "nvidia|renderD"/
{ printf("%s %d %s\n", comm, pid, str(args->filename)); }'
```

With `libnvidia-hide` active, no NVIDIA paths should appear.

---

## Final notes

This is a **userspace-only workaround**:
- no kernel patches
- no driver removal
- no power-management hacks
  
You essentially force electron to behave like a good citizen.
