# Progress notes

Running log of what has been done, what was learned, and what to pick up next. Deliberately
separate from [PORTING_PLAN.md](PORTING_PLAN.md): the plan describes the shape of the work and
should stay stable; this file changes constantly. Findings get promoted into the plan only when
they change the plan.

**State: Phases 0 and 1 complete. The kernel is being entered — we are now at the Phase 2 gate.**

The loader boots from Sun-disklabelled media, mounts a BFS volume, loads `kernel_sparc`, sets a
video mode, and jumps into the kernel. The kernel then runs as far as `create_debug_alloc_pool`
and takes a `data_access_exception`, which is precisely what a kernel with no trap table and no
TLB-miss handling should do. See §13.

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

| # | Where | What | Committed? |
| :-: | --- | --- | --- |
| 1–4 | `openfirmware/{devices,network,video}.cpp`, `loader/menu.cpp` | Four `-Werror` failures that stopped the loader compiling at all | `b4845b6002` |
| 5 | `openfirmware/arch/sparc/mmu.cpp` | **PowerPC page-protection constants passed to a sun4u MMU** — see §3 | no |
| 6 | `openfirmware/devices.cpp` | Boot device opened by a path naming a *file*, not the device — see §6 | no |

Three of the first four are architecture-neutral, so the Open Firmware loader is bit-rotted for
PowerPC too. Haiku's own guide claims the loader runs. Discount similar claims about this port.

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

## 12. Next steps

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
5. **Phase 2, starting with `early_map`** (§17). It is the right first target: it runs before any
   TSB exists, has no locking or teardown, and the boot generates ~2000 calls to prove it
   against. Everything after it — the trap table, spill/fill, the TSB and the miss fast path —
   ships as one unit, ported closely from OpenBSD's `pmap.c` and `locore.s` rather than invented.
   Keep Open Firmware's mappings alive throughout: they are servicing every trap we currently take.
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
