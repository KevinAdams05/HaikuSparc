# HaikuSparc

Porting [Haiku](https://www.haiku-os.org/) to 64-bit SPARC workstation hardware — specifically
the **Sun Blade 150** and **Sun Ultra 10**, developed against QEMU's `sun4u` machine.

Haiku lists SPARC as an official Tier-2 architecture. The OpenFirmware bootloader works; the
kernel architecture layer is stubs. This repository is the work of closing that gap.

## Documents

| | |
| --- | --- |
| **[Porting plan](sparc-port/PORTING_PLAN.md)** | Current state of the port, the four hard problems, the phased plan with exit criteria, licensing policy, and upstream-sync strategy. |
| **[Hardware support matrix](sparc-port/HARDWARE_MATRIX.md)** | Every 64-bit Sun workstation mapped onto its silicon, against what Haiku already has drivers for. Drives the hardware sourcing decision — and §8 carries a note on how that decision changed once `hme` and the CMD646 driver existed. |
| **[First boot on hardware](sparc-port/FIRST_BOOT.md)** | **Read this before powering on a machine.** The serial console has a condition attached that will otherwise give a silent kernel, and there are two `setenv` lines to type before anything else. |
| **[Progress log](sparc-port/PROGRESS.md)** | Every experiment, failed approach and correction, in order. Long, and the place to look when something does not make sense. |
| **[Upstream delta](sparc-port/UPSTREAM_DELTA.md)** | Every change to shared, cross-architecture Haiku files — the entire merge-conflict surface, one row each. |

## Status

**Phases 0 through 7 are complete, and the next thing that blocks the project is owning a machine.**

The kernel boots from a real disk, mounts BFS over DMA, runs dynamically linked programs against the
real `runtime_loader` and `libroot`, and answers on the network — ARP both ways, an ICMP echo request
out and the reply delivered to the program that asked for it, read out of a capture the *host* took
rather than from the driver's own opinion.

Four userland tests exist and pass, each installed where the launch daemon goes and ordered by how
much it puts between itself and the thing it tests: a freestanding assembly rig for the kernel alone,
a dynamically linked program for the loader and `libroot`, one that brings up the interface and pings,
and one that gets a system call interrupted by a signal and checks it resumes with its deadline
intact. `boot-test.sh TAG --user-test|--dynamic-test|--net-test|--sig-test` runs any of them from a
clean tree in one command.

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

### What is left, and why it waits

**Hardware.** Phase 8 is graphics and input, and the plan has always said it gets validated on real
silicon rather than in the emulator: QEMU's sun4u graphics "work during firmware and early boot but
fail when an OS switches into graphics mode". So does everything else outstanding — netboot, which
fails under OpenBIOS and should work under real OpenBoot; confirming the CMD646's read-to-clear
interrupt latch, which QEMU does not model; and the Blade 150's ALi M5229 IDE controller, which has
neither driver nor emulator.

Everything that *could* be done from the manuals ahead of a machine has been. The instruction-cache
flush and memory barriers are written; the TLB/TSB locking the manual requires and an emulator
forgives is in place for the trap handlers, the trap table, the trap data block and the TSB;
Erratum 51 is documented and unviolated; every Open Firmware property the kernel reads now names
itself when missing; the device tree is dumped on every boot, which is the day-one capture the plan
asks for, automatically.

**A real Haiku image, deliberately behind hardware.** `jam @minimum-raw` builds 4786 targets and fails
37 — `libbe.so`, `app_server`, `registrar` and `launch_daemon` all compile. Of the 37, 24 were Zydis
not knowing about SPARC (fixed) and 13 are a `gcc_syslibs` version mismatch that **cannot** be fixed
by bumping the package pin, and which blocks only the Installer, the HTTP kit, printing and one MIME
utility. None of that is on the path to booting, and for a first boot on silicon the hand-assembled
test volume is the better thing to carry: smaller, faster to rebuild, one thing per test.

Somewhere over fifty genuine bugs have been found and fixed along the way, a good many of them
architecture-neutral. Two are still carried by the PowerPC Open Firmware port. One, `PAGE_SHIFT`,
had been quietly corrupting physical memory for as long as the port existed. Several were not merely
untested but unreachable — Haiku's kernel never enters a program directly, and nothing on this port
called the trap-return hook that delivers signals, so an entire subsystem had been written, verified
by disassembly, and could not have run.

The running log is [PROGRESS.md](sparc-port/PROGRESS.md) — read the last two or three sections first
on resume, not the whole thing; it carries every experiment, every failed approach and why, and it is
long. §10 of the plan says what to do next in one place. Design documents:
[PHASE2_MMU_DESIGN.md](sparc-port/PHASE2_MMU_DESIGN.md) for the MMU and trap table,
[USERSPACE_DESIGN.md](sparc-port/USERSPACE_DESIGN.md) for the privilege boundary.

## Resuming after a break

Everything needed to get back to a booting kernel in one command is in the tree; nothing lives in a
scratch directory any more.

```sh
cd generated.sparc
../../buildtools/jam/bin.linuxx86/jam -q -j8 kernel_sparc     # jam is not on PATH
cd ..
./sparc-port/tools/boot-test.sh check --user-test              # expect: usertest deep/winok/sig/ok
```

Then read **[§10 of the plan](sparc-port/PORTING_PLAN.md#10-immediate-next-actions)** for what is
next, and the last two or three sections of the progress log for how the previous session ended.

**What will have gone stale, and is expected to:**

- **The work directory.** `boot-test.sh` writes to `/tmp/haiku-sparc-boot`, which `/tmp` cleaning
  removes. It recreates itself; only old logs are lost. Set `BOOT_TEST_WORK` to keep them.
- **Numbers in the documents.** Target counts, failure counts and package versions were measured on
  the date beside them. Re-measure rather than quote — that is a habit the log has had to learn
  twice, most recently in §54 and §55.
- **`jam` does not rebuild on a Jamfile change.** Remove the target first, or a stale object will be
  linked without a word about it.
- **Hardware availability.** The matrix's sourcing advice is about silicon, not about what is for sale
  in any given month, and §8 now carries a note on how the ranking changed once `hme` and the CMD646
  driver existed.

## Scope

- **In:** `sun4u`, single CPU, the sabre generation — Ultra 5, Ultra 10, Blade 100, Blade 150.
- **Out:** 32-bit SPARC (`sun4c`/`sun4m`), SMP, `sun4v` / T-series, SCSI-only workstations.

## Licensing

Haiku is MIT. Ported code from OpenBSD, NetBSD and FreeBSD is BSD-family and used with its
original copyright notices intact; provenance is tracked in `sparc-port/THIRD_PARTY.md`. Linux is
GPL-2.0 and is used as a behavioural reference only — never copied. See
[§3.2 of the plan](sparc-port/PORTING_PLAN.md#32-donor-code-and-licensing).
