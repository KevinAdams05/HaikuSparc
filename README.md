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

**The kernel runs its own MMU, page table and VM, schedules threads, and reaches the disk device
manager.** Phases 0–3 are complete, including Phase 2 — the MMU and trap table, the gate this port
has always turned on.

On QEMU's `sun4u` machine the loader boots from Sun-disklabelled media, mounts a BFS volume and
enters the kernel. The kernel brings up the platform, debug output, locking and interrupts, takes
the MMU and trap table over from Open Firmware, builds its own three-level page table, runs
`vm_page_init` and the slab allocator, creates its areas, and initialises the ELF loader, the
commpage and the scheduler, switches between kernel threads, and gets as far as
`KDiskDeviceManager::InitialDeviceScan()` — which finds nothing, because there are no disk drivers
yet.

All three of Phase 2's exit criteria are met by deliberate tests rather than by inference: it maps
a page it allocated itself with the firmware no longer involved, it survives a TLB miss provoked
by demapping a translation that exists only in its own TSB, and it survives a 24-frame recursion
against 8 register windows with the values arriving back intact.

Phase 3's exit criterion is met the same way: two kernel threads alternate a counter 64 times each
and the total comes out exact. The context switch is twelve instructions, because on SPARC the
callee-saved registers *are* the window registers — `flushw` spills them to the outgoing thread's
own stack, and the fill after `restore` pulls them from the incoming one.

Next is Phase 4, the timer and interrupts. Nothing preempts yet; every switch so far is
voluntary.

Twenty genuine bugs have been found and fixed along the way, several of them
architecture-neutral — including two the PowerPC Open Firmware port is still carrying, and one,
`PAGE_SHIFT`, that had been quietly corrupting physical memory for as long as the port existed.

The running log is [PROGRESS.md](sparc-port/PROGRESS.md), and the current design work is in
[PHASE2_MMU_DESIGN.md](sparc-port/PHASE2_MMU_DESIGN.md).

## Scope

- **In:** `sun4u`, single CPU, the sabre generation — Ultra 5, Ultra 10, Blade 100, Blade 150.
- **Out:** 32-bit SPARC (`sun4c`/`sun4m`), SMP, `sun4v` / T-series, SCSI-only workstations.

## Licensing

Haiku is MIT. Ported code from OpenBSD, NetBSD and FreeBSD is BSD-family and used with its
original copyright notices intact; provenance is tracked in `sparc-port/THIRD_PARTY.md`. Linux is
GPL-2.0 and is used as a behavioural reference only — never copied. See
[§3.2 of the plan](sparc-port/PORTING_PLAN.md#32-donor-code-and-licensing).
