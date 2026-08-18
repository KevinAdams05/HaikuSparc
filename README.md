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

Planning. No code yet.

The immediate next steps are in [§10 of the plan](sparc-port/PORTING_PLAN.md#10-immediate-next-actions):
fork Haiku here, stand up the QEMU harness and cross toolchain, and get the loader to print its
banner over serial.

## Scope

- **In:** `sun4u`, single CPU, the sabre generation — Ultra 5, Ultra 10, Blade 100, Blade 150.
- **Out:** 32-bit SPARC (`sun4c`/`sun4m`), SMP, `sun4v` / T-series, SCSI-only workstations.

## Licensing

Haiku is MIT. Ported code from OpenBSD, NetBSD and FreeBSD is BSD-family and used with its
original copyright notices intact; provenance is tracked in `sparc-port/THIRD_PARTY.md`. Linux is
GPL-2.0 and is used as a behavioural reference only — never copied. See
[§3.2 of the plan](sparc-port/PORTING_PLAN.md#32-donor-code-and-licensing).
