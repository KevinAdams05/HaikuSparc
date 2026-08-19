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

**The kernel boots and runs its early initialisation.** On QEMU's `sun4u` machine the loader
boots from Sun-disklabelled media, mounts a BFS volume, loads and enters the kernel, and the
kernel brings up the platform, debug output, locking and interrupts before reaching `vm_init`.

Phase 2 — the MMU and trap table, the gate this port has always turned on — is in progress. The
TSB is allocated and its arithmetic verified against the hardware; the TLB miss fast path, the
window spill/fill handlers and the unhandled-trap handler are written. None of it is installed
yet: `%tba` still belongs to Open Firmware.

Fourteen genuine bugs have been found and fixed along the way, several of them
architecture-neutral and so broken for the PowerPC Open Firmware port too.

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
