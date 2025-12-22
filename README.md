# libnvidia-hide (Rust port)

This branch contains the **Rust port** of the C implementation from the other branch in the same repo.

This README intentionally **does not** re-explain what the project is or how the hiding logic works — see the C branch README for the full overview. This file only documents what’s *different* in the Rust port and how to build it.

---

## Noteworthy differences vs the C implementation

- **Nightly Rust required**: the preload hooks include **variadic** libc APIs (e.g. `open(…, …)`), which requires nightly via `#![feature(c_variadic)]`.
- **Toolchain pinned in-repo**: `rust-toolchain.toml` is included to select **nightly** automatically (and includes `rustfmt`/`clippy`).
- **`open64` handling adjusted**: the Rust port is written to avoid trying to “forward” a variadic argument list in a way that’s not supported / can be UB. (Nightly-safe behavior.)
- **Output name may differ**: by default, the built library may be named `libnvidia_hide.so` (underscore) rather than `libnvidia-hide.so` (dash). You can symlink/rename if you want the same filename as the C branch.

---

## Building

### Install Rust (Arch)

Install `rustup` and the nightly toolchain:

```bash
sudo pacman -S rustup
rustup toolchain install nightly
```

### Build

In the repo directory, either:

- rely on `rust-toolchain.toml` (auto nightly), **or**
- build explicitly:

```bash
cargo +nightly build --release
```

### Artifacts

After a successful build, look in:

- `target/release/` for the compiled `.so` (and the launcher binary, if this branch includes it)

If you want a dash-named library like the C branch:

```bash
ln -sf target/release/libnvidia_hide.so ./libnvidia-hide.so
```

**OR** rename the file (which is what I would do).
