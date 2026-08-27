# Upstream delta

Changes this fork makes to **shared, cross-architecture Haiku files** — the ones upstream also
edits. This is the entire merge-conflict risk surface, so the goal is to keep this list short
enough to read in one sitting.

See [§7 of the porting plan](PORTING_PLAN.md#7-staying-in-sync-with-upstream).

## The two classes

**Class A — inside `*/arch/sparc/*` or `sparc-port/`.** Not tracked here. Upstream touches those
paths only during tree-wide sweeps, so conflicts are rare and shallow. Target: 95% of our
commits.

**Class B — everything else.** Tracked here, one row per change, each in its own labelled commit.

## Ledger

| Files | Commit | Why | Upstreamable? |
| --- | --- | --- | --- |
| `README.md` | `91c5724` | Repo landing page. A new path — Haiku's own file is `ReadMe.md` — so it can never conflict. | No, and never needs to. |
| `src/system/boot/platform/openfirmware/devices.cpp` | `b4845b60` | `printf("%ld", status)` where `status_t` is `int`. Harmless on the 32-bit PowerPC OF port; a hard `-Werror=format=` failure on 64-bit SPARC. | **Yes** — plain 64-bit portability bug. |
| `src/system/boot/platform/openfirmware/network.cpp` | `b4845b60` | `memcpy` into a `mac_addr_t`, which has a user-defined copy constructor and so is not trivially copyable — `-Werror=class-memaccess`. Now copies into the `address` array member. | **Yes** — and it is the Sun-specific `mac-address` path, so it matters to us directly. |
| `src/system/boot/platform/openfirmware/video.cpp` | `b4845b60` | `platform_init_video()` held an `edid1_info` on the stack *and* memcpy'd it to a `kernel_args` allocation, costing 1840 bytes against a 1023-byte budget. Now decodes straight into the allocation, dropping both the stack use and a redundant copy. | **Yes** — smaller and simpler on every platform. |
| `src/system/boot/loader/menu.cpp` | `b4845b60` | `alloca(128)` inside a conditional made the function's stack usage unbounded to the compiler. Replaced with a function-scoped 128-byte array. | **Yes** — but note this file is shared by *every* architecture, making it the highest-risk row here. |
| `.../openfirmware/arch/sparc/mmu.cpp` | `413bc053` | **PowerPC PTE protection bits passed to a sun4u MMU.** `PAGE_READ_WRITE` was `0x1`, which sets the TTE Global bit, not Writable — so every "writable" mapping came back read-only and the loader's first heap write took a `fast_data_access_protection` trap. Now passes `-1`, the firmware's default mode, as SunOS and NetBSD do. | **Yes** — a plain bug, and the file is SPARC-only, so this is nearly Class A. |
| `.../openfirmware/devices.cpp` | `413bc053` | `of_open(sBootPath)` opened the boot path *including its file component*, yielding an instance of a file inside a filesystem, which the partition scan then issued block reads against. Now strips the `,file` argument while keeping `:partition` — looking for the comma **after the last colon**, since OF device paths contain commas of their own (`pci@1fe,0`). | **Yes** — affects PowerPC identically; Tabby boots `hd:,\haikuloader.elf`, the same shape of path. |
| `.../openfirmware/console.cpp` | `3c864d9f` | **Console reads asked the firmware for three bytes at a time.** Open Firmware's `read` returns what it has, and OpenBIOS rejects a multi-byte request outright — logging `pc_serial_read: bad len` *to the console*, so every key poll produced a warning that then had to be read back as input. 570,000 of them and a 26 MB log per boot, plus unreliable input. Now reads one byte at a time, reassembling the `ESC [ X` cursor sequences across calls. | **Yes** — one-byte reads are what every implementation supports, and PowerPC has the same bug. |
| `.../openfirmware/devices.cpp` | `9e61df30` | **`int handle = of_open(...)` truncated a 64-bit Open Firmware handle.** `of_open` returns `intptr_t`; storing it in `int` dropped the high half, and the value then sign-extended back into a non-handle. The firmware dereferenced it and took a data access exception *inside itself*, which is why this presented as a broken `seek`. Harmless on 32-bit PowerPC where `int` and `intptr_t` coincide. | **Yes** — textbook 32-to-64-bit porting bug. |
| `.../openfirmware/arch/sparc/start.cpp` | `9e61df30` | **The FPU was never enabled.** SPARC V9 gates floating point behind PSTATE.PEF (bit 4) and FPRS.FEF (bit 2), and Open Firmware does not set them for a client program; any FP instruction traps `fp_disabled`. The loader does use floating point — the menu formats partition sizes with `%f`, and GCC emits FP register loads to move small structures. Enabled in `_start` before the constructors run. | Not directly — SPARC-only file, so effectively Class A. |
| `src/system/boot/platform/openfirmware/start.cpp` | `8cff17f` | **The kernel_args range arrays were never sorted.** The kernel's early allocators require ascending order and do not check -- `allocate_early_virtual()` looks for a gap between `range[i-1]` and `range[i]`, `vm_allocate_early_physical_page()` checks the next page does not run into `range[i+1]`. `insert_address_range()` appends a non-touching range wherever there is room, so the order is arbitrary. Every other platform sorts before handing over; openfirmware was the exception. Unsorted, the kernel handed out early virtual addresses straight through memory the loader had already allocated. | **Yes** — and PowerPC has been carrying it too. |
| `headers/private/kernel/boot/elf.h` | `769c8d3` | **`preloaded_image` was 62 bytes**, so every derived image's `elf_header` sat six bytes off alignment. `Elf64_Ehdr` is not packed, so the compiler emits full-width loads for its members, and reading `e_phoff` became a 64-bit load from an address ending in 6. Harmless on x86 and PowerPC; `mem_address_not_aligned` on SPARC. Two bytes of padding. | **Yes** — required by any strict-alignment target, and the change is deterministic for both widths. |
| `src/system/boot/loader/elf.cpp` | *pending* | **`kernel_args_malloc()` aligns to a byte unless told otherwise, and this file never told it.** The image structure and the symbol table are structures of 64-bit fields; all three allocations are also read into directly from disk, and Open Firmware's block read stores into the buffer with the width it finds convenient — OpenBIOS's IDE path uses halfword PIO, so an odd buffer faults inside the firmware. Invisible until a second image is loaded: the kernel is the first allocation out of a fresh page-aligned block and is aligned by accident. | **Yes** — required by any strict-alignment target, and free elsewhere. |
| `headers/.../openfirmware/platform_arch.h`, `.../arch/ppc/mmu.cpp`, `.../arch/sparc/mmu.cpp`, `.../openfirmware/video.cpp` | *pending* | **A display node's `address` property is a firmware virtual address, not a physical one**, and `platform_switch_to_logo()` was handing it to the kernel as `frame_buffer.physical_buffer.start` under a comment saying the memory is identity-mapped. True of Open Firmware on PowerPC Macs, not of sun4u's. New `arch_mmu_translate()`: the PowerPC one returns its argument and documents the assumption; the SPARC one looks the address up in the `translations` property the loader already parses. No translation means the frame buffer stays disabled rather than being handed over unusable. | **Yes** — the PowerPC path is unchanged in behaviour and the assumption stops being silent. |
| `src/system/boot/platform/openfirmware/Handle.cpp` | *pending* | Comment only. Records that `of_read()` stores into the caller's buffer with the firmware's choice of access width, so an unaligned buffer faults inside the firmware three frames from the caller that supplied it. | **Yes** — it is the explanation for the `elf.cpp` row above. |
| `src/add-ons/kernel/bus_managers/pci/pci_io.cpp` | *pending* | **PCI I/O space is little endian and the non-x86 accessors did not say so.** These reach the ports through a mapping of the host bridge's I/O window rather than through an instruction that knows what bus it is talking to, so a multi-byte load returns host-order bytes and the value is byte-reversed. Compiles away on a little-endian host, which is every platform that took this path before SPARC. | **Yes** — it is what the specification already says about the bus. |
| `src/add-ons/kernel/bus_managers/pci/pci.cpp` | *pending* | `InitDomainData()` mapped the I/O port window and, on failure, set the base to NULL and said nothing. `pci_read_io_8()` then adds the port number to NULL and reads low memory, so every I/O-port driver gets plausible rubbish instead of an error — which is exactly how this presented. Now reports both outcomes. | **Yes** — a silent failure with an unrecognisable consequence. |
| `src/add-ons/kernel/generic/ata_adapter/ata_adapter.cpp` | *pending* | Two changes, both byte order. **PIO transfers carry a byte stream, not numbers:** with the accessors above converting, the data path has to convert back, or every pair of bytes on the disk is swapped. **The PRD table's own address was converted twice:** `write_io_32()` puts a host integer on a little-endian bus and converts on the way, so the explicit `B_HOST_TO_LENDIAN_INT32()` around the table address swapped it back — the controller was handed a base pointing nowhere and walked whatever it found there as descriptors. That is the *actual* reason DMA once corrupted the kernel: not an unprogrammed IOMMU, but a bus master writing data to addresses read out of garbage PRD entries. | **Yes**, both. Each compiles away on a little-endian host, which is every platform that had exercised this driver before SPARC. |
| `src/add-ons/kernel/bus_managers/ata/{ATAHelper.cpp,ATADevice.cpp,ATAPrivate.h}` | *pending* | **The identify block is a list of little-endian numbers** and was read without conversion, so on a big-endian host the capability bits land in the wrong half of each word and an ordinary disk reports no LBA support. New `ata_info_block_to_host()` converts each word and then the three fields wider than one. The strings are deliberately untouched: ATA puts a string's first character in the *high* byte of its word, so the per-word swap leaves them in reading order, which is the state `swap_words()` already expects on a big-endian host. | **Yes** — no-op on little-endian, and the driver is otherwise unusable big-endian. |
| `src/system/kernel/arch/generic/generic_vm_physical_page_mapper.cpp` | *pending* | `%ld` and `%03lx` against an `int32`, which is `int` on LP64 -- a `-Werror=format=` failure. Confined to the `iospace` debugger command, which is why it has evidently never been compiled on a 64-bit target. | **Yes** -- plain 64-bit portability bug, same class as the loader rows above. |
| `build/jam/ArchitectureRules` | *pending* | `-z max-page-size=0x2000` for the SPARC kernel and its add-ons. The linker aligns segments to 1 MB on sparc64, so an add-on's data segment starts a megabyte after its text ends, and `load_kernel_add_on()` refuses an image whose segments are more than 8 KB apart -- it reserves the whole span. Every add-on failed to load from disk with `B_BAD_DATA` while the same file worked preloaded, because the boot loader maps each segment on its own. | Not really -- it is one arm of an existing `switch` on the CPU, alongside identical lines for arm, arm64 and m68k. Class B only because the file is shared. |
| `src/libs/compat/freebsd_network/compat/machine/{bus.h,cpufunc.h}` | *pending* | Two `#error Need a … for this arch!` gates that had no sparc branch. Added, pointing at the existing `machine/generic/` implementations RISC-V already uses. Two lines each. | **Yes** — additive, and the same shape as the RISC-V branch beside it. |
| `src/libs/compat/freebsd_network/compat/machine/generic/bus.h` | *pending* | **`bus_space_read_*` did not convert byte order.** Correct on RISC-V and wrong on any big-endian host: PCI is little-endian by specification, so a 32-bit register read on SPARC returns the bytes reversed. FreeBSD's own contract is that `bus_space_read_N` yields the *value* and `bus_space_read_stream_N` yields the *bytes* — and the stream family here was `#define`d to the non-stream one, which is exact only where the distinction is empty. The scalars now convert and the stream variants are spelled out. Also adds `bus_space_subregion()`, which `machine/x86/bus.h` has and this file did not. | **Yes** — it makes the file correct on every endianness rather than on one, and no little-endian platform changes at all. |
| `src/libs/compat/freebsd_network/bus.cpp` | *pending* | **A BAR's host address was taken from `pci_info::base_registers[]`, which is a `uint32`.** On a machine where the host bridge puts PCI memory above 4 GB the correct address does not fit: sabre places it at host physical `0x1ff00000000`, so a BAR at PCI `0x21000000` is at `0x1ff21000000` and the field holds the low half — which is the PCI address again and looks entirely reasonable. What follows is a mapping of whatever is at that physical address, and a bus error on the first register access a long way from here. Now translated at the point of use, from `base_registers_pci[]` through the bus manager's own `ram_address()` hook, which is identity on x86. | **Yes** — a latent 64-bit correctness bug that no current platform trips, fixed without touching a public structure. |
| `src/libs/compat/freebsd_network/pci.cpp` | *pending* | `pci_get_intpin()` and `pci_set_intpin()` were declared in `pcivar.h` and never defined, so any driver calling them failed to link. Two lines each. | **Yes.** |
| `src/libs/compat/freebsd_network/compat/sys/param.h` | *pending* | `ulmin()`/`ulmax()`, which FreeBSD has and this layer did not. | **Yes.** |
| `src/add-ons/kernel/drivers/network/ether/Jamfile` | *pending* | One `HaikuSubInclude hme ;`. | **Yes**, with the driver. |
| `src/system/libroot/posix/glibc/include/bits/hwcap.h` | *pending* | New file. glibc's SPARC `sysdep.h` includes `<bits/hwcap.h>` unconditionally, because upstream dispatches between implementations at load time on AT_HWCAP; Haiku's import carries neither ifunc resolvers nor a populated auxv, and nothing in the tree references an `HWCAP_SPARC_*` constant. The include had nothing to find, so the seven multi-precision assembly routines the string and stdio code is built on failed to compile -- which presented as `posix_string.o` rather than as anything to do with capabilities. Deliberately defines no constants: providing glibc's list would let a future resolver test a bit against a value this platform never computes. | **Yes** — a platform header for a platform that has no hardware capability mechanism. |
| `src/system/libroot/posix/malloc/openbsd/wrapper.c` | *pending* | `_MAX_PAGE_SHIFT` was defined only `#if B_PAGE_SIZE == 4096`, with no `#else`. Not a missing case so much as a silent one: the identifier simply does not exist, and the build fails in `malloc.c` on a line mentioning neither page sizes nor this file. SPARC's page is 8192. Now enumerated exhaustively with an `#error` for the next one. | **Yes** — the same shape of bug as `PAGE_SHIFT` in the kernel, which had been corrupting physical memory. |
| `src/system/libroot/posix/musl/math/sparc/Jamfile` | *pending* | Missing `__fpclassify.c __fpclassifyf.c __fpclassifyl.c`, which every other architecture's list carries. Diffed against riscv64's list; those three were the only difference. | **Yes.** |
| `headers/libs/zydis/Zycore/Defines.h`, `src/libs/zydis/{Zycore/Format.c,Zydis/String.c}` | *pending* | **Zydis had no SPARC arm**, so its architecture switch fell through to `#error "Unsupported architecture detected"` and took 24 targets of a minimum image with it — the debugger kit and everything downstream. Added `ZYAN_SPARC64`/`ZYAN_SPARC` in the ppc64/ppc shape beside the existing `ZYAN_RISCV64`, and listed the 64-bit one in the two places the define is actually consumed. Those two are the whole surface: both are `#if <is 64-bit>` choosing a direct 64-bit path over a 32-bit-friendly fallback that computes the same answer more slowly. | **Yes**, and it is upstream Zydis's to take rather than Haiku's — the same one-line-per-architecture addition riscv64 and loongarch already have. |
| `src/system/libnetwork/netresolv/net/{getnetent.c,getnetnamadr.c}` | *pending* | **`libnetwork` did not compile for this architecture.** Three sites set `net.__n_pad0`, guarded by a condition that is true on 64-bit SPARC, alpha and LP64 i386/sh — so 64-bit SPARC is the only Haiku architecture it fires on, which is why nobody had hit it. `__n_pad0` is a NetBSD ABI artifact: their `struct netent` carries explicit padding on those platforms, Haiku's does not have the member, and does not need it — two pointers, an `int` and an `in_addr_t` pack without a hole on LP64. The condition now excludes `__HAIKU__`, which is how the rest of this vendored tree marks the same kind of divergence. | **Yes** — it is a NetBSD-only field being assigned in code shared with a platform whose structure does not have it. |
| `src/system/runtime_loader/elf_load_image.cpp` | `f2d8b360` | **A `DT_INIT` of zero was being called.** The load address is added to whatever the tag holds and the caller tests the result against zero to decide whether to run it — so an image declaring zero gets called at its own first byte, which is the ELF header. Now treated as "there is no initialiser", which is what it means: linkers omit the tag rather than write zero, so an image declaring zero is one where something went wrong at build time. Ours did — see the `crti.S` fix in the same commit — but the binaries already shipped in `gcc_syslibs` cannot be relinked from here. | **Yes** — architecture-neutral, and the alternative behaviour is a jump into a mapping's first byte on any platform. |

### Why the loader-build fixes exist

The Open Firmware bootloader **does not build from upstream master** — not for SPARC, and by
inspection not for PowerPC either. Haiku's own guide says the loader works, which was evidently
true at some earlier point. Three of those four failures are architecture-neutral and would fail
any modern-GCC build of this loader; only the `%ld` one is genuinely 64-bit-specific.

`-Wstack-usage=1023` is applied to the `openfirmware` boot target and no other
(`build/jam/ArchitectureRules:554`). That is deliberate — the loader runs on the firmware's own
small stack — so the right response was to fix the stack consumers rather than to raise the
limit for SPARC, even though SPARC's mandatory 176-byte register-window save area does make
every frame inherently larger.

**Follow-up:** `menu.cpp` is cross-architecture and has not yet been compile-tested on x86_64.
Do that before the first upstream submission. The same applies to the
`headers/private/kernel/boot/elf.h` padding, which changes a structure every architecture uses --
deterministically, and loader and kernel are always built together, but it wants a build of each
before it is offered upstream.

## Decisions that keep this list short

**Ticket #19597 — deliberately not implemented.** Implementing separate kernel/user address
spaces would mean changing `IS_USER_ADDRESS` and `user_memcpy` for every architecture: the
single largest Class B change this port could possibly make. The TTE Global bit
(UltraSPARC-IIi manual, FIGURE 15-1, printed p.205) lets us use a shared address space with
kernel mappings marked Global and user mappings context-tagged, which requires **no shared-code
changes at all**. Revisit only if profiling ever justifies it. See
[§4.3](PORTING_PLAN.md#43-kernel-and-user-address-spaces--ticket-19597).

**Documentation lives in `sparc-port/`, not `docs/`.** Haiku has its own `docs/` tree; sharing
directories would manufacture merge noise for no benefit.

## Checking the delta at any time

```sh
# our entire delta
git diff --name-only master...sparc/main

# Class B only: exclude our own directory, our README, and every arch/sparc path
git diff --name-only master...sparc/main -- . \
    ':!sparc-port' ':!README.md' ':!*/arch/sparc/*' ':!*/arch/sparc64/*'
```

The second command should print only files that have a row in the table above. Anything else is
either a row that needs adding or a change that wants reconsidering.

Note that the excluded `arch/sparc` paths are where most of the work lives, and deliberately so —
see the Class A note above. They are not untracked out of laziness; they are the changes that
cannot conflict with upstream, which is the whole point of keeping the diff shaped this way.

## Before merging upstream

```sh
git fetch upstream master
git diff --name-only master...sparc/main            > /tmp/ours
git diff --name-only master..upstream/master        > /tmp/theirs
comm -12 <(sort /tmp/ours) <(sort /tmp/theirs)      # the intersection is the risk
```

An empty intersection means the merge is mechanical. Anything listed wants a human before
merging — and `rerere` is enabled, so any conflict resolved once is replayed automatically
afterwards.
