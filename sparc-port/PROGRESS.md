# Progress notes

Running log of what has been done, what was learned, and what to pick up next. Deliberately
separate from [PORTING_PLAN.md](PORTING_PLAN.md): the plan describes the shape of the work and
should stay stable; this file changes constantly. Findings get promoted into the plan only when
they change the plan.

**State: Phase 0 complete. Phase 1 substantially working.** The Haiku bootloader now boots from
real Sun-disklabelled media, gets a proper boot path, and runs until it hits a bug *in OpenBIOS*
rather than in Haiku.

**Nothing is committed past `b9aa013d50`.** Everything below is in the working tree.

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

## 12. Next steps

Phase 1 is essentially done: the loader boots from media and reaches its menu. The critical path
now runs through getting a kernel loaded, which is what makes Phase 2 workable.

1. **Put a BFS volume with a kernel on the disk.** This is the single highest-value next task —
   it is what stands between us and the kernel being entered, and Phase 2 (the gate) cannot start
   until it is. Build `@minimum-raw`, or teach `make-sun-image.py` to place a BFS partition. Note
   Haiku's own `makebootable` has no SPARC support, so partition a's contents may need arranging
   by hand.
2. **Watch for the big-endian BFS problem.** Our reference notes record that big-endian Haiku has
   **no writable BFS**; reading may be fine but this needs confirming early, since the loader has
   to read a BFS volume to find the kernel.
3. **Attach gdb** — `--gdb` on the harness, then `gdb-multiarch`, `set architecture sparc:v9`,
   `target remote :1234`. The last Phase 0 item, and indispensable for Phase 2.
4. **Find the Pegasos II big-endian patches** (BFS, ATI, PS/2) — §9.
5. **Netboot**, as a second path, since it is the fastest iteration loop on real hardware.

### Open questions

- Does OpenBIOS sparc64 implement TFTP at all, or only the RARP half?
- Is the a.out wrapper meant to be written to a disklabel boot block — is `sparcbootblock.h` the
  intended first stage? Nothing in the tree exercises it.
- What reads the `-r 0` requirement — OpenBIOS's grubfs feature support, or something narrower?
- Does the kernel need its own FPU enable, or does it inherit PSTATE from the loader? The trap
  table will reset PSTATE on entry, so this probably needs doing again in the kernel.
