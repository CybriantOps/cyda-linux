# The CYDA kernel

CYDA OS ships its own kernel: Linux plus the CYDA patches and the CYDA
configuration. A stock distribution kernel boots the image too (that is
how development started), but the project's direction is a kernel that
knows what an agent is.

```text
kernel/
├── build.sh          source → patches → config → bzImage   (kernel/out/vmlinuz-cyda)
├── cyda.config       configuration fragment merged over x86_64 defconfig
├── patches/          CYDA patches, applied in order
│   └── 0001-cyda-kernel-interface.patch   /sys/kernel/cyda/{version,agents}
├── src/cyda/         the patch's sources, kept readable (cyda.c, Kconfig, Makefile)
└── out/              build products (not committed)
```

## Build

```bash
scripts/dev-setup.sh        # once (apt: linux-source, flex, bison, libssl-dev, libelf-dev, bc, zstd)
kernel/build.sh             # ~15 min on 4 cores; incremental afterwards
scripts/boot-test.sh        # boots the image on kernel/out/vmlinuz-cyda in QEMU/OVMF
```

`build.sh` takes the source from `--source DIR`, `$CYDA_KERNEL_SRC`,
`kernel/linux` (a git clone of Linux), or the Ubuntu `linux-source`
package in `/usr/src`. Applied patches are recorded in
`<source>/.cyda-patches-applied`, so a tree is never patched twice.
`kernel/check-patches.sh` applies the whole stack to pristine sources
without fuzz and compares the result with `kernel/src/`; CI runs it
before every build and boots the image on the resulting kernel.

## Configuration principles (`cyda.config`)

* **No loadable modules.** One kernel, one binary. Every driver the image
  needs is built in; the initramfs carries no `/lib/modules`.
* **Deterministic scheduling.** `PREEMPT`, `HZ_1000`, cgroup v2 with the
  cpu, cpuset, freezer, pids, memory controllers. RT group scheduling is
  *off*: under cgroup v2 it would forbid `SCHED_FIFO` for any thread
  outside the root group, and CRITICAL agents must be able to go
  real-time from their own cgroup.
* **Boots anywhere x86-64 UEFI.** EFI stub, GPT, devtmpfs, ext4/vfat,
  common Intel/Realtek NICs, AHCI/NVMe/USB storage, serial and
  framebuffer consoles.
* **Warnings stay warnings** (`CONFIG_WERROR=n`): vendor trees carry them.

## Patch 0001: the kernel interface

`CONFIG_CYDA` adds `/sys/kernel/cyda/version` and
`/sys/kernel/cyda/agents`, one line per agent thread
(`tid name policy nice cpu_ns`), and prints the CYDA banner at boot. The
runtime names every agent thread `agent:<id>`, so the kernel can already
enumerate agents without new syscalls. `cyda kernel` shows this view next
to the runtime's own.

## What comes next (see `docs/ROADMAP.md`, v0.2.5)

1. `struct cyda_agent` in the kernel, created through `/dev/cyda`.
2. `SCHED_AGENT`: an agent-aware scheduling class.
3. An LSM enforcing agent capabilities at the syscall boundary.
4. Per-agent time budgets and watchdogs.
5. Agents started from a signed manifest before user space.

Each step lands as a patch in `kernel/patches/`, with the sources in
`kernel/src/`, and is verified by the boot test before it is merged.

## Patch 0002: the agent object

`struct cyda_agent` (`include/linux/cyda.h`) is bound to a thread through
a new `task_struct::cyda_agent` pointer, created by `ioctl(/dev/cyda,
CYDA_IOC_REGISTER)` from the thread itself, updated with `CYDA_IOC_UPDATE`
(priority, state, capability bitmap), and released in `do_exit()` — an
agent identity cannot outlive its thread and is never inherited by
`fork`/`clone`. `CYDA_IOC_QUERY` looks an agent up by tid.
`/sys/kernel/cyda/agents` lists registered agents:

```text
tid id priority state capabilities policy nice cpu_ns
89  machine-01 70 normal 0x71b 0 -8 588000000
90  vision-qc  90 critical 0x1a0 1 -4 390000000
```

The user API is `include/uapi/linux/cyda.h`; `cyda-kernel/src/dev.rs`
mirrors it and the runtime's `AgentWorker` registers every agent thread
on the CYDA kernel (a no-op elsewhere). The capability bits are shared
with `cyda_core::Capability::kernel_bit`.

With the identity in the kernel, the next patches can act on it:
`SCHED_AGENT` (scheduling by agent priority/state instead of cgroup
knobs) and the capability LSM (refusing a `connect()` to a PLC from a
thread whose agent lacks `PLC_WRITE`).

## Patch 0003: the capability LSM

`security/cyda/` is a stacking LSM (`CONFIG_SECURITY_CYDA`, first in
`CONFIG_LSM`). For threads that are agents (`current->cyda_agent`):

| Hook                | Refused unless the agent holds                              |
|---------------------|-------------------------------------------------------------|
| `socket_connect`    | a device capability (`PLC_*`, `MODBUS_*`, `OPCUA_READ`, `RESOURCE_READ`) for AF_INET/AF_INET6 |
| `file_open`         | `CAMERA_READ` for `/dev/video*` (major 81); `GPU_USE` for DRM (226) / NVIDIA (195); cgroup writes: never |
| `task_setscheduler` | never — an agent does not reschedule itself                 |
| `task_setnice`      | never                                                       |

Denials are `-EPERM`, counted in `/sys/kernel/cyda/denied` and logged
rate-limited. Threads that are not agents (the runtime, the console) are
untouched, so the runtime still places and elevates agents from outside.

`cyda probe <agent> <host:port>` opens a raw TCP connection from the
agent's own thread, bypassing the system layer's gates on purpose: on the
CYDA kernel `vision-qc` (no PLC capability) gets `EPERM`, `machine-01`
connects. The boot test asserts both.

## Patch 0004: endpoint allow-lists

`CYDA_IOC_SET_ENDPOINTS` stores up to 8 `(family, address, port)` entries
on the agent object. When an agent has a list, the LSM's `socket_connect`
also requires the destination to be on it; with no list, any address is
allowed as long as the agent holds a device capability. The runtime fills
the list from the addresses of the physical resources the agent owns, so
`machine-01` can reach `10.0.2.2:5020` and nothing else, even with
`PLC_WRITE`. `/sys/kernel/cyda/agents` shows the endpoint count;
`cyda kernel` prints it as ENDPOINTS (`any` = no list).

## Patch 0005: watchdog and quarantine

Deterministic execution needs a guarantee that a faulting agent stops
acting. Every agent thread arms a watchdog (`CYDA_IOC_SET_WATCHDOG`,
three periods, at least 3 s) and heartbeats (`CYDA_IOC_HEARTBEAT`) after
every step and while idle. If the watchdog expires the kernel marks the
agent `faulted`, counts it in `/sys/kernel/cyda/faults`, moves the thread
to `SCHED_IDLE` and the LSM refuses it every network and file access. A
single thread cannot be killed, so the quarantined thread keeps existing
harmlessly; the runtime gives up on it after the same timeout, marks the
agent FAILED, and `cyda restart` gives the agent a fresh thread while
Context and Memory survive. `cyda hang <agent>` demonstrates the path;
the boot test asserts it.
