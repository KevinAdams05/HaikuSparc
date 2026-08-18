# Phase 2: the TSB, the trap table, and the TLB-miss fast path

Design notes for the MMU gate, written from the UltraSPARC-IIi User's Manual chapter 15 and
verified against QEMU's implementation before any code was committed to a shape.

**Companion documents:** [Porting plan](PORTING_PLAN.md) · [Progress log](PROGRESS.md)

---

## 1. Why this document exists

[§18 of the progress log](PROGRESS.md) established that the kernel cannot keep delegating
mappings to Open Firmware: OpenBIOS exhausts its heap after roughly 1300 of the ~2700 mappings a
boot needs. The kernel has to own a TSB.

Before writing trap-handler assembly it is worth being certain of two things: what the hardware
actually does, and whether the emulator we develop against does the same. Getting that wrong
means writing correct-looking code that behaves differently on the Blade 150 — the most expensive
class of mistake available on this project, because it surfaces months later on hardware that
offers no debugger.

Both are now settled. Sources are cited so the claims can be rechecked.

---

## 2. What the hardware does

### 2.1 There is no table walker

> **Compatibility Note** — The UltraSPARC-IIi MMU performs no hardware table walking. The MMU
> hardware never directly reads or writes to the TSB.
> — IIi manual §15.3.2, printed p.211

That is the whole problem in one sentence, from the vendor. The TSB is a software structure; the
hardware's only contribution is to make the software's job fast.

### 2.2 The four services the hardware does provide

From §15.3.1, printed p.209:

- **Formation of TSB pointers** from the missing virtual address
- **Formation of the TTE tag target** for the tag comparison
- **Atomic write of a TLB entry** with a single store ASI operation
- **Alternate globals** on MMU-signalled traps

### 2.3 The canonical miss and refill sequence

Quoted from §15.3.1:

1. A TLB miss causes either an `instruction_access_MMU_miss` or a `data_access_MMU_miss`
   exception.
2. The appropriate TLB miss handler loads the TSB pointers and the TTE tag target with loads from
   the MMU alternate space.
3. Using this information, the handler checks whether the desired TTE exists in the TSB. If so,
   the TTE data is loaded into the TLB Data In register to initiate an atomic write of the TLB
   entry chosen by the replacement algorithm.
4. If the TTE does not exist in the TSB, the handler jumps to a more sophisticated (and slower)
   TSB miss handler.

The virtual address feeding the pointer formation comes from the **Tag Access register**, which
holds the VA and context of the access that faulted.

### 2.4 Pointer formation

`N` is the `TSB_Size` field of the TSB register, 0 through 7.

Shared TSB (`Split = 0`):

```
8K_POINTER  = TSB_Base<63:13+N> || VA<21+N:13> || 0000
64K_POINTER = TSB_Base<63:13+N> || VA<24+N:16> || 0000
```

Split TSB (`Split = 1`):

```
8K_POINTER  = TSB_Base<63:14+N> || 0 || VA<21+N:13> || 0000
64K_POINTER = TSB_Base<63:14+N> || 1 || VA<24+N:16> || 0000
```

The tag target is the faulting VA and context realigned to the positions they occupy in the TTE
tag, **so that hit detection is a single XOR**.

### 2.5 Alternate globals: which traps, and which set

§15.3.2 is precise, and the distinction matters because it decides what a handler may touch:

| Traps | Register set |
| --- | --- |
| `fast_instruction_access_MMU_miss`, `fast_data_access_MMU_miss`, `instruction_access_exception`, `data_access_exception`, `fast_data_access_protection` | **MMU globals** |
| `privileged_action`, `*mem_address_not_aligned` | normal alternate globals |
| everything else | normal alternate globals |

This is what lets the miss handler run **with no stack and no register saving**: the hardware
hands it a private set of `%g` registers on the way in.

### 2.6 What must be locked, and where

Two separate lists, and conflating them is a good way to build a machine that dies unpredictably
(§15.3.1, printed p.210):

- **Locked in the TLB**: the TLB-miss handler, the TSB and its linked data, asynchronous trap
  handlers and their data.
- **Locked in the TSB** (not necessarily the TLB): the TSB-miss handler and data, the
  interrupt-vector handler and data.

The `L` bit in the TTE marks an entry as exempt from the automatic replacement algorithm. Note
the manual's caveat: *"Software must ensure that at least one entry is not locked when replacing
a TLB entry, otherwise the last TLB entry will be replaced."*

### 2.7 Trap table geometry

From the UA2005 spec, `FIGURE 12-4`, and confirmed by QEMU's addressing below:

- The table is **32 KB**: 512 entries of 32 bytes for `TL = 0`, then 512 more for `TL > 0`.
- Entry address is `TBA | (TL > 0 ? 0x4000 : 0) | (trap_type << 5)`.
- `%tba` must therefore be **32 KB aligned**.
- Spill and fill traps occupy `0x080`–`0x0FF` in each half — 128 entries, because the type
  encodes the window set and whether it is a spill or a fill.

### 2.8 The nested-trap death, precisely

From the IIi manual §14.1.3, printed p.224:

> UltraSPARC-IIi supports five trap levels; that is, MAXTL=5. Traps at MAXTL−1 cause the CPU to
> enter RED_state. If a trap is generated while the CPU is operating at TL = MAXTL, the CPU will
> enter error_state and generate a Watchdog Reset.

So the budget is four usable nested traps. A TLB miss that spills a window whose stack write
misses the TLB is three levels deep before anything unusual has happened.

---

## 3. Does QEMU implement all of this? Yes — verified against source

Checked against `qemu/target/sparc` at master. This matters because the entire fast-path design
leans on hardware behaviour that an emulator could plausibly stub.

### 3.1 Alternate globals — implemented, and on exactly the right traps

`int64_helper.c`, `sparc_cpu_do_interrupt`:

```c
case TT_TFAULT:
case TT_DFAULT:
case TT_TMISS ... TT_TMISS + 3:
case TT_DMISS ... TT_DMISS + 3:
case TT_DPROT ... TT_DPROT + 3:
    ...
    cpu_change_pstate(env, PS_PEF | PS_PRIV | PS_MG);
    break;
default:
    cpu_change_pstate(env, PS_PEF | PS_PRIV | PS_AG);
    break;
```

That trap list is precisely §15.3.2's: the fast MMU misses, the access exceptions and the
protection trap get **MMU globals**; everything else gets the normal alternate globals.
`win_helper.c` backs `PS_MG` with a real `env->mgregs` bank, and disables `AG`/`IG`/`MG` only for
CPUs with the `GL` feature — which the IIi is not.

### 3.2 Hardware pointer formation — implemented, and the formula is exact

`ldst_helper.c`, `ultrasparc_tsb_pointer()`:

```c
uint64_t tsb_base_mask = (~0x1fffULL) << tsb_size;
uint64_t va = mmu->tag_access >> (3 * page_size + 9);
if (tsb_split) {
    if (idx == 0) va &= ~(1ULL << (13 + tsb_size));
    else          va |=  (1ULL << (13 + tsb_size));
    tsb_base_mask <<= 1;
}
return ((tsb_register & tsb_base_mask) | (va & ~tsb_base_mask)) & ~0xfULL;
```

Worked through by hand against §2.4:

- `(~0x1fff) << N` keeps bits `63:13+N` — the manual's `TSB_Base<63:13+N>`. ✅
- For 8 KB (`page_size = 0`), `tag_access >> 9` puts VA bit 13 at bit 4 and VA bit `21+N` at bit
  `12+N`; masking to `~tsb_base_mask` and clearing the low four bits yields
  `VA<21+N:13> || 0000`. ✅
- For 64 KB (`page_size = 1`), `>> 12` puts VA bit 16 at bit 4 and VA bit `24+N` at `12+N`,
  giving `VA<24+N:16> || 0000`. ✅
- Split shifts the base mask one further (`63:14+N`) and forces bit `13+N` to 0 for the 8 KB
  pointer and 1 for the 64 KB one — the manual's `|| 0 ||` and `|| 1 ||`. ✅

`ultrasparc_tag_target()` is `((tag_access & 0x1fff) << 48) | (tag_access >> 22)`, placing the
context at bits 60:48 and `VA<63:22>` at bits 41:0 — exactly the TTE tag layout in `FIGURE 15-1`.
✅ The single-XOR hit test will work.

### 3.3 Atomic TLB write and demap — implemented, lock bit honoured

Stores to `ASI_ITLB_DATA_IN` (0x54) and `ASI_DTLB_DATA_IN` (0x5C) go to `replace_tlb_1bit_lru()`,
which skips entries where `TTE_IS_LOCKED()`, prefers invalid then unused entries, and resets the
used bits before a second pass. That is the manual's described replacement algorithm including
the `L` bit semantics.

`ASI_IMMU_DEMAP` (0x57) and `ASI_DMMU_DEMAP` (0x5F) go to `demap_tlb()`, which walks all 64
entries and invalidates matches **regardless of the lock bit** — correct, since demap is an
explicit software action and the lock only guards automatic replacement.

### 3.4 Trap table addressing and MAXTL — implemented

```c
env->pc = env->tbr & ~0x7fffULL;
env->pc |= ((env->tl > 1) ? 1 << 14 : 0) | (intno << 5);
```

32 KB aligned base, `TL > 0` half at `+0x4000`, 32-byte entries indexed by trap type. Matches
§2.7 exactly.

And the failure mode is modelled:

```c
if (env->tl < env->maxtl - 1) env->tl++;
else { env->pstate |= PS_RED; if (env->tl < env->maxtl) env->tl++; }
/* trapping at tl >= maxtl -> "Error state" */
```

**This is unexpectedly good news.** The nested-trap death from §2.8 — the failure this project
most feared, because on hardware it is a silent reset — shows up in QEMU as an explicit
`Trap ... while trap level >= MAXTL, Error state` message. The worst bug in Phase 2 is therefore
*observable in the emulator*.

---

## 4. What this settles

**Use a split TSB, not a common one.** The manual is unusually direct about it: with `Split = 0`
an 8 KB page and a 64 KB page can land on the same line with identical tags, and the handler
cannot tell them apart without reading the TTE data — which defeats the point of a fast path.
*"Therefore, do not use the common TSB mode in an optimized handler."*

**Sizing — revised against measurement, see §4.1.** Entries per TSB are `512 × 2^N` at 16 bytes
each, so `N = 0` is 8 KB and `N = 7` is 1 MB. **The base must be aligned to the size of both TSBs
together**, and the manual warns that *"stores to the TSB registers are not checked for
out-of-range violations"* — a misaligned base is accepted silently and fails later.

---

## 4.1 What Open Firmware actually has mapped

`sparc_dump_openfirmware_translations()` reads the firmware's `translations` property from the
kernel and prints it during `arch_vm_translation_map_init`. This is the real inherited state,
with physical addresses and modes, which `kernel_args` does not carry:

```
sparc_mmu: 17 Open Firmware translations to preserve:
  va                0x0 len     0x2000 -> pa          0x0  vwp-- size 0
  va             0x2000 len   0x200000 -> pa       0x2000  vwp-- size 0
  va           0x202000 len    0x52000 -> pa     0x202000  vwp-- size 0   <- boot loader
  va           0x254000 len   0x5ae000 -> pa     0x254000  vwp--
  va           0x802000 len     0xa000 -> pa     0x802000  vwp--
  va           0x80c000 len   0x180000 -> pa     0x80c000  vwp--          <- loader heap
  va           0x98c000 len    0x80000 -> pa     0x98c000  vwp--
  va         0x80000000 len   0x222000 -> pa     0xa0c000  vwp--          <- THE KERNEL, unlocked
  va         0xfd000000 len  0x1000000 -> pa 0x1ff22000000  vwp--         <- 16 MB frame buffer
  va         0xfef7e000 len     0x2000 -> pa 0x1ff23000000  vwp--
  va         0xfef80000 len    0x80000 -> pa   0x1fe80000  vwpl-          <- locked
  va         0xffd00000 len    0x80000 -> pa 0x1fff0000000  v-pl-         <- locked, READ-ONLY
  va         0xffd80000 len    0x80000 -> pa 0x1fff0080000  v-pl-         <- locked, READ-ONLY
  va         0xffe00000 len    0x80000 -> pa   0x1ff00000  vwpl-          <- locked
  va         0xffe80000 len    0x80000 -> pa   0x1ff80000  vwpl-          <- locked
  va         0xfffce000 len     0x2000 -> pa 0x1fe02002000  vwp--
  va         0xfffd0000 len     0x2000 -> pa 0x1fe02006000  vwp--
```

Four things fall out of this, and they change the plan.

**Open Firmware locks its own trap handler, exactly as §2.6 requires.** The two read-only locked
regions at `0xffd00000` and `0xffd80000` are where OpenBIOS's code lives — every trap PC we have
seen all session, `0xffd0f1f4`, `0xffd1c184`, `0xffd0a534`, falls inside them. **Locked entries
survive in the TLB unless explicitly demapped**, so if the cutover does not flush them, the
firmware's handlers stay reachable. That makes the transition materially less dangerous than
feared: it can be staged rather than atomic.

**But the kernel's own mapping is not locked.** `va 0x80000000 → pa 0xa0c000` carries the kernel
image and is evictable. It must be in our TSB before `%tba` is repointed, or the kernel faults on
its own code with no handler able to help.

**Every entry reports `size 0`, i.e. 8 KB pages** — `len` is the length of the region, not the
page size. So the firmware describes regions and expanding them into TTEs is our job.

**Expanded, that is a lot of pages:**

| | 8 KB pages |
| --- | ---: |
| Total across all 17 regions | 3930 |
| Locked (already resident, survive the cutover) | 320 |
| **Unlocked and evictable — must live in our TSB** | **3610** |
| Plus the kernel's own `early_map` demand | ~2700 |
| **Worst-case live entries** | **~6310** |

### Revised sizing

| `TSB_Size` | Entries | Per TSB | Split pair | Load factor at 6310 |
| :---: | ---: | ---: | ---: | ---: |
| 3 | 4096 | 64 KB | 128 KB | 1.54 — oversubscribed |
| **4** | **8192** | **128 KB** | **256 KB** | **0.77** |
| 5 | 16384 | 256 KB | 512 KB | 0.39 |

**`N = 4`**, so a 256 KB split pair aligned to 256 KB. The earlier `N = 3` guess was made before
the firmware's own mappings were counted and is too small: a direct-mapped structure at a load
factor above 1 thrashes.

**And use large pages for the big regions.** The 16 MB frame buffer alone is 2048 of those 3610
pages — over half — and the `0x5ae000` region is another 727. The TTE size field supports 512 KB
and 4 MB, so mapping those with large TTEs would cut the count by more than half and make `N = 3`
viable again. Worth doing, but as an optimisation after the fast path works, not before.

**The miss handler can be written with no stack.** MMU globals are guaranteed by hardware and
faithfully emulated, so the fast path is: read the 8 KB pointer from `ASI_DMMU_TSB_8KB_PTR`, read
the tag target from `ASI_DMMU` VA 0x00, load the TSB line, XOR the tags, and on a match store the
data half to `ASI_DTLB_DATA_IN`. No saving, no spilling, no calls.

**Lock the handler's own pages.** §2.6 is not advice. The TLB-miss handler and the TSB itself
must be locked in the TLB, or a miss on the miss handler recurses to `MAXTL` and resets the
machine.

**Ordering, revised.** Allocate and populate the TSB first, convert `early_map` to write into it,
and only then install `%tba`. Open Firmware's translations must be carried across at the moment
`%tba` changes — OF's handlers are currently servicing every trap the kernel takes, and they stop
being reachable the instant we take over.

---

## 5. The one caveat about developing against QEMU

Every mechanism above is emulated faithfully, so the *logic* can be developed and debugged in
QEMU with confidence. What QEMU does not reproduce is the hardware's timing, its caches, and its
errata — and the locking requirements in §2.6 are exactly the class of constraint that an
emulator forgives and silicon does not. A handler that is missing a lock may simply never get
unlucky under QEMU's replacement policy, and then fail on a Blade 150 under memory pressure.

So: build it against the manual, not against what the emulator tolerates. The errata in the IIi
manual's Appendix K deserve a full read before the assembly is written — we have already been
caught once by #51, the mispredicted-branch membar hazard, which cost nothing only because it was
found in the documentation rather than on hardware.

---

## 6. Sources

- **UltraSPARC-IIi User's Manual**, `SPARC/UltraSPARC-IIi/manual.pdf` — §15.2 TTE format
  (printed p.205), §15.3 TSB organisation and pointer formation (pp.207–210), §15.3.1 hardware
  support and the refill sequence (p.209), §15.3.2 alternate globals (p.211), §15.4 MMU traps
  (p.211), §15.9.6 `FIGURE 15-9` I-/D-TSB register format (printed p.227), §14.1.3 trap levels
  and MAXTL (p.224), Appendix K errata.
- **UltraSPARC Architecture 2005**, `SPARC/UA2005-HP-EXT.pdf` — `FIGURE 12-4` trap table layout,
  window state registers and the `spill_n_*` / `fill_n_*` trap types (pp.73, 87, 151, 212).
- **QEMU** `target/sparc/{int64_helper.c,win_helper.c,ldst_helper.c}` at master, read directly
  for §3.
- **OpenBSD** `sys/arch/sparc64/sparc64/{pmap.c,locore.s}` — the reference implementation, BSD
  licensed and portable per [§3.2 of the plan](PORTING_PLAN.md#32-donor-code-and-licensing).
