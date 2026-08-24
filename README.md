# HaikuSparc

Porting [Haiku](https://www.haiku-os.org/) to 64-bit SPARC workstation hardware — specifically
the **Sun Blade 150** and **Sun Ultra 10**, developed against QEMU's `sun4u` machine.

Haiku lists SPARC as an official Tier-2 architecture. The OpenFirmware bootloader works; the
kernel architecture layer is stubs. This repository is the work of closing that gap.

## Documents

| | |
| --- | --- |
| **[Porting plan](sparc-port/PORTING_PLAN.md)** | Current state of the port, the four hard problems, the phased plan with exit criteria, licensing policy, and upstream-sync strategy. |
| **[Hardware support matrix](sparc-port/HARDWARE_MATRIX.md)** | Every 64-bit Sun workstation mapped onto its silicon, against what Haiku already has drivers for. Drives the hardware sourcing decision. |

## Status

**The kernel boots from a real disk, mounts BFS, and runs userland programs.** A freestanding SPARC
binary loaded by the kernel's own ELF loader runs unprivileged, nests register windows across the
privilege boundary, makes real system calls into `libroot`'s stubs, receives a signal it sent
itself, and exits cleanly. Phases 0 through 6 are complete and Phase 7 is most of the way there.

The whole chain works. The loader boots from Sun-disklabelled media; the kernel takes the MMU and
trap table over from Open Firmware, builds its own three-level page table, brings up the slab
allocator and the VM, schedules and preempts threads, keeps time from `%TICK`, and drops into a
working kernel debugger. Then a new `busses/pci/sabre` driver publishes the host bridge — Haiku's
PCI bus manager does not *find* controllers, it attaches beneath one, and sun4u has neither ACPI nor
FDT to hang it off — and PCI enumerates through both `simba` bridges to the CMD646, which reaches a
disk over DMA through `generic_ide_pci` → `ata_adapter` → `ata` → `scsi_disk`, so `intel` can read
the partition map and `bfs` can mount.

Every phase's exit criterion is met by a deliberate test that prints its own result, rather than by
inference from the boot getting further:

| | |
| --- | --- |
| **2 — MMU** | maps a page it allocated itself with the firmware uninvolved; survives a TLB miss provoked by demapping a translation that exists only in its own TSB; survives a 24-frame recursion against 8 register windows |
| **3 — threads** | two kernel threads alternate a counter 128 times and the total comes out exact. The switch is twelve instructions, because on SPARC the callee-saved registers *are* the window registers |
| **4 — time** | `system_time()` off `%TICK`, the level-14 timer interrupt taken, and a thread that never yields taken off the CPU anyway |
| **5 — backtraces** | a trace walks across spilled windows and terminates at the fabricated frame Phase 3 builds for a thread that has never run |
| **6 — userspace** | contexts, per-address-space page tables, `ta 0x40`, TLS in `%g7`, windows across the boundary, signal delivery end to end |
| **7 — disk** | `bfs: mounted "Haiku" … device = /dev/disk/ata/0/slave/raw`, over DMA, with real device interrupts |

KDL works: `sc` prints a symbolised backtrace and `threads` prints the thread table.

What is left in Phase 7 is `winfixup` — a fault taken *during* a register-window spill or fill, the
one case the window machinery does not yet survive — and an `hme` driver for the Ultra 10's onboard
Ethernet, which is the other half of the phase's exit criterion and would validate DMA and
interrupts on a second device. Phase 8 is graphics, and needs hardware.

Around forty genuine bugs have been found and fixed along the way, a good many of them
architecture-neutral. Two are still carried by the PowerPC Open Firmware port. One, `PAGE_SHIFT`,
had been quietly corrupting physical memory for as long as the port existed. Several were not merely
untested but unreachable — Haiku's kernel never enters a program directly, and nothing on this port
called the trap-return hook that delivers signals, so an entire subsystem had been written, verified
by disassembly, and could not have run.

The running log is [PROGRESS.md](sparc-port/PROGRESS.md) — read it first on resume; it carries every
experiment, every failed approach and why. Design documents:
[PHASE2_MMU_DESIGN.md](sparc-port/PHASE2_MMU_DESIGN.md) for the MMU and trap table,
[USERSPACE_DESIGN.md](sparc-port/USERSPACE_DESIGN.md) for the privilege boundary.

## Scope

- **In:** `sun4u`, single CPU, the sabre generation — Ultra 5, Ultra 10, Blade 100, Blade 150.
- **Out:** 32-bit SPARC (`sun4c`/`sun4m`), SMP, `sun4v` / T-series, SCSI-only workstations.

## Licensing

Haiku is MIT. Ported code from OpenBSD, NetBSD and FreeBSD is BSD-family and used with its
original copyright notices intact; provenance is tracked in `sparc-port/THIRD_PARTY.md`. Linux is
GPL-2.0 and is used as a behavioural reference only — never copied. See
[§3.2 of the plan](sparc-port/PORTING_PLAN.md#32-donor-code-and-licensing).
