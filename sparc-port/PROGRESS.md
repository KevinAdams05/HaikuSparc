# Progress notes

Running log of what has been done, what was learned, and what to pick up next. Deliberately
separate from [PORTING_PLAN.md](PORTING_PLAN.md): the plan describes the shape of the work and
should stay stable; this file changes constantly. Findings get promoted into the plan only when
they change the plan.

**State: Phases 0–3 complete. Phase 4 is half done: the clock and the timer interrupt work,
preemption does not.**

The loader boots from Sun-disklabelled media, mounts a BFS volume and enters the kernel. The
kernel takes the MMU and trap table over from Open Firmware, builds its own three-level page
table, runs the VM and slab allocator, initialises the ELF loader, the commpage and the scheduler,
switches between kernel threads, **keeps time from %TICK and takes the timer interrupt**, and gets
as far as `KDiskDeviceManager::InitialDeviceScan()` — which finds nothing, because there are no
disk drivers yet.

Exit criteria met, each by a deliberate test rather than by inference:

| Phase | | |
| --- | --- | --- |
| 2 | Maps a page it allocated itself | after the cutover `early_map` writes only the page table and TSB (§21) |
| 2 | Survives a provoked TLB miss | translation demapped, read back correctly — no route but the fast path (§21) |
| 2 | Survives a forced window overflow | 24 frames against 8 windows, sum exact (§21) |
| 3 | Two threads hand control back and forth | `counter 128 of 128 after 122 yields` (§23) |
| 4 | `system_time()` is right | monotonic, and agreeing with the raw %TICK delta (§24) |
| 4 | **A tick preempts a busy loop** | **not met** — see §24 |

What exists:

| | |
| --- | --- |
| Page table | three levels, 1024 entries each, physical interior pointers, TTE leaves (§22) |
| TSB | 256 KB split pair, a cache in front of the page table |
| TLB miss fast path | 10 instructions; slow path walks the page table in about 20 more (§22) |
| Trap table | 32 KB, geometry asserted at build time and re-checked every boot (§20) |
| Failure reporting | unresolved misses and unhandled traps return to TL=0 and panic (§22) |
| `VMTranslationMap` | `SPARCVMTranslationMap`, wired into `create_map` (§22) |
| Context switch | twelve instructions, built on the window spill and fill handlers (§23) |
| Clock | `system_time()` from %TICK and the firmware's clock-frequency (§24) |
| Timer interrupt | entry path, C handler, `%TICK_CMPR` arming (§24) |

**Next: finish Phase 4.** Preemption is the one thing missing, and §24 says exactly what is known
about why.

**Open, and not on that path:** `wait_for_thread()` trips `could acquire exit_sem for thread 5`
(§23); the boot ends at `did not find any boot partitions!`, which is the absence of disk drivers;
and window-aware backtraces (Phase 5) are still not written, which the plan warned against
deferring.

Read [PHASE2_MMU_DESIGN.md](PHASE2_MMU_DESIGN.md) before touching any of it: the mechanism, the
sizing, the table layout and the QEMU-fidelity verification are all there.

---

## 1. Environment — exact, reproducible

Everything runs on the Mint 22.3 desktop (24 cores, 31 GB, 1.5 TB free), *not* the usual Haiku
build server at 192.168.74.122, because QEMU lives here and the build/test loop wants one machine.

| Thing | Where |
| --- | --- |
| Fork | `/home/kevin/Code/Haiku/SPARC/src` — branches `master` (pristine upstream) and `sparc/main` |
| Buildtools | `/home/kevin/Code/Haiku/SPARC/buildtools` — a **clean clone**; the shared one at `~/Code/Haiku/build/buildtools` is dirty from an old in-tree build, do not use it |
| Cross toolchain | `SPARC/src/generated.sparc/cross-tools-sparc/bin/sparc64-unknown-haiku-*`, GCC 13.3.0 |
| jam | `SPARC/buildtools/jam/bin.linuxx86/jam` — built with plain `make`; there is no system jam |
| QEMU | `/usr/bin/qemu-system-sparc64` 8.2.2, already installed |
| Firmware | `/usr/share/qemu/openbios-sparc64`, OpenBIOS v1.1 |
| Scratch | `/home/kevin/Code/Haiku/SPARC/` (parent) — never pushed |

Host packages installed: `flex bison gawk gdb-multiarch`. QEMU did **not** need installing.
Stock gdb rejects `set architecture sparc:v9`; `gdb-multiarch` accepts it.

```sh
# toolchain, once
cd SPARC/src && mkdir -p generated.sparc && cd generated.sparc
../configure --use-gcc-pipe -j24 \
    --cross-tools-source /home/kevin/Code/Haiku/SPARC/buildtools --build-cross-tools sparc

# loader, each iteration
JAM=/home/kevin/Code/Haiku/SPARC/buildtools/jam/bin.linuxx86/jam
cd SPARC/src/generated.sparc && $JAM -q -j24 haiku_loader.openfirmware
```

---

## 2. Fixes made so far

Everything below is committed and pushed. The bugs found so far, in the order they were hit:

| # | Where | What |
| :-: | --- | --- |
| 1–4 | `openfirmware/{devices,network,video}.cpp`, `loader/menu.cpp` | Four `-Werror` failures that stopped the loader compiling at all |
| 5 | `openfirmware/arch/sparc/mmu.cpp` | **PowerPC page-protection constants passed to a sun4u MMU** — §3 |
| 6 | `openfirmware/devices.cpp` | Boot device opened by a path naming a *file*, not the device — §6 |
| 7 | `openfirmware/devices.cpp` | **A 64-bit Open Firmware handle truncated to `int`** — §7a |
| 8 | `openfirmware/arch/sparc/start.cpp` | The FPU was never enabled — §7a |
| 9 | `kernel/arch/sparc/arch_elf.cpp` | `R_SPARC_WDISP30` fell through into `HI22`, corrupting 196 call sites — §13 |
| 10 | `kernel/arch/sparc/arch_elf.cpp` | **`R_SPARC_RELATIVE` written 32 bits wide**, so every GOT entry held its address shifted left by 32 — §15 |
| 11 | `openfirmware/arch/sparc/arch_start_kernel.S` | The kernel was entered with interrupts enabled, failing its first assertion — §16 |
| 12 | `openfirmware/console.cpp` | Three-byte console reads the firmware rejects, 570k warnings and unreliable input per boot — §16 |
| 13 | `kernel/arch/sparc/arch_platform.cpp` | **Kernel debug output silently discarded** whenever a frame buffer existed — §17 |
| 14 | `kernel/arch/sparc/arch_elf.cpp` | `WDISP22`/`WDISP19` unimplemented, so a kernel containing either would not load — §19 |

Several are architecture-neutral, so the Open Firmware loader is bit-rotted for PowerPC too.
Haiku's own guide claims the loader runs. Discount similar claims about this port.

`-Wstack-usage=1023` applies to the `openfirmware` boot target and **no other**
(`build/jam/ArchitectureRules:554`), because this loader runs on the firmware's small stack. So
the stack consumers wanted fixing, not the limit — even though SPARC's mandatory 176-byte
register-window save area makes every frame larger.

---

## 3. The MMU protection bug — the one that unblocked everything

`mmu.cpp` carried:

```c
#define PAGE_READ_ONLY   0x0002
#define PAGE_READ_WRITE  0x0001
```

Those are **PowerPC PTE `PP` protection bits**, inherited from the PowerPC loader this file was
copied from. They mean nothing to a sun4u MMU, where the mode argument to Open Firmware's `map`
method is a **TTE data value** and **bit 1 is Writable** (UltraSPARC-IIi User's Manual,
FIGURE 15-1, printed p.205).

So a request for writable memory passed `0x1` — which sets `G` (Global), not `W` — producing a
**read-only** mapping. The loader allocated its heap, printed `heap base = …`, wrote to it, and
took trap `0x6c` = `fast_data_access_protection`.

Two details confirm the diagnosis rather than merely fitting it:

- The same file's `Mode()` helper decodes an existing OF translation with `if (data & 2)` — the
  author knew the sun4u W bit is `0x2`, and the constants were simply never corrected.
- NetBSD passes `-1` for this argument everywhere, commented `-1/* sunos does this */`
  (`sys/arch/sparc64/sparc64/pmap.c`). `-1` asks the firmware for its default mode: cacheable,
  privileged, writable.

The fix passes `-1`, widening the parameter to `int64` so it sign-extends into a full OF cell
rather than becoming `0xFFFF`. **Effect: the loader immediately got from a heap fault all the
way to `Welcome to the Haiku boot loader!`**

---

## 4. Booting from real media — it works

```
0 > boot /pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\loader.elf
Not a Linux kernel image
init_real_time_clock(): Found no RTC device!
heap base = 0x000000000080c000
Welcome to the Haiku boot loader!
Haiku revision:
boot path = "/pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\loader.elf"
boot type = block
add_partitions_for(0x000000000080c258, mountFS = no)
0x000000000080c2b8 Partition::Scan()
check for partitioning_system: Intel Partition Map
Unhandled Exception 0x0000000000000030      <- inside OpenBIOS, see section 7
```

**The working recipe**, reproducible end to end:

```sh
L=generated.sparc/objects/haiku/sparc/release/system/boot/openfirmware/boot_loader_openfirmware

dd if=/dev/zero of=fs.ext2 bs=1M count=16
mkfs.ext2 -q -F -r 0 -b 1024 fs.ext2          # -r 0 is REQUIRED, see below
debugfs -w -R "write $L loader.elf" fs.ext2   # no mount, no root needed

python3 sparc-port/tools/make-sun-image.py \
    --payload fs.ext2 --output disk.img --start-cylinder 1 --size-mb 64

qemu-system-sparc64 -M sun4u -cpu "TI UltraSparc IIi" -m 512 -nographic \
    -bios /usr/share/qemu/openbios-sparc64 \
    -drive file=disk.img,format=raw,if=ide,media=disk
# then at the ok prompt:
#   boot /pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\loader.elf
```

Three requirements, each learned by having it fail:

1. **The ELF, not the a.out.** `boot_loader_openfirmware` (ELF64 MSB SPARC V9, entry 0x202000)
   is the real loader; `haiku_loader.openfirmware` is an a.out wrapper. OpenBIOS carries a
   `/packages/elf-loader`, and the Tabby PowerPC port likewise boots `haikuloader.elf`.
2. **ext2 revision 0.** `mkfs.ext2` without `-r 0` produces a filesystem OpenBIOS's `grubfs`
   cannot read — the boot fails with a bare `File not found`. This cost a confusing rebuild
   cycle when a "no-op" change to the recipe silently broke it.
3. **Partition a must not start at cylinder 0**, or the payload overwrites the disk label.

---

## 5. Boot media — everything that was tried

Each of these is a data point, not a dead end.

| Attempt | Result |
| --- | --- |
| `-kernel` with the **a.out** | `[sparc64] Kernel already loaded`, then traps at PC 0 |
| `-kernel` with the **ELF** | Loader runs, but at the wrong address — see below |
| `-boot n`, TFTP, both formats | `Trying net...` then `No valid state has been set by load or init-program` |
| `boot cdrom:,\loader.elf` | `File not found`; `cdrom` is not a valid alias, `devalias` is empty here |
| `dir <full-ide-path>:,\` | **Crashes OpenBIOS**: trap 0x34 (`mem_address_not_aligned`) at PC `0xffd1c184` |
| Raw ELF at device offset 0 | `Not a bootable ELF image` (plus Linux / a.out / FCode) |
| Sun label, ELF at partition offset 0 | Same four rejections |
| **Sun label + ext2 rev 0 + `:a,\loader.elf`** | **Boots.** |

**`-kernel` is a dead end and the reason matters.** QEMU reports `kernel phys 202000 virt
40004000`, and the loader then faults at `0x40010888` — it executes at an address it was not
linked for. It also cannot establish the Open Firmware client calling convention: `_start` takes
the OF entry point as its *fifth* argument (`%o4`), which only a real `boot` supplies. Useful as
a smoke test, nothing more.

**A control experiment worth remembering.** When the raw-ELF-on-disk attempts failed, a
sixteen-instruction hand-written SPARC ELF was put on the same image and failed *identically* —
proving the problem was media layout, not our binary. Bisecting with a trivial payload took two
minutes and saved a long hunt through the loader.

---

## 6. The boot-device path fix

`platform_add_boot_device()` did:

```c
int handle = of_open(sBootPath);
```

`sBootPath` is the **whole** boot path including the file: `…/disk@0:a,\loader.elf`. `of_open`
honours the file argument, so the handle is an instance of the *file inside ext2*; the partition
scan that follows then issues block reads against a filesystem instance.

`of_finddevice()` hides this, because it ignores everything after the `:` — which is why
`boot type = block` prints correctly and the problem only shows up later.

The fix keeps the `:partition` suffix and drops the `,file` argument. **The comma must be looked
for after the last colon**: Open Firmware device paths contain commas of their own, as in
`pci@1fe,0`. A naive `strchr(path, ',')` would truncate the path at the *bus address*.

This did not change the observed failure — see §7 — but it is correct, and it removes a real
class of misbehaviour.

---

## 7a. RESOLVED — it was never a firmware `seek` bug

**§7 below is retained as a record of a wrong diagnosis, because the way it was wrong is
instructive.** The conclusion "OpenBIOS's `seek` is broken" was well-evidenced and still wrong.

Two further bugs were found, both ours:

**The handle was being truncated.** `platform_add_boot_device()` had
`int handle = of_open(devicePath)`. `of_open` returns `intptr_t`; on sparc64 storing it in an
`int` drops the high half, and the value then sign-extends back into something that is not a
valid instance handle. The firmware dereferenced it and took a data access exception *inside
itself* — which is exactly why the trap PC was in OpenBIOS and why it looked like a firmware
bug. On 32-bit PowerPC `int` and `intptr_t` coincide, so this was invisible there.

*What broke the false diagnosis:* skipping `seek` entirely still crashed at the **same PC**. An
operation-specific bug cannot survive removing the operation. Once both `seek` and `read` failed
identically, the only shared input left was the handle.

*What made the false diagnosis so convincing:* the debug `printf` after `of_seek` never appeared,
so the trap looked like it happened inside `of_seek`. Console output can be lost when the machine
traps immediately afterwards — **never infer where a fault happened from the last line printed.**

**The FPU was never enabled.** With the handle fixed, the trap moved out of OpenBIOS and into our
own code at a `ld [%g1], %f0` — trap `0x20`, `fp_disabled`. SPARC V9 gates floating point behind
**PSTATE.PEF (bit 4)** and **FPRS.FEF (bit 2)**, and an FP instruction with either clear traps
(UltraSPARC-IIi User's Manual §A.4). Open Firmware does not set them for a client program.

The loader genuinely needs FP: the menu formats partition sizes with `%f`, and GCC also emits FP
register loads to move small structures — which is what actually faulted first. `_start` now
enables both bits before the constructors run. Note the VIS graphics instructions share the FP
register file and the same two enables.

### Where that leaves us: the boot menu renders

```
	no boot path found, scan for all partitions...
	/pci@1fe,0/pci@1,1/ebus@1/fdthree@0        (could open device, handle = 0xfef86860)
	/pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0      (could open device, handle = 0xfef86c10)
	/pci@1fe,0/pci@1,1/ide@3/ide@1/cdrom@0     (could open device, handle = 0xfef86fc0)
	check for partitioning_system: Intel Partition Map
	check for partitioning_system: Intel Extended Partition
	check for file_system: BFS Filesystem / FAT32 Filesystem / TAR Filesystem
	Could not locate any supported boot devices!

	Welcome to the Haiku Boot Loader
	Copyright 2004-2026 Haiku, Inc.
	  Select boot volume/state (Current: None)
	  Select safe mode options
	  Select debug options
	  Exit to OpenFirmware
```

Full device enumeration, partition scanning on every device, filesystem probes, and an
interactive menu. Handles now print as plausible values (`0xfef86860`) instead of a truncated
negative. "Could not locate any supported boot devices" is **correct**: the test disk carries
ext2, which Haiku's loader does not read. It needs a BFS volume with a kernel — that is the next
step, not a bug.

### Two emulator quirks that are genuinely OpenBIOS's fault

Both are about *empty* removable drives, and both are worked around by attaching media:

- **Opening an empty CD-ROM traps** `0x34` (`mem_address_not_aligned`) at PC `0xffd1c184` — the
  same PC as the earlier `dir` crash. Attach any ISO and it opens fine. QEMU's sun4u always
  instantiates a CD-ROM on the secondary IDE channel, so pass
  `-drive file=<any>.iso,format=raw,if=ide,index=2,media=cdrom`.
- **Scanning an empty floppy hangs** the partition scan indefinitely. Pass `-fda <any image>`.

With both attached, the loader runs to the menu. Also note the console emits a continuous stream
of `pc_serial_read: bad len, addr … len 3` once the menu starts polling for keys — harmless
QEMU noise, but it buries the log, so filter it with `grep -v pc_serial_read`.

---

## 7. Superseded: the "OpenBIOS seek is broken" diagnosis

Instrumenting `Handle::ReadAt` was decisive:

```
Handle::ReadAt handle=… pos=0 buffer=0x00000000ffe8df48 size=512
Unhandled Exception 0x0000000000000030   PC = 0x00000000ffd0f1f4
```

The follow-up `"seek ok, reading"` never printed, so **the trap happens inside `of_seek`**, with
the most benign arguments possible: position zero, on a freshly opened handle.

Things that were checked and ruled out:

- **Argument order is correct.** IEEE 1275-1994 §6.3.2.3 defines the client-interface service as
  `seek IN: ihandle, pos.hi, pos.lo`, which is exactly what `openfirmware.cpp` passes. (The
  `( pos.lo pos.hi -- status )` seen elsewhere in the spec is the *package method* signature, in
  Forth stack order — a genuinely easy way to talk yourself into "fixing" working code.)
- **No handle truncation.** `Handle::fHandle` and every `of_*` prototype use `intptr_t`, 64-bit
  here. The `-17275608` in the trace above was a `%d` in the debug printf, not a real value.
- **Not specific to the partition instance.** Opening the whole disk instead of `:a` traps at
  the identical PC.
- **OpenBIOS can read this disk perfectly well** — it just parsed the Sun label, walked ext2 and
  loaded a 455 KB ELF from it. It is the *client-interface* `seek` service that is broken, not
  the block layer beneath it.

The earlier floppy-probe crash was at `0xffd0f25c`, 0x68 bytes away — the same OpenBIOS routine.
This is consistent with the known QEMU bug *"OpenBIOS seek fails on NetBSD CD image"*
(launchpad #1169856).

**This was recorded as a firmware bug. It was not** — see §7a. Every bullet above is sound
evidence and the conclusion drawn from it was still wrong, which is the point of keeping this
section. The planned next step at the time was "build a newer OpenBIOS and see if it fixes seek";
that would have consumed an afternoon and fixed nothing.

The lesson worth carrying: *"the fault PC is in their code"* localises the **dereference**, not
the **cause**. A caller can hand a callee a bad pointer, and then the callee is where it dies.

---

## 8. What the sun4u machine looks like

From `show-devs`. A genuine Ultra 5/10 topology — sabre, then simba, then ebus — which is why
this emulator is the right development target.

```
/pci@1fe,0                                  sabre host bridge
/pci@1fe,0/pci@1,1                          simba PCI bridge
/pci@1fe,0/pci@1,1/ebus@1                   ebus
        ├── eeprom@0, power@0
        ├── fdthree@0 (block)               floppy
        ├── su@0 (serial)                   the console we use
        └── 8042@0/kb_ps2@0                 PS/2 keyboard
/pci@1fe,0/pci@1,1/network@1,1              sunhme
/pci@1fe,0/pci@1,1/QEMU,VGA@2 (display)
/pci@1fe,0/pci@1,1/ide@3                    cmd646 IDE
        ├── ide@0/disk@0 (block)
        └── ide@1/cdrom@0 (block)
/SUNW,UltraSPARC-IIi (cpu)

/packages/elf-loader   /packages/sun-parts   /packages/disk-label
/packages/deblocker    /packages/grubfs-files
```

Default `boot-device` is `disk:a`; `load-base` is `4000`; **`devalias` is empty**, so every
device must be named by full path.

---

## 9. Reference: the Tabby PowerPC port

Thread: <https://discuss.haiku-os.org/t/i-have-made-some-progress-on-the-powerpc-port/19578>
(113 posts, read in full). Different firmware and architecture, so not to be followed blindly,
but they have hit several of our problems first.

- They boot **`haikuloader.elf` by name** (`boot hd:,\haikuloader.elf`), which corroborated the
  ELF-not-a.out decision in §4.
- `leo75` describes the Pegasos II multi-architecture work as *"a few big-endian fixes (BFS, ATI
  driver)"* plus *"PS/2 support lacked PowerPC support which was added"*. **All three are things
  we will need**: BFS big-endian to read our own filesystem, the ATI driver for Mach64 on both
  target machines, and PS/2 for the Ultra 10's ebus keyboard. Find those patches before writing
  our own.
- `guidol` hit **`CLAIM failed`** and `LOAD-SIZE is too small` on real OpenBoot — Open Firmware
  memory-claim failures, exactly what our loader's `mmu.cpp` claim path does. Re-read when
  hardware arrives.

**Not yet done:** the thread's repository links live in the posts' HTML anchors, which the text
extraction dropped. Re-fetch preserving hrefs to locate `leo75`'s and `omgmonsters`' trees.

---

## 10. Tooling built

- **`tools/qemu-sun4u.sh`** — the harness. CPU selection (`iii` / `iie`), disk, cdrom, kernel,
  TFTP, gdb stub, timeout-safe logging.
- **`tools/make-sun-image.py`** — builds and inspects Sun-disklabelled disk images. The label is
  a fixed 512-byte big-endian VTOC with magic `0xDABE` at offset 508 and a checksum making the
  XOR of all 256 words zero. `--inspect` decodes and validates an existing image.
- **`tools/style-check.py`** — Haiku style checker, scoped to our delta. See §11.

### QEMU gotchas, learned the hard way

- `-nographic` serial produces **no output at all** if stdout is a pipe that closes early
  (`| head`, `| sed`). Redirect to a file, then grep. This wasted two runs.
- The sun4u machine already instantiates an onboard sunhme. Use `-nic user,model=sunhme`;
  `-device sunhme` fails with `PCI: no slot/function available`.
- The `ok` prompt is scriptable by feeding stdin on a delay — how every experiment above ran:
  ```sh
  ( sleep 9; printf 'show-devs\n'; sleep 6 ) | timeout 40 qemu-system-sparc64 ... > log 2>&1
  ```

---

## 11. The style checker, and why it is scoped the way it is

`python3 sparc-port/tools/style-check.py` — checks **our delta only**:

- a file we **created** is checked in full;
- a file we **modified** is checked only on **the lines we changed**;
- a file we have not touched is not checked at all;
- **cosmetic** rules apply only to files we created.

That last point was added after the checker flagged `void *virtualAddress` on a line in
`mmu.cpp` that we had touched for an unrelated reason. Acting on it would have meant restyling
upstream code — the exact churn §7 of the plan exists to avoid. Rules indicating a real defect
still apply to every line we touch; only the cosmetic ones (`pointer-style`, `cast-space`,
`func-blank-lines`, indentation) are limited to code we wrote.

Rules come from **Haiku's own `src/tools/checkstyle/checkstyle.py`**, including its real
**100-column** limit, plus a few from the published guidelines a script can judge without false
positives. Naming conventions, const correctness and "explain why not what" are deliberately
absent: a checker that cries wolf gets ignored, and then it is worse than nothing.

Ported BSD code is exempt via a `style-check: donor` comment marker, per
[THIRD_PARTY.md](THIRD_PARTY.md).

`--self-test` runs 25 cases against the checker itself, and caught two bugs in its own first
draft:

1. It **exempted itself** as donor code, because its own docstring contained the marker string.
   The marker is now a regex requiring a comment line of its own, built from concatenated parts
   so the source never contains the literal.
2. It applied Haiku's tab-indentation rule to **Python and shell**, where spaces are correct.
   Scope is now four-level (`text` / `code` / `native` / `source`).

Current state: **clean across 15 files.**

---

## 13. The kernel runs — Phase 1 complete

```
	Select boot volume/state (Current: Haiku (1024 GiB))
	load kernel kernel_sparc...
	video mode: 1024x768x8
	Unhandled Exception 0x0000000000000030
	PC = 0x00000000800f36c0
```

`0x800f36c0` is inside the kernel (`KERNEL_LOAD_BASE_64_BIT` is `0x80000000`), in
`create_debug_alloc_pool`, on a plain `ld [%g3], %g2`. Trap `0x30` is
`data_access_exception`. **This is the expected wall**: the kernel has no trap table and no
TLB-miss handler, so the first access outside Open Firmware's existing translations faults with
nothing to service it. Phase 2 is exactly the work of fixing this.

### What it took to get here

**The kernel would not compile.** `musl/arch/sparc/atomic_arch.h` did not exist, so anything
including `atomic.h` failed — `ffs`, `ffsl`, `ffsll`. Added, using SPARC V9's native
compare-and-swap:

- `cas` / `casx`: `casa [rs1] asi, rs2, rd` compares `rs2` against memory and exchanges `rd` on a
  match, or loads memory into `rd` on a mismatch — so `rd` always ends up with the previous value,
  which is exactly musl's `a_cas` contract. Verified against the SPARC V9 Architecture Manual A.9
  and cross-checked against OpenBSD's `sys/arch/sparc64/include/atomic.h`, which uses the
  identical constraint form.
- `a_barrier` uses `membar #StoreLoad` — under TSO that is the only ordering a barrier must add —
  placed in the delay slot of an always-taken branch to work around **erratum 51**, which the IIi
  manual's Appendix K explicitly lists as affecting *"US-I, II, and IIi"*: a membar issued late in
  the delay slot of a mispredicted control transfer can stop instruction issue entirely. Linux
  carries the same workaround as `membar_safe()`.

**Haiku's loader has no Sun disklabel support.** It only probes Intel partition maps, so a BFS
partition inside a Sun-labelled disk is invisible to it. Rather than write a partitioning-system
add-on now, the BFS volume goes on its own disk, which the device scan finds directly. Teaching
the loader about Sun labels is real future work — it is what an installed single-disk system will
eventually need.

**BFS endianness is fine, and that is worth knowing.** The host tool runs little-endian and
writes a little-endian volume; Haiku's BFS defaults to `BFS_LITTLE_ENDIAN_ONLY`, so the
big-endian SPARC loader byte-swaps on read and mounts it correctly. Every field the loader
validates was decoded and checked by hand before the first boot attempt. Note
`BFS_BIG_ENDIAN_ONLY` applies only to the separate `bfs_big` add-on, so loader and kernel agree.

**A bare kernel is enough.** `BootVolume::_SetTo` treats a missing `system/packages` as
"apparently not packaged" and returns `B_OK`, so `PackageVolumeInfo::SetTo(): failed to open
packages directory` is benign, not an error to chase.

**The boot menu is interactive and reachable over serial.** Volume selection needs three
keypresses: `\r` to open "Select boot volume/state", `\r` to take the listed volume, then
navigate to the boot entry. Cursor keys are `ESC [ A/B/C/D`; the OF console reads three bytes at
a time. QEMU grumbles `pc_serial_read: bad len ... len 3` continuously once the menu polls for
keys — noisy but harmless, and `grep -v pc_serial_read` is essential to read the log at all.

### Reproducing it

```sh
JAM=/home/kevin/Code/Haiku/SPARC/buildtools/jam/bin.linuxx86/jam
cd generated.sparc && $JAM -q -j24 haiku_loader.openfirmware kernel \
    '<build>bfs_shell' '<build>fs_shell_command'
cd ..
./sparc-port/tools/make-boot-disk.sh --output /tmp/loader.img   # loader on ext2 + Sun label
./sparc-port/tools/make-bfs-image.sh  --output /tmp/bfs.img     # BFS + system/kernel_sparc

qemu-system-sparc64 -M sun4u -cpu "TI UltraSparc IIi" -m 512 -nographic \
    -bios /usr/share/qemu/openbios-sparc64 \
    -drive file=/tmp/loader.img,format=raw,if=ide,index=0,media=disk \
    -drive file=/tmp/bfs.img,format=raw,if=ide,index=1,media=disk \
    -drive file=/tmp/any.iso,format=raw,if=ide,index=2,media=cdrom \
    -fda /tmp/any.fd
# ok prompt:  boot /pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\loader.elf
# then \r, \r, and navigate to boot
```

The cdrom and floppy media are the empty-drive workarounds from §7a, not optional.

---

## 14. Debugging the kernel: what works, and what does not

`tools/gdb-kernel.sh` boots the kernel with gdb attached and serial driven
automatically by `tools/serial-driver.py`. Four things about this were learned the hard way and
every one of them will waste an afternoon if forgotten.

### `set endian big` is mandatory

`set architecture sparc:v9` alone is not enough. Without the explicit endian setting **every
register reads byte-swapped**: the reset PC appears as `0x200000f0ff010000` instead of
`0x1fff0000020`, which is the sun4u reset vector. Reversing those bytes by hand is how the
problem was spotted, and nothing about the symptom says "endianness".

### `symbol-file`, never `add-symbol-file kernel 0x80000000`

The kernel is an ELF `DYN`, which invites the assumption that it needs relocating for gdb. It
does not: it is *linked* at `KERNEL_LOAD_BASE` and `nm` shows absolute `0x8000xxxx` addresses.
Passing `0x80000000` to `add-symbol-file` relocates on top of that, and gdb then reports **wrong
symbols silently** — `0x800f36c0` resolved to `inet_ntop` rather than `create_debug_alloc_pool`.
Cross-checking one address against `addr2line` is what caught it.

### QEMU halts on an unhandled trap but does not tell gdb

The console prints `Stopping execution`, the CPU stops, and gdb sits in `continue` forever. Hence
`--interrupt-on PATTERN`: a watcher greps the serial log and sends gdb `SIGINT` once the pattern
appears, which interrupts the target and, in batch mode, carries on with the remaining commands.

Note that by the time `Unhandled Exception` is printed the machine is already spinning in
OpenBIOS's error handler at `0x1fff000d914` (`b .` at the RED_state vector), so the faulting
context is gone from the CPU. `%g1` there holds the trap type, which is a small consolation.

### Breakpoints on kernel addresses do not work at all

Neither software (`break`) nor hardware (`hbreak`) breakpoints on kernel addresses ever fire.
gdb accepts both without complaint. The kernel is not in memory when gdb attaches at reset, so
software breakpoint insertion cannot write the trap instruction, and QEMU's sparc64 stub appears
to accept hardware breakpoint requests without implementing them. Setting them later loses a race
against the loader, which jumps into the kernel within milliseconds of printing `video mode:`.

**So do not plan Phase 2 around breakpoints.** Two things work instead:

1. **`-d int,mmu` tracing**, which is better than a breakpoint for this work anyway — see below.
2. **A deliberate spin loop** at the point of interest, then `--interrupt-on` and inspect. Ugly,
   deterministic, and reliable.

### `-d int,mmu` is the real Phase 2 instrument

`qemu-system-sparc64 -d int,mmu -D trace.log` logs every trap with the **complete** register
file, and that is exactly what trap-table and window work needs. The fault we are stuck on, in
full:

```
172300: Data Access Fault (v=0030)
pc: 00000000800f36c0  npc: 00000000800f36c4
%g0-3: 0000000000000000 00000000000003ff 0000000000000401 80217fc800000000
%g4-7: 80217fc7fffffff8 0000000000000000 0000000000000000 00000000802106d8
pstate: 00000014  asi: 00  tl: 0  pil: 0  gl: 2
tbr: 00000000ffd00000
cansave: 0 canrestore: 6 otherwin: 0 wstate: 0 cleanwin: 7 cwp: 1
fprs: 0000000000000005
```

Three things to read out of that:

- The faulting instruction is `ld [%g3], %g2`, and **`%g3` is `0x80217fc800000000`** — which is
  `0x80217fc8` shifted left by 32. `0x80217fc8` is a plausible kernel address, and `%g4` holds
  the same value minus 8. So a 64-bit address is being assembled or moved wrongly, not merely
  pointing somewhere unmapped.
- **`tbr` is still `0xffd00000`** — Open Firmware's trap table. The kernel has installed none,
  which is precisely the Phase 2 work; even a legitimate TLB miss here would have no handler.
- `fprs: 5` confirms the loader's FPU enable (§7a) survives into the kernel. `gl: 2` is worth a
  second look: the IIi has no GL register, so either QEMU is modelling it regardless or the
  kernel is running in a global-register set it did not intend, which would neatly explain a
  corrupt `%g3`.

### One real bug found and fixed along the way

`arch_elf.cpp`'s `R_SPARC_WDISP30` case had **no `break`** and fell through into
`R_SPARC_HI22`/`LM22`, OR-ing a second unrelated value into the same instruction word. The kernel
image carries **196 WDISP30 relocations**, so 196 call displacements were being corrupted. Fixed.
It did not change this particular fault, so it was not the cause here — but it was silently
corrupting call targets and would have caused something eventually.

### Leads for the fault, not yet followed

Both NetBSD and OpenBSD build their sparc64 kernels with **`-mno-fpu`**
(`conf/Makefile.sparc64`), and Haiku does not. Haiku does match NetBSD on `-mcmodel=medlow`.
Whether the kernel should use the FPU at all is worth deciding deliberately rather than by
default.

---

## 15. The `%g3` corruption: root-caused and fixed

### It was a relocation bug, and `gl: 2` was a red herring

**`gl: 2` is inert on this CPU and means nothing here.** UltraSPARC-IIi has no GL register — GL
arrives with UA2005 and the T-series. On the IIi the active global register set is selected by
**PSTATE.AG (bit 0), MG (bit 10) and IG (bit 11)** (UltraSPARC-IIi User's Manual, TABLE 14-12,
printed p.201, and the selection encoding in TABLE 14-13). Our trace shows `pstate: 00000014`,
which is PRIV (bit 2) and PEF (bit 4) only, so **AG, MG and IG are all clear and the normal
global set is active** — exactly right.

QEMU prints a `gl` field unconditionally because it models it for hyperprivileged CPUs. The
decisive argument that it is not banking anything: OpenBIOS runs correctly from power-on to the
boot menu with `gl: 2` displayed the whole time. If GL were switching register sets, the firmware
would break first.

### What it actually was

`%g3` came from `ldx [%g2], %g3` where `%g2 = %l7 + offset`, and `%l7` is the **GOT base** — the
kernel is built `-fPIE`. So the corrupt value was a **GOT entry**, not a corrupt register.

The GOT is populated by `R_SPARC_RELATIVE` relocations, and `arch_elf.cpp` had:

```c
case R_SPARC_RELATIVE:
	write_word32(P, B + A);   // Elf64_Word is uint32
```

`R_SPARC_RELATIVE` on ELF64 is a **64-bit** field. Writing 32 bits of it left the other half
untouched, and **because SPARC is big-endian the value landed in the high half** — so a slot that
should have held `0x80217fc8` held `0x80217fc800000000`, which is precisely the observed `%g3`.
The kernel carries **1153** of these relocations, so most of the GOT was wrong. Fixed to
`write_word64`.

The arithmetic matching exactly is what makes this conclusive rather than plausible: writing
`0x80217fc8` as 32 bits at a big-endian 64-bit slot gives bytes `80 21 7f c8 00 00 00 00`, which
reads back as `0x80217fc800000000`.

### What the fix bought

The kernel now runs a great deal of real code. From the trap trace:

| Trap | Count |
| --- | --- |
| Clean Windows | 51,938 |
| Window Spill | 9,959 |
| Window Fill | 9,956 |
| Data Access MMU Miss | 693 |
| Instruction Access MMU Miss | 24 |
| Instruction Access Error | 1 (fatal) |

Ten thousand spill and ten thousand fill traps, all serviced — by **Open Firmware's** trap table,
since `tbr` is still `0xffd00000`. That is a useful thing to know: OF's handlers are carrying the
kernel until we install our own, which is why the kernel gets this far with none of Phase 2 done.

Resolving the instruction-miss addresses gives the execution path:

```
_start → __sparc_get_pc_thunk.l7 → smp_set_num_cpus → cpu_preboot_init_percpu
       → arch_cpu_preboot_init_percpu → thread_preboot_init_percpu
       → panic → blue_screen_enter → kprintf → sort_debugger_commands → [wild jump]
```

**The kernel reaches early init and then panics**, and the panic path itself dies on a wild jump.
Two separate problems now, which is progress: something panics, and the debugger path cannot
report it. The latter is unsurprising — `arch_debug.cpp` is entirely stubs.

### Reading the panic message is blocked by a third bug

The panic text goes to `blue_screen_enter`, i.e. the framebuffer, where nothing can read it.
Serial output is selectable with `serial_debug_output` in
`home/config/settings/kernel/drivers/kernel`, which `make-bfs-image.sh --serial-debug` will write.

**That currently makes things worse, so it is off by default.** With the settings file present the
loader dies *before* the kernel, taking `mem_address_not_aligned` at `0x204870` — a `call %g1` in
`of_finddevice`, where `%g1` was loaded from a global. In other words **`gCallOpenFirmware` itself
is corrupt**, so merely reading driver settings damages loader state. Worth chasing: it is a
memory-corruption bug in code shared with PowerPC.

The alternative route to the panic message is the boot menu's *Debug Options → Enable serial debug
output*, which passes the flag through `kernel_args` and never touches the settings file. That
needs a few more keystrokes in `serial-driver.py` and avoids this bug entirely.

---

## 16. The panic, found and fixed — and the Phase 2 gate reached

### The panic was an assertion about interrupts

Reading it needed the spin-loop technique from §14, because breakpoints do not work here: `panic()`
was temporarily patched to spin, and gdb attached afterwards to read the call frame.

```
pc  = panic + 44                      (the spin)
i7  = 0x800afbc8                      (the caller)
o7  = panic + 16
```

Disassembling the call site gave the argument registers, and `%o2 = 0x4f7 = 1271` is a **line
number** — so this was an `ASSERT`, not a hand-written panic:

```c
smp.cpp:1271:   ASSERT(!are_interrupts_enabled());
```

**The kernel was being entered with interrupts still enabled.** Open Firmware runs with
`PSTATE.IE` set, `arch_start_kernel` did not clear it, and the kernel has no other opportunity to
before that assertion — which fires before it has any means of reporting the failure. Fixed by
clearing `PSTATE.IE` (bit 1, TABLE 14-12) in `arch_start_kernel.S` immediately before the jump.

Worth noting how indirect this was: the trap trace showed `pstate: 00000014` at the *fault*, with
IE clear, because by then `panic()` had already called `disable_interrupts()`. The offending
state was gone by the time anything reported it.

### Attaching gdb early corrupts the boot

A harness bug found on the way, now worked around with `--filler-iso` / `--filler-floppy`: if gdb
connects while Open Firmware is reading the kernel off disk, the read fails and the loader dies at
`0xffd1c184`. Attaching *after* the kernel is running is reliable. For the spin-loop technique
this costs nothing, since the machine waits forever.

### How far the kernel gets now

```
_start → smp_set_num_cpus → cpu_preboot_init_percpu → arch_cpu_preboot_init_percpu
       → thread_preboot_init_percpu → arch_platform_init → debug_init
       → debug_paranoia_init → frame_buffer_console_init → mutex_init
       → debug_output → interrupts_init
       → vm_init → vm_page_init_num_pages → slab_init → MemoryManager::Init
       → rw_lock_init → vm_allocate_early → [fault]
```

It initialises the platform, the debug output layer, the frame buffer console, locking, and the
interrupt layer, and then enters **`vm_init`**. It dies in `MemoryManager::_AllocateArea` on
`stx %g1, [%i5 + 0x20]` — a write to memory that `vm_allocate_early` has just handed back.

**That is the Phase 2 gate, and this is the expected failure.**
`arch_vm_translation_map.cpp`'s `early_map()` is a no-op and `create_map()` yields a null map, so
the address was allocated but never mapped. Trap totals for the run: 51,771 clean-window, 9,926
spill, 9,924 fill, 700 data-MMU-miss, 38 instruction-MMU-miss — all still serviced by Open
Firmware, `tbr` unchanged.

### Kernel debug output still is not on serial

`debug_output` and `vsnprintf` are in the trace, so the kernel *is* producing messages — they go
to `frame_buffer_console`, not the serial port, so we still cannot read them. Enabling
"serial debug output" through the boot menu (`--script boot-kernel-debug`) did not change this.
Worth resolving early in Phase 2: `arch_debug_serial_early_boot_message()` is an empty stub, and
that is the function specifically meant for fatal situations before the console is up.

---

## 17. Kernel debug output works — and Phase 2's job is now fully specified

### The fix

`SparcOpenFirmware::InitSerialDebug()` only fetched Open Firmware's `stdout` when the frame
buffer was **disabled**:

```c
if (!kernelArgs->frame_buffer.enabled) {
	if (of_getprop(gChosen, "stdout", &fOutput, sizeof(int)) == OF_FAILED)
		return B_ERROR;
}
```

The constructor leaves `fOutput` at `-1`, and `SerialDebugPutChar()` returns early on `-1`, so on
any machine where the loader set a video mode — which is every machine it can — **the kernel
discarded every byte of its own debug output, silently.** That is why the panic in §16 was
invisible.

The caution behind the guard was sound, though, and worth keeping: if OF's `stdout` *is* the
screen, writing through it while Haiku draws to the same frame buffer corrupts the display. So
`stdout` is now always fetched, and suppressed only when the device actually is one — decided by
asking it, via `of_instance_to_package()` and its `device_type` property, rather than inferred
from the frame buffer being present. A machine consoled over serial, which is how this port is
developed and how a headless Sun is normally run, keeps its output.

### What the kernel now tells us

```
Welcome to kernel debugger output!
Haiku revision: , debug level: 2
vm_translation_map_init: entry
physical memory ranges:
         0x0 - 0x20000000
allocated physical ranges:
         0x0 -   0xc2e000
  0x1fe80000 - 0x20000000
allocated virtual ranges:
         0x0 -   0xa0c000
  0xfef80000 - 0xff000000        <- Open Firmware
  0xffd00000 - 0xfff00000        <- Open Firmware
  0xfffce000 - 0xfffd2000
  0x80000000 - 0x80222000        <- the kernel image
early_tmap: entry pa 0xc2e000 va 0x81000000
early_tmap: entry pa 0xc30000 va 0x81002000
... about two thousand more ...
Unhandled Exception 0x30   PC = 0x80169cdc   (MemoryManager::_AllocateArea)
```

**2065 lines of kernel narration, and roughly two thousand of them are the same stub.**
`arch_vm_translation_map_early_map()` prints its arguments and returns `B_OK` without mapping
anything, so every one of those ~2000 pages is unmapped when the memory manager first writes to
one. `arch_vm_translation_map_create_map()` likewise returns `B_OK` while leaving `*_map` null.

### This specifies Phase 2 concretely

The kernel is now telling us exactly what it wants, which is a much better starting position than
the plan's abstract description:

- **~2000 early mappings**, 8 KB apart, physical `0xc2e000`+ to virtual `0x81000000`+ — a
  straightforward linear run, so `early_map` can be implemented and tested before anything else.
- **Ranges that must keep working**: Open Firmware's own mappings at `0xfef80000`–`0xff000000`
  and `0xffd00000`–`0xfff00000` are live and are currently servicing every trap we take. Whatever
  the TSB ends up holding, those must survive, or the machine loses its trap handlers mid-flight.
- **The kernel image** occupies `0x80000000`–`0x80222000`, comfortably inside a 4 MB page, which
  is an argument for mapping it with one large TTE as the plan's §4.3 suggests.
- Physical memory is a single range, `0x0`–`0x20000000` (512 MB as configured).

`early_map` is the right first target: it runs before any TSB exists, it has no locking or
teardown to get right, and there are two thousand calls to prove it against.

---

## 18. `early_map` implemented — and what it proves about Phase 2

### The approach

`arch_vm_translation_map_early_map()` now asks **Open Firmware** to create the mapping, via the
MMU node's `map` method, exactly as the boot loader does.

The reasoning is about who is servicing TLB misses at that moment. `%tba` is still Open
Firmware's — the kernel has installed no trap table — so writing a TTE straight into the TLB
would work only until that entry was evicted, at which point the miss would be handed to a
firmware handler that had never heard of it. Asking the firmware to make the mapping puts it in
the tables its own handler consults, so it survives eviction. That is also what makes these
mappings "early": they belong to the window before the kernel owns the MMU.

### It works, and moves the kernel forward

The fault moved from `MemoryManager::_AllocateArea` to **`vm_page_init`** — further into VM
initialisation, on memory that is now genuinely mapped.

### But it does not scale, and that is the useful finding

```
early_tmap calls:        2688
mappings that succeeded: ~1348
then: out of malloc memory (10010)!
      Unable to allocate memory for translations property!   (x1340)
```

**OpenBIOS rebuilds its `translations` property on every `map` call**, so the cost is quadratic
in the number of mappings and its heap runs dry after roughly 1300 — about half of what a boot
needs.

Two obvious workarounds were considered and both are unsafe:

- **Coalescing adjacent pages into one larger call.** `vm_allocate_early()` maps an entire
  allocation and only then returns the address, so a deferred flush would land *after* the memory
  had been used. Each allocation starts a fresh virtual run, so the flush could not be triggered
  by the next call either.
- **Mapping ahead.** That would create translations for virtual addresses the VM has not handed
  out, which it would later map elsewhere — a stale-mapping conflict rather than a saving.

Also worth knowing: OF's `map` **returns nothing**, so a firmware that declines a mapping does so
silently. The failures above are visible only because OpenBIOS prints to the console. Nothing in
the client interface reports them.

### What this settles about Phase 2

It removes an attractive-looking shortcut. Delegating the MMU to firmware indefinitely is not
viable — not because of anything SPARC-specific, but because the firmware's bookkeeping is not
built for thousands of mappings. **The kernel has to own a TSB.**

That reorders the phase slightly and for the better: the TSB comes first, `early_map` becomes a
few stores into it, and only then does the trap table need to be installed to service misses
against it. The present implementation stays as a stepping stone — it gets the kernel measurably
further than the no-op stub did, and real Open Firmware may have headroom OpenBIOS lacks, which
is worth checking on hardware.

---

## 19. Phase 2: the fast path, verified piece by piece

The strategy here was to prove every mechanical step from C, where a mistake costs a `printf`,
before writing any of it in assembly, where a mistake costs a silent hang with no stack and no
debugger. All four steps are now verified on the machine.

| Step | How it was proved |
| --- | --- |
| **TSB allocation** | 256 KB split pair at `0x80240000`, aligned to its own size |
| **Index arithmetic** | Programmed the TSB register, set Tag Access, read back `ASI_DMMU_TSB_8KB_PTR`, compared against our own index — **four probes, all match**, and all match values worked out by hand beforehand |
| **TTE construction and TLB load** | Mapped a fresh page at `0xa0000000`, wrote a pattern through it, read it back — **OK** — then demapped it |
| **Lookup algorithm** | `0x80000000 → pa 0xa0c000` and `0xffe00000 → pa 0x1ff00000`, both matching the firmware's own translations |

```
sparc_mmu: TSB at 0x80240000, 8192 entries per half, 256 KB total
sparc_mmu: warmed with 3963 firmware pages, 306 collisions (3657 distinct entries live)
sparc_mmu: firmware TSB registers: D 0x0000000000000000 (unused, so free to program)
sparc_mmu: index arithmetic agrees with the hardware
sparc_mmu: TLB load test: va 0xa0000000 -> pa 0xc70000, wrote 0x123456789abcdef read 0x123456789abcdef -- OK
sparc_mmu: lookup   0x80000000 -> hit  pa     0xa0c000
sparc_mmu: lookup   0xffe00000 -> hit  pa   0x1ff00000
```

### The handler

`arch_traps.S` now carries both MMU miss handlers, twelve instructions each, assembled and
disassembly-checked but **not installed** — `%tba` is untouched:

```
ldxa  [%g0] #ASI_DMMU_TSB_8KB_PTR, %g1    ! pointer the hardware formed
clr   %g2 ; ldxa [%g2] #ASI_DMMU, %g2     ! tag target
ldx   [%g1], %g4 ; ldx [%g1 + 8], %g5     ! the TSB line
xor   %g4, %g2, %g4                        ! compare
sllx  %g4, 1, %g4                          ! discard the Global bit
brnz,pn %g4, miss
stxa  %g5, [%g0] #ASI_DTLB_DATA_IN         ! atomic TLB write
membar #Sync
retry                                       ! re-run the faulting access
```

### The current fault is what the trap table will fix

The kernel still dies in `vm_page_init`, and the trap trace shows it accessing **`0x82000000`** —
an address `early_map` handed out. That is the OpenBIOS heap exhaustion from §18: the firmware
silently declined the mapping. **Our TSB has that entry**, because `early_map` records into it
regardless of what the firmware does. So the cutover is not merely the next step, it is the fix
for the fault we are looking at.

### Two bugs found while making the assembly build

**The miss branch became a branch to itself.** `FUNCTION()` makes a symbol global; a branch to a
global symbol goes through a relocation even within one section, because the linker must allow
for interposition; and the kernel is linked as a shared object, so `ld` left an
`R_SPARC_WDISP22` for load time with a displacement of zero. The miss path would have spun in
place with nothing to say why. Fixed by making the target local. **Worth remembering for every
handler added from here: keep internal branch targets local.**

**`arch_elf.cpp` handled neither `WDISP22` nor `WDISP19`**, so a kernel containing either would
have refused to load. Both are now implemented.

Also: the kernel-only parts of `arch_mmu.cpp` are now behind `!_BOOT_MODE`, because the boot
loader compiles that file too — for the TSB register readers — and is built with
`-Wstack-usage=1023`, which the 1.5 KB translation buffers failed outright.

### What remains before the cutover

1. **Window spill and fill handlers.** The big piece, and unavoidable: the traces show ~10,000
   spills and ~10,000 fills per boot, all currently serviced by the firmware. Port from OpenBSD's
   `locore.s`.
2. **The 32 KB table itself**, with entries for every trap type, both `TL = 0` and `TL > 0`
   halves.
3. **A slow path** for TSB misses, resolving from the authoritative translation map. The 306
   collisions above are exactly the cases that will need it.
4. **Lock the handler and the TSB in the TLB** before installing, per §2.6 — this is the
   requirement QEMU will forgive and hardware will not.
5. **The cutover**: `%tba` and the TSB register together. The firmware's locked entries survive,
   which keeps its own code mapped, so this can be staged rather than atomic.

---

## 20. Next steps

Phases 0 and 1 are done and the kernel is being entered. **Everything from here is Phase 2**, the
gate described in the plan's §4.1 and §4.2.

1. ~~Attach gdb~~ — **done, see §14.** `tools/gdb-kernel.sh` boots the kernel with gdb attached
   and serial automated. Read §14 before using it: breakpoints do not work, and `-d int,mmu`
   tracing is the instrument that does.
2. ~~Chase the `%g3` corruption~~ — **root-caused and fixed, see §15.** A 32-bit write to a 64-bit
   `R_SPARC_RELATIVE` GOT slot on a big-endian target. `gl: 2` was a red herring.
3. ~~Read the panic message~~ — **done, see §16.** It was `ASSERT(!are_interrupts_enabled())`;
   the kernel was entered with `PSTATE.IE` still set. Fixed, and the kernel now reaches `vm_init`.
4. ~~Get kernel debug output onto serial~~ — **done, see §17.** The kernel now narrates its own
   VM initialisation.
5. ~~Phase 2, starting with `early_map`~~ — **implemented via Open Firmware, see §18.** It works
   and moves the kernel to `vm_page_init`, but OpenBIOS's heap gives out after ~1300 mappings.
6. ~~Allocate and populate a kernel-owned TSB~~ — **done, and the whole fast path is verified
   step by step against the machine. See §19.**
7. **Window spill and fill handlers**, ported from OpenBSD's `locore.s`. The one genuinely large
   remaining piece, and unavoidable: ~10,000 spills and ~10,000 fills per boot, all currently
   serviced by the firmware.
8. **The 32 KB trap table**, both halves, and a slow path for TSB misses.
9. **Lock the handler and the TSB in the TLB**, then cut over `%tba` and the TSB register. §2.6
   of the design note is the requirement QEMU forgives and hardware does not.
6. **Chase the settings-file corruption** (§15): with a driver settings file present, the loader
   dies with `gCallOpenFirmware` corrupt. Shared with PowerPC, so likely upstreamable.
3. **Start the KDL backtrace early**, per the plan's Phase 5 note — a window-state bug corrupts
   silently and kills the machine with no diagnostic.
4. **Haiku's `arch_atomic.h` for sparc is empty stubs** — all three memory barriers are `// TODO`.
   The musl header added in §13 now has correct implementations to copy from, including the
   erratum 51 workaround.
5. **Find the Pegasos II big-endian patches** (BFS, ATI, PS/2) — §9.
6. **Later, not now:** a Sun disklabel partitioning add-on for the loader, so a single disk can
   hold both the loader and the system; and netboot, the fastest loop on real hardware.

### Open questions

- Does the kernel need its own FPU enable? The loader sets PSTATE.PEF and FPRS.FEF, but the trap
  table resets PSTATE on entry, so the kernel probably has to do it again.
- Does OpenBIOS sparc64 implement TFTP at all, or only the RARP half?
- Is the a.out wrapper meant to be written to a disklabel boot block — is `sparcbootblock.h` the
  intended first stage? Nothing in the tree exercises it.
- What exactly does the `mkfs.ext2 -r 0` requirement come from — a grubfs feature check, or
  something narrower?


## 21. The cutover — the kernel takes the MMU

The gate. From the store to `%tba` onwards every trap the machine takes goes to our handlers,
including the window spills and fills that ordinary C code generates by the thousand. It is not a
step that can be taken halfway.

### What had to be true first

**What the firmware locked, and what it did not.** `sparc_dump_tlb()` reads both TLBs through the
Tag Read and Data Access ASIs. Open Firmware locked only its own mappings — five D-entries and
two I-entries, its OBP, PCI/EBus and code regions, all 512 KB pages. Every kernel page was an
ordinary replaceable 8 KB entry, the trap table's included. And the D-TLB was already **full**:
64 valid, 0 free.

**Why the TSB had to be locked, and could not be locked cheaply.** The miss handler reads the TSB
with an atomic quad load, and TABLE 13-32 lists this CPU's atomic quad load ASIs in full: 0x24 and
0x2c, both virtual. The physical variants arrived with UltraSPARC III. Giving up atomicity instead
was not an option — a separate tag and data load can pick up a stale pairing, the one race the
quad load exists to close — so the read is virtual, can miss, and the TSB must be locked. Locking
256 KB as 8 KB pages would have taken 32 of the 64 D-TLB entries; it is locked as four 64 KB pages
instead. Full reasoning in [design note §4.4](PHASE2_MMU_DESIGN.md).

That needed physical memory both 64 KB aligned and contiguous, which `vm_allocate_early()` cannot
supply: it takes physical pages one at a time and its alignment argument constrains only the
virtual base. So the TSB takes its virtual range from that function with a `physicalSize` of zero,
which maps nothing, and `sparc_allocate_aligned_physical()` gathers the physical side — drawing
pages until one lands on the alignment, then **checking** each next page really is the last plus
one rather than assuming the early allocator stays contiguous. The locked entries are then the
TSB's only mapping, so the `memset` that zeroes it is the first write through them, done
deliberately while the firmware can still report a fault.

**Ten entries locked in total:** four for the TSB, four for the trap table, one for the handlers,
one for the trap data block. Handlers and table go in the I-TLB because a fetch that missed there
would need the very code that could not be fetched.

**That the tag comparison would match.** `sparc_tsb_insert()` stores `VA<63:22>` and nothing else,
because the hardware's tag target is `(context << 48) | VA<63:22>` and the context is zero
throughout the kernel. The fast path's single xor depends entirely on that. If it were wrong,
every access would miss and the slow path would stop the machine on the first one with nothing
said. Rather than read the context registers and reason about them,
`sparc_verify_tag_target()` writes an address into Tag Access and reads back the tag target the
hardware forms from it — the exact value a handler is handed. Both contexts read zero; all four
probes match.

### The cutover itself

TSB registers first, then `%tba`, interrupts off across both. The order is the whole of it: a trap
taken after `%tba` was set but before the TSB base was programmed would send the fast path to read
a TSB at address zero, find whatever was there, treat it as a translation, and load it into the
TLB. The reverse costs nothing, since nothing reads those registers until a handler does.

```
sparc_mmu: contexts: primary 0, secondary 0
sparc_mmu: tag target 0x80000000 -> hardware 0x000000000200, stored 0x000000000200 -- match
sparc_mmu: installing TSB register 0x0000000080241004 and %tba 0x801b8000
sparc_mmu: the kernel now services its own traps
```

That last line is itself the first evidence: it reached the console through C code, which means
window spills and fills, all handled by us.

### Then the firmware came out of the mapping path

With the kernel owning the MMU, `early_map` no longer needs to tell the firmware anything. That
turned out to be the next thing to break rather than a tidiness point: every `map` call extends the
firmware's `translations` property, and OpenBIOS's heap does not survive the thousands of pages
`vm_page_init()` maps. The first post-cutover boot died with **`out of malloc memory (10010)!`** —
an OpenBIOS message, not Haiku's — partway through that loop. Mappings made *before* the cutover
still go to the firmware, because until then the firmware is what services a miss.

### How far it gets now

Past `vm_page_init`, into `kernel malloc: using slab_heap`, through
`reserve_boot_loader_ranges()`, and into `vm_translation_map_init_post_area` — where it stops on
`ASSERT FAILED (vm.cpp:1929): wiring != B_ALREADY_WIRED`. That is not a trap and not an MMU
problem: `arch_vm_translation_map_create_map()` returns `B_OK` without ever setting `*_map`, so
the kernel address space has no translation map and the `B_ALREADY_WIRED` path's `map->Query()`
cannot work.

Worth noting from the same output: *"Current thread pointer is 0x000000008022e8d8, which is an
address we can't read from."* That value is the normal-bank `%g7` seen during the trap-globals
check, which confirms `%g7` is the thread pointer on this ABI — and that setting it in the MMU and
alternate banks left the normal one alone, as the check reported.

### The two deliberate tests

Getting this far only shows that most handler paths worked most of the time. A handler that is one
register short or one displacement off corrupts something far away, long after the evidence is
gone. So both criteria are provoked on purpose, where the answer is known in advance:

```
sparc_mmu: provoked TLB miss at 0x80280000: read 0xfeedfacecafebeef -- refilled
sparc_mmu: forced window overflow 24 deep: got 0x66408c6fe4, expected 0x66408c6fe4 -- spilled and filled
```

The miss test allocates a page, writes a pattern, demaps the translation but leaves it in the TSB,
and reads back — there is no route to a correct answer except the fast path. The window test
recurses 24 frames against 8 windows with a marker live across each recursive call, so it must
occupy a register the window owns and must therefore be spilled and filled; an exact sum also
confirms the stack bias.

### Still owed

- **A resolving slow path.** `sparc_tsb_miss_trap` records the miss into the trap data block and
  stops. It cannot resolve anything until there is an authoritative page table to resolve from,
  and collisions in a direct-mapped TSB mean this *will* be reached eventually — 288 were counted
  warming it from the firmware's translations alone.
- **A legible failure when it is reached.** The record is complete, but the machine stops with no
  output; reading it means attaching gdb and looking at the trap data block, whose address is
  printed at boot. A trampoline that returns to TL=0 and panics with the recorded address would be
  better, and the address for it can live in the still-unused MMU-global `%g3`.


## 22. The page table, and five bugs that were not the page table

Phase 2 left the TSB as the only translation structure, and it was never going to be enough on its
own: it is direct-mapped on VA<25:13>, so any two live regions more than 64 MB apart index to the
same lines and one of them silently loses. Warming it from the firmware's translations alone
produced 554 collisions. Something has to be able to answer for every address.

### What was built

**A three-level page table**, each level one page of 1024 eight-byte entries, ten bits of virtual
address each, covering VA<42:13> — which is the whole usable space, since sun4u implements a
44-bit address with a hole and VA<63:43> must be all ones or all zeroes. The geometry is
OpenBSD's, deliberately: its miss handler faces the same constraint, so matching the layout makes
the walk a known quantity.

Two properties make it work at all. Interior entries hold **physical** addresses, read through
`ASI_PHYS_USE_EC`, so no level needs a TLB entry and the walk cannot fault — which is what lets it
run from a trap handler on no stack, and lets the table itself live in ordinary unlocked memory.
Leaf entries are **TTE data halves**, so what comes out of the table is exactly what goes into the
TLB, and the slow path ends in the same three instructions as the fast path.

**`SPARCVMTranslationMap`**, wired into `arch_vm_translation_map_create_map()`, maintaining table,
then TSB, then TLB in that order. **The assembly slow path**, about twenty instructions. And
**failure reporting**: both the unresolved-miss path and the unhandled-trap handler now record
what happened, overwrite `%tnpc` and execute `done`, returning to TL=0 on the interrupted stack
where `panic()` can be called.

That last piece paid for itself immediately and repeatedly. Every previous version of these
handlers spun at a named symbol, which made a null function pointer indistinguishable from a hang.

### Five bugs, none of them in the page table

Each was silent, each surfaced far from its cause, and each is recorded because the *shape* of the
failure is the reusable part.

**The kernel address space was x86_64's.** `arch_kernel.h` had `KERNEL_BASE 0xffffff0000000000`
and a 512 GB size, copied verbatim, while the kernel is loaded at `0x80000000` and has never been
anywhere near that address. `IS_KERNEL_ADDRESS` therefore rejected every address the kernel
actually uses, `reserve_boot_loader_ranges()` skipped all of them including the kernel image's own
37 MB, and the first attempt to create an area for already-mapped memory failed with
`B_BAD_VALUE`. The correct bounds follow from `-mcmodel=medlow`, which requires the kernel inside
the low 32 bits: the upper 2 GB of the low 4 GB, the same shape as 32-bit x86.

**The loader allocated outside the kernel address space.** The block that based anonymous
allocations at `KERNEL_BASE` has been `#if 0`'d upstream, and the comment above it describes
exactly what that costs. Re-enabled — but not at `KERNEL_BASE` itself, since the heap is allocated
before the kernel is loaded and would take its address. The first attempt used a round 256 MB
above the kernel image and produced a *worse* failure than the original: 256 MB is a multiple of
the TSB's 64 MB indexing span, so those allocations aliased the kernel image line for line and
evicted it entirely as the TSB was warmed. Eight megabytes, inside the same window, keeps them
apart.

**The range arrays were never sorted.** The kernel's early allocators require ascending order and
do not check. Every other platform sorts before handing over; openfirmware was the exception, so
Open Firmware's own regions — recorded first, at the top of the address space — left the kernel
image's range last in the array, and `allocate_early_virtual()` took its "gap after the last
range" path and handed out addresses straight through the loader's heap.

**`PAGE_SHIFT` was 12 while `PAGESIZE` was 8192.** sparc64 was the only architecture where the two
disagreed, and the kernel uses them interchangeably. `vm_page.cpp` clears a freshly allocated page
with `vm_memset_physical(page->physical_page_number << PAGE_SHIFT, ...)`, so every clear landed at
half the page's real address, wiping 8 KB somewhere in the bottom half of physical memory — where
the boot loader's data lives. It surfaced as the kernel image structure reading back as zeroes.

Finding it is the part worth keeping: bracket the corruption with probes until the window is one
function wide, then guard the physical write paths and let one of them catch its own caller. The
giveaway was the address in that report, `0x9ff000` — not a multiple of 8192, so not a physical
page address that can exist.

**`preloaded_image` was 62 bytes.** It is packed so 32- and 64-bit builds agree, but the
sub-structures the derived images add are not, so the compiler emits full-width loads for
`Elf64_Ehdr`'s members. Six bytes off alignment made `verify_eheader()`'s read of `e_phoff` a
64-bit load from an address ending in 6 — absorbed by x86 and PowerPC, `mem_address_not_aligned`
on SPARC. Two bytes of padding.

### And one that was ours

**"Locked" does not mean what it sounds like.** The Lock bit exempts a TLB entry from the
*replacement* algorithm; it does nothing about an explicit demap. The trap table lives inside the
kernel image, so creating the areas for the preloaded image reached `sparc_tlb_demap()` with an
address in the middle of it and took the locked entry away. The next instruction fetch of the miss
handler then missed, and the handler needed to service that miss was the code that could not be
fetched.

QEMU reported it precisely: `Trap 0x0064 while trap level (5) >= MAXTL (5), Error state`, with the
trace showing every level vectoring to `0x801c4c80` — the table's own TL>0 entry for an
instruction miss — and immediately missing again. **This is the nested-trap death
[design note §2.8](PHASE2_MMU_DESIGN.md) describes, observed for the first time**, and it took
QEMU modelling MAXTL faithfully to be legible at all.

The fix records the ranges whose mappings are permanent as they are locked, and has both
`sparc_tlb_demap()` and `sparc_tsb_invalidate()` decline addresses inside them. Declining loses
nothing, because those mappings never change.

### A constraint worth remembering

**The kernel does not run global constructors.** There is a `.ctors` section with relocations
against it and nothing that walks it. For a plain struct that costs nothing, but a class with
virtual methods gets a null vtable pointer, and the first virtual call jumps to zero.

The physical page mapper was a static object, and `vm_page_allocate_page()` asking for a cleared
page reached `vm_memset_physical()`, which loaded the vtable, loaded the method at offset 0x40 and
called zero — reported as an illegal instruction at `pc 0x4`. This is why x86's paging code
constructs its mapper with placement new into a buffer, and why this one now does too.

### Still owed

- **Modified tracking.** Doing it properly means mapping writable pages read-only and catching the
  first write with a `fast_data_access_protection` handler, and there is no such handler yet. So W
  and MODIFIED are set up front: the VM sees every writable page as dirty, which costs writebacks
  that were not needed and never the reverse. `TTE_SOFT_REAL_WRITABLE` is already recorded, so the
  change is to stop setting two bits.
- **`GetPage()`** returns `B_NOT_SUPPORTED`. Handing back a usable virtual address for an arbitrary
  physical page needs either a physical map over all of RAM or the generic slot-pool mapper. An
  error rather than a plausible wrong address, which would corrupt memory quietly.
- **User page table teardown** panics rather than leaking, since nothing creates one yet.
- **Execute permission is recorded but not enforced.** sun4u has no per-page execute bit; a page is
  executable exactly when it has an I-TLB entry, so enforcement means deciding which TLB to refill
  from the trap type. The soft bit is there for that decision to consult.


## 23. The context switch

Twelve instructions, and short for a reason worth stating: on SPARC the registers save themselves.

### Why it is small

The ABI's callee-saved registers *are* the window registers, %l0-%l7 and %i0-%i7. So
`sparc_context_switch()` takes a window of its own, executes `flushw` to push every *other* window
out to the stack frame it belongs to, and is then the only live window in the machine. At that
point its `%i6` and `%i7` mean exactly "which stack to return onto" and "where to return to" —
overwrite them with the incoming thread's pair, and the ordinary function epilogue performs the
switch:

```
	save	%sp, -SPARC_MINIMUM_FRAME_SIZE, %sp
	flushw
	stx	%i6, [%i0 + CONTEXT_SP]
	stx	%i7, [%i0 + CONTEXT_PC]
	ldx	[%i1 + CONTEXT_SP], %i6
	ldx	[%i1 + CONTEXT_PC], %i7
	ret
	 restore
```

`ret` jumps to the new `%i7 + 8`; `restore` in its delay slot drops into a window that has to be
filled, and the fill reads from the new `%i6`. Nothing is copied anywhere: the registers are
spilled to one stack and filled from another by the same handlers that service every other window
trap. **This could not have been written before Phase 2 made those work**, and once they did, it
was nearly free.

**The floating-point registers need no saving.** Every `%f` register is caller-saved in the SPARC
V9 ABI, so the compiler has already spilled anything live across a call — and a voluntary switch
happens inside one. Preemption is a different case and arrives with the interrupt frame, which
saves what it interrupts.

### The fabricated first frame

A thread that has never run has no spilled window to fill from, so
`arch_thread_init_kthread_stack()` builds the frame a spill would have left. The entry function and
its argument go in the first two words — the positions a fill loads into `%l0` and `%l1`, which is
where `sparc_thread_entry()` looks for them, rather than being passed in the usual argument
registers.

`%i6` and `%i7` in that frame are both zero, deliberately: it terminates a stack walk at the bottom
of the thread instead of letting a backtrace wander into whatever the memory used to hold.

The **frame address** is what must be 16-byte aligned, not the stack pointer. SPARC V9 biases `%sp`
by 2047, so the two cannot both be aligned, and it is the frame the hardware cares about.

### Two things that went wrong

**The test was in the wrong place, twice over.** `arch_platform_init_post_thread()` looks like the
first point at which threads can be created — and it is — but nothing is *scheduled* before
`scheduler_start()`, which main.cpp says outright in the comment where it spawns `main2`. Threads
created there spawn successfully, resume successfully, and never run: the test reported `counter 0
of 128 after 2000001 yields`. It now runs from `arch_int_init_post_device_manager()`, inside
`main2`, which is the first thread the scheduler ever picks.

**The first version used `wait_for_thread()`** and tripped `could acquire exit_sem for thread 5` —
`acquire_sem_etc()` returning `B_OK` on a semaphore created with a count of zero. That is an
invariant in the semaphore or thread-death bookkeeping, not a context switch problem, and this
port had never exercised it. **It is left open rather than chased from here**, and the test polls
with `thread_yield()` instead, which depends on far less: no semaphores, no thread-death path, no
timer.

### The barriers

All three memory barriers were empty functions with a TODO. Architecturally that is nearly
harmless on one CPU under Total Store Order, which already orders load-load, load-store and
store-store — store-load is the only ordering a barrier must add.

What an empty body does not do is tell the *compiler*, and that is the half that mattered: a read
barrier emitting nothing lets GCC hoist a load out of a loop waiting on another thread's store.
Invisible until threads exist, which they now do. They emit a `"memory"` clobber, and the full
barrier a `membar #StoreLoad` in the delay slot of an always-taken branch — erratum 51 again.


## 24. The clock, and half of the timer

### system_time()

%TICK counts elapsed CPU clock cycles, and Open Firmware publishes the rate as `clock-frequency`
on the node whose `device_type` is `cpu` — found by walking the root's children, because the path
differs between an Ultra 10 and a Blade while the device type does not. QEMU reports 100 MHz.

Bit 63 is NPT, and masking it off is not optional: it is set out of reset, so the raw register
reads as a number that looks like a plausible timestamp right up until two of them are subtracted.

The conversion is two divisions rather than one, because the obvious form overflows — %TICK reaches
2^62 and multiplying by a million does not fit. Splitting into whole seconds and a remainder keeps
every intermediate in range and the result exact.

### Open Firmware's clock is the broken one, not ours

The obvious way to check a new clock is against an existing one, and Open Firmware has
`milliseconds`. The two disagreed by a factor of eleven, and from inside the kernel there is no way
to tell which is wrong.

The host settled it. With `serial-driver.py --timestamps`, twenty samples that each claimed to have
waited **101 firmware milliseconds** arrived **17 milliseconds apart** by the host's wall clock,
while `system_time()` reported **8.9 ms** for the same interval — which matches the host once the
serial output between samples is accounted for.

So `%TICK` at the advertised frequency is right and **OpenBIOS's `milliseconds` runs about eleven
times fast**. Worth knowing before anything in this port trusts `of_milliseconds()`, and worth
re-checking on hardware, where %TICK and `clock-frequency` are related by definition rather than by
an emulator's opinion.

The self-check that remains does what can be done from inside: an absolute value at boot that is
plausible rather than the thirty million years an unmasked NPT bit produces, monotonicity across
two hundred thousand samples, and elapsed time agreeing with the raw %TICK delta — computed by
different routes, so agreement means the frequency and the arithmetic match the counter.

### The timer interrupt

`%TICK_CMPR` is bit 63 INT_DIS and bits 62:0 a compare value, and a match posts TICK_INT in
SOFTINT<0>, which arrives as a **level-14** interrupt — trap type 0x4e. The manual is explicit that
the level-14 handler must check both SOFTINT<14> and TICK_INT, since the two share a level.

**The comparison is for equality, not for "greater than".** A comparator set to a value the counter
has already passed does not fire late; it fires in about three thousand years. So the timeout has a
floor, and after arming, the comparator is checked against the counter once more and the interrupt
posted by hand if the counter got there first.

Those registers are reached by **ASR number**: the assembler rejects `%tick_cmpr`, `%softint`,
`%set_softint` and `%clear_softint` with "architecture mismatch" while accepting the same registers
addressed numerically.

The entry path takes a window of its own, reads the trap registers before they go out of scope,
saves the interrupted globals, drops to trap level zero so that a window spill or TLB miss inside C
is handled by the ordinary table, and raises the level again on the way out to make `retry`
meaningful. The interrupted window needs no saving: a trap does not rotate CWP, so `save` makes it
the previous window and `restore` brings it back.

### Two bugs, both about state that is not where it looks

**Interrupt traps use the interrupt globals.** The entry cleared `PSTATE.AG` to get back to the
normal register bank, which is what most traps need. UltraSPARC-IIi adds interrupt and MMU global
sets, selected by `PSTATE.IG` at bit 11 and `MG` at bit 10 (TABLE 14-12, printed p.201) — and
interrupts use IG. So the handler ran C with the wrong bank, `%g7` was not the thread pointer, and
the first dereference of it faulted. All three bits are cleared now.

**`%pil` has to be raised, not merely `PSTATE.IE` relied upon.** The handler runs at trap level
zero, and an interrupt arriving before it returned wrote its own trap level over this one; the two
returns then drove the level below zero and QEMU stopped with **"Trap 0x0064 while trap level (-1)
>= MAXTL (5), Error state"**. Blocking at the interrupt level does not depend on anyone else's
discipline about the enable bit. OpenBSD does this and the first version here did not.

**And `PSTATE` and `%pil` are now part of the thread context.** Both are per-CPU registers that
behave as though they were per-thread: a thread switched out from inside an interrupt handler has
interrupts off and the level raised, and the thread switched in has its own idea of both.

That last change also surfaced the rule about new threads. They must start with interrupts
**disabled**, because every context switch in Haiku happens inside `scheduler_reschedule()` holding
the scheduler lock with interrupts off, and a thread scheduled for the first time arrives in
exactly that state — `common_thread_entry()` does the matching `release_spinlock()` and
`enable_interrupts()` itself. Starting one with interrupts on trips Haiku's own check immediately.

### What is not done: preemption

Rescheduling from the interrupt means switching stacks with a trap frame still on this one. With it
enabled, the kernel faults inside ordinary code — `BOpenHashTable::Insert` in the runs seen — with
`%i7` still pointing into this handler's caller, which means **the register window it is running in
is not the one it thinks**. The entry and exit paths are right for a handler that returns to what it
interrupted, and are not yet right for one that returns somewhere else entirely.

Without it the timer still runs: `system_time()` advances, `timer_interrupt()` fires, and Haiku's
timers and timeouts work. What does not happen is a thread being taken off the CPU against its
will, so scheduling stays cooperative.

The next step is a QEMU `-d int` trace of the failure, which was attempted and abandoned only
because tracing slows the machine enough that the boot does not reach the failure inside a
reasonable timeout. Narrowing the window first — forcing a reschedule from the very first tick,
rather than waiting for the scheduler to want one — would make that trace short enough to read.
