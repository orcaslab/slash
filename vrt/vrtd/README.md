# vrtd — V80 Runtime Daemon

vrtd is a systemd-managed daemon written in C (gnu11) that multiplexes
access to AMD Alveo V80 FPGA devices managed by the SLASH kernel
module. It communicates with client applications over an AF_UNIX
socket using a binary request/response protocol, passing device file
descriptors out-of-band via `SCM_RIGHTS`.

## Architecture

vrtd sits between the client libraries and the low-level driver stack:

```
libvrtdpp  (C++ RAII wrapper)
libvrtd    (C wire-protocol client library)
──────── AF_UNIX / SOCK_SEQPACKET ────────
vrtd       (this daemon)
libslash   (driver wrapper)
Linux kernel module (slash)
AMD Alveo V80 hardware
```

Key responsibilities:

- Sysfs-based automatic device discovery at startup
- Per-client DMA buffer management (HBM and DDR) with automatic
  cleanup on disconnect
- FPGA bitstream programming via the design writer subsystem, skipping
  designs that are already loaded
- Clock frequency control via AXI clock wizard
- PCIe hotplug (secondary bus reset) support
- Role-based access control with per-device granularity
- Systemd integration: socket activation, watchdog, sd-event loop,
  journal logging

## Design writes

A client programs a design by sending `VRTD_REQ_DESIGN_WRITE` with a file
descriptor of the PDI. vrtd parks the user clock, transfers the PDI to the
card and refreshes PF2. Every design write on a card goes through vrtd, so
vrtd knows which design the card holds and does not write it twice.

**Design UUID.** `slashkit` stamps a random UUID into each vbin
(`<UUID>` in `system_map.xml`). The request carries it. For each device vrtd
keeps the UUID of the loaded design. The all-zero UUID means unknown.

| Event | Loaded UUID |
|-------|-------------|
| Daemon start | unknown |
| Device object rebuilt (reset sequence, shell switch, remove + rescan) | unknown |
| Bus reset or hotplug of a function, cfgmem program, `SET_SHELL_STATE` | unknown |
| Design write accepted, transfer not finished | unknown |
| Design write failed or interrupted | unknown |
| Design write completed | UUID of the request (unknown if the request has none) |

**Skip.** A request whose UUID is non-null and equal to the loaded UUID is
answered at once with `skipped` set in the response. Nothing is written, the
clock is not parked and PF2 is not refreshed. A request is never skipped when
it carries `VRTD_DESIGN_WRITE_FORCE`, when it needs another shell, or when the
card needs a reset.

**References.** The client whose request wrote the design, and every client
whose request was skipped, holds a reference on the design until its
connection closes. When the last holder disconnects, vrtd parks the user
clock; the design stays loaded and known.

**In use.** A request that would replace the design (another UUID, no UUID, or
`VRTD_DESIGN_WRITE_FORCE`) is refused with `VRTD_RET_BUSY` while a connection
other than the requester holds a reference. The log names the holder. A client
that is the only holder may replace its own design.

**Several PDIs.** A vbin with several PDIs is one design. The client sends one
request per PDI with the same UUID and sets `VRTD_DESIGN_WRITE_MORE` on all
but the last. vrtd records the UUID when a request without that flag
completes. Only the first request can be skipped; the client then skips the
rest.

**Query.** `VRTD_REQ_GET_DESIGN_INFO` returns the loaded UUID and the number of
holders. `v80-smi list` shows both.

vrtd cannot see a design that was loaded behind its back (for example over
JTAG while the daemon keeps running). After such a load, report it with
`SET_SHELL_STATE`, restart the daemon, or force the next write.

The rules live in `src/design_state.c`, which does no I/O and is covered by
`tests/design_state_test.cpp`. `src/serve.c` applies them.

## Directory layout

| Path | Contents |
|------|----------|
| `src/` | Daemon source code (C11) |
| `include/vrtd/` | Public wire-protocol headers shared with libvrtd |
| `libvrtd/` | C client library for the vrtd wire protocol |
| `libvrtdpp/` | C++ RAII wrapper around libvrtd |
| `conf/` | Default configuration file (`vrtd.conf`) |
| `systemd/` | systemd service and socket unit files |
| `sysusers/` | systemd-sysusers configuration (vrtd user/group) |
| `udev/` | udev rules for device permissions |
| `cmake/` | CMake config-file templates |

## Building

**Prerequisites:** libslash must be installed first (or built in-tree
with `-DVRTD_INCLUDE_LIBSLASH=ON`). System dependencies:

```bash
sudo apt install cmake pkg-config libsystemd-dev libinih-dev
```

**Build:**

```bash
cd vrt/vrtd
cmake -B build -S . -G Ninja
cmake --build build
sudo cmake --install build
```

## Running

```bash
# Manual
sudo vrtd

# Production (systemd)
sudo systemctl enable --now vrtd
```

The daemon reads its configuration from `/etc/vrt/vrtd.conf` (see
`conf/vrtd.conf` for the default format). The configuration file uses
an INI-style format to define roles, user permissions, and per-device
access rules.

## Minimum toolchain versions

The baseline is Ubuntu 22.04 LTS/RHEL 9. Lower versions may work but are
untested and may break in any update.

| Dependency | Minimum version |
|------------|-----------------|
| CMake | 3.22.1 |
| GCC | 11.4.0 |
| glibc | 2.35 |
| Linux | 5.15.0 |
| libsystemd | 249.11 |

Developers contributing to vrtd are encouraged to make use of useful
extensions and capabilities as long as they are supported by the
versions listed above.

## Coding conventions

**Read this section before writing C code for this daemon.**

This daemon is not written in standard/POSIX C, but instead leans
heavily on C11, libsystemd, glibc features, Linux syscall features,
and GNU compiler extensions (also supported by Clang). The goal is not
to write a portable application, but a modern systemd daemon using all
the tools at our disposal.

The language version is **C11** with GNU extensions (`-std=gnu11`).
C23 features should not be used unless they are available as GNU
extensions.

All `.c` source files must start with `#define _GNU_SOURCE` after the
copyright header and before including any other headers.

For the complete coding style guide — error handling, macros, ownership
rules, RAII patterns, GNU extensions, naming, and formatting — see
**[STYLE.md](STYLE.md)**.

## License

MIT — see [LICENSE](../../LICENSE).
