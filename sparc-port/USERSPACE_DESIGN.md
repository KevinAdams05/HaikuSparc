# Userspace on sun4u — the design before the code

Every phase before this one had a natural test: a banner over serial, a mounted volume, an interrupt
that arrives. This one does not, because nothing in the tree can run a user binary yet, and the pieces
that make one possible are entangled — a syscall needs a window state, a window state needs a context,
a context needs the TSB comparison to survive a non-zero value. Getting the order wrong means
debugging four things at once.

So this is written first. It is a design document, not a status one: `PROGRESS.md` records what
happened, `PORTING_PLAN.md` records the phases, and this records the decisions and the reasoning behind
them for the one subsystem where the reasoning is most of the work.

![Crossing into userspace on sun4u](diagrams/userspace-boundary.svg)


## 1. What is actually missing

Five things, in dependency order. The first two are the ones with design content; the last three are
work.

| # | Piece | State |
| --- | --- | --- |
| 1 | MMU contexts, and a TSB comparison that survives them | Nothing allocates a context; the fast path assumes zero |
| 2 | Register windows across the privilege boundary | All 64 spill/fill vectors point at the kernel handlers |
| 3 | The syscall path | No trap number chosen; `libroot`'s `syscalls.inc` emits no instructions at all |
| 4 | `arch_thread_enter_userspace`, signal frames, TLS | Three stubs and a `panic` |
| 5 | A userland to run | Phase 6's packaging gap, seen from the other side |

Items 1 and 2 are prerequisites for everything else and are independent of each other, so they can be
built and tested in either order. Item 1 is testable without any of the others, which makes it the
place to start.


## 2. Contexts, and the problem the plan did not reach

### The decision that already exists

`PORTING_PLAN.md` §4.3 settled the policy, against Haiku ticket
[#19597](https://dev.haiku-os.org/ticket/19597): **one address space, kernel mappings high and marked
Global, user mappings low and tagged with a per-team 13-bit context id.** The Global bit makes the
hardware ignore the context field during TLB hit detection, so kernel pages are visible in every
context, so the primary context register can hold the running team's id *at all times* — including
inside the kernel.

That is what keeps `IS_USER_ADDRESS` meaningful and `user_memcpy` ordinary. The idiomatic SPARC
alternative — kernel in its own context, user memory reached through `ASI_*_AS_IF_USER` — is a
cross-cutting change to Haiku's shared code, and §4.3 is right to refuse it.

The translation map already implements this half. `tte_flags_for_attributes()` sets `TTE_GLOBAL` and
`TTE_PRIVILEGED` for kernel mappings and neither for user ones, so a user page is already
context-tagged in the only sense the hardware cares about. What is missing is anything that *allocates*
a context id or writes it to the primary context register.

### The part §4.3 does not address

The Global bit governs **TLB** hit detection, which is hardware. The **TSB** comparison is software —
the hardware only forms the pointer and the tag target, and the miss handler does the compare itself
with a single `xor`. So the Global bit buys nothing there, and the two operands disagree the moment a
context is non-zero:

```
hardware tag target :  (current primary context << 48) | VA<63:22>
stored tag, kernel  :  (0                       << 48) | VA<63:22>
```

While a user context is loaded, every kernel address misses the fast path, falls into the slow path,
walks three levels of page table, and refills a TSB line that will mismatch again on the next access.
Correct, and catastrophically slow — the kernel would run permanently on the slow path.

`sparc_tsb_insert()` anticipated this. It stores `virtualAddress >> 22` with the Global bit
deliberately clear and a comment saying the comparison "will need to become G-aware" when user contexts
arrive. Worth being precise about what that should mean, because the obvious reading is the more
expensive one.

### The mechanism: mask by address, not by the G bit

Making the comparison G-aware in the literal sense — set the Global bit in the stored tag, have the
handler test it and compare fewer bits when it is set — costs a load-dependent branch on the hit path,
which is the single hottest path in the system.

There is a cheaper equivalence. The tag target contains `VA<63:22>` at bits 41:0, so virtual address
bit *n* sits at tag bit *n* − 22 — and a single bit separates the halves, because the kernel owns
everything from `KERNEL_BASE` upwards and `KERNEL_BASE` is a power of two. On this port that is
`0x80000000`, so the bit is **VA<31>, at tag bit 9**. The handler can therefore decide which kind of
address faulted from a register it has already loaded, without touching memory and without consulting
the stored tag at all:

```
    if tag_target has bit 9 set:         # kernel address
        tag_target &= ~(0x1fff << 48)    # mask the context off
    xor against the stored tag
```

Kernel tags stay stored with context zero, exactly as they are now. User tags get the mapping's
context. Four instructions on the fast path with no branch at all — shift the bit to the top, let an
arithmetic shift turn it into an all-ones or all-zeroes mask, clear bits 63:48 — and the Global bit in
the stored tag stays unused, which is fine, since its only purpose was to tell a handler to do what
this handler now decides more cheaply.

Note which way round it is: the mask applies to *kernel* addresses and the context is kept for
everything else. So an address below `KERNEL_BASE` that no team owns — a wild pointer, a leftover
firmware mapping — is compared with the context included and misses, which is the outcome it deserves.
The reverse polarity would silently hand such an access a kernel translation.

`KERNEL_BASE` being `0x80000000` rather than something in the high half is worth stating, because it is
easy to assume otherwise from the other 64-bit ports and this document did on its first draft.
`arch_kernel.h` records why: it previously carried x86_64's `0xffffff0000000000`, which described an
address space this kernel has never been near, and `IS_KERNEL_ADDRESS` rejected every address the
kernel actually uses. Userspace therefore gets the low 2 GB. Since the whole single-bit trick depends
on that layout, `sparc_verify_mmu_defines()` derives the bit from `KERNEL_BASE` at init and panics if
the two disagree — rather than leaving a constant that is correct only by coincidence.

One consequence to state plainly: **user entries from different contexts alias in a shared TSB.** Their
tags differ, so the comparison correctly misses and the slow path fixes it up — this is a capacity
problem, not a correctness one, and it is the same aliasing the TSB already has between any two regions
64 MB apart. A per-address-space TSB is the answer if it ever measures badly, and the TSB base register
is per-CPU state that a context switch could write. Not first.

### Context ids

Thirteen bits, so 8192 ids, of which 0 is the nucleus context and belongs to the kernel. Recycling is
required and the recycling is where the bugs live: when an id is reassigned, every TLB entry and every
TSB line still carrying it becomes a mapping into the wrong address space.

The demap-by-context operation is what makes this tractable — one store to the DMMU and IMMU demap
registers removes every non-locked entry for a context. The TSB has no such operation, so a reassigned
context needs the TSB cleared of that context's lines, and since the TSB has no per-context index that
means walking all 8192 of them.

**What was implemented is simpler than what this section first proposed**, and better for the same
reason: an id is *freed when its address space is destroyed*, not stolen from a live one. That makes
exhaustion mean 8191 simultaneously live teams rather than 8191 teams over the machine's uptime, which
turns a recycling scheme into a case that does not arise. `arch_vm_translation_map_create_map()` fails
if it ever does, which is honest and visible; stealing an id from an inactive address space, as the
arm64 port does with refcounts, is the answer if it ever matters.

The invalidation is the part that is not optional either way. It happens on *free*, before the id goes
back in the bitmap, because the other order leaves a window where another team could be handed an id
whose translations are still cached — and that failure is not a slow path, it is one team reading
another team's memory.

### What is testable, and it is a real test

None of this needs a user binary. A test can allocate two contexts, map the same virtual address to
two different physical pages in each, switch the primary context register between them, and check that
a load returns each page in turn — and that a kernel address keeps reading correctly throughout, which
is the assertion the masking above exists to satisfy. That last part is the one that would otherwise
be discovered as a mysterious slowdown.


## 3. The page table root — implemented, reverted, and not understood

This was not on the list in section 1, and it should have been. Contexts decide which mappings the
*hardware* will match. They say nothing about which page table the TLB miss handler walks to find
them — and that handler walks exactly one, whose physical root is put in `%g3` at cutover and never
changes. A user address space gets a table of its own, so its mappings would never be found.

The fix looks small. The same address bit the fast path already tests for the tag masking says which
root applies; the kernel's is in `%g3`; the running team's can live in the trap data block beside the
context register it has to agree with, written by the same function so the two cannot drift. Six
instructions in the slow path.

**It hangs the machine, reliably, and the reason does not survive contact with the evidence.**

The symptom is precise. The boot proceeds normally for sixty-three seconds and stops inside
`user_memcpy()` on the deliberate probe against an unmapped user address — the one arch_int.cpp already
runs to prove faults are caught. Nothing is printed after it. Every kernel address is unaffected, which
is why the boot gets that far: the probe is the first and only user-half access the whole boot makes.

What the bisect found, one boot per row:

| Slow-path selection | Result |
| --- | --- |
| `mov %g3, %g5`, then walk from `%g5` | boots, mounts, probe passes |
| plus `srlx`/`andcc` to test the address bit | boots, mounts |
| plus `ldx [%g7 + 0xc0]`, result discarded | boots, mounts |
| plus a `stx` recording what was loaded | boots, mounts — **records `0x0`** |
| `movne %xcc, %g3, %g5` then `brz` to the fault exit | hangs in the probe |
| `movne` then `movrz %g5, %g3, %g5` | hangs earlier, during TSB area setup |
| `ldx` then `movrz` alone | hangs earlier still |
| two conditional branches, falling back to `%g3` | hangs in the probe, reproducible across reruns |

Three things were checked rather than assumed. The **disassembly** of every variant is what was
intended — `movre %g5, %g3, %g5` encodes rcond=RZ, rs1=`%g5`, rs2=`%g3`, rd=`%g5` correctly. The
**loaded value is zero**, recorded by the handler itself into a spare trap-data word and printed. And
`MOVcc` and `MOVR` **both work**, verified by a probe that runs them with known inputs in both the
taken and not-taken direction and prints the results.

Which leaves a contradiction. The last row's code ends with `%g5 == %g3` on every path — for a kernel
address by the branch's delay slot, for a user address because the loaded root is zero and falls back
— so it is behaviourally identical to the first row, and does strictly *less* work than the fourth. It
hangs and they do not.

Every variant that works is straight-line. Every variant that hangs makes `%g5`'s final value depend on
data, even when the value is provably the same. That is the whole correlation, and it is not a
correlation any model of the architecture explains, which means the model is wrong somewhere rather
than the code being subtly buggy.

So it is **reverted** rather than shipped. It is not needed until a user address space exists, and
shipping a change that reliably hangs the boot to enable something nothing uses yet is the wrong trade.
The half that is correct and verifiable stays: the trap data block has the field, offset-checked
against the assembler like every other, and `sparc_switch_address_space()` writes it beside the context
register. Only the reader is missing — which is also why **user address spaces do not work yet**, and
this is the first thing to fix before they can.

The next diagnostic, and it is cheap: run the failing build under `-accel tcg,one-insn-per-tb=on`. If
the hang goes away, it is QEMU's translation of a branch inside the miss handler at TL>0 and the code
is right; if it stays, it is ours. That single measurement decides which of two very different
investigations to start, and it should be the first thing tried rather than more reading.


## 4. Register windows across the boundary

### What the hardware does

A trap from userspace leaves the user's windows live. The kernel needs windows of its own, so it
executes `save`, and if `CANSAVE` is zero that traps to a spill handler. The hardware chooses between
two sets of spill vectors: `spill_*_normal` when the window being spilled belongs to the current
privilege level, and `spill_*_other` when `OTHERWIN` says it belongs to the other one. Thirty-two
vectors each way, spill and fill, because the trap type encodes the window number.

The trap table currently points all sixty-four at `SPILL_KERNEL_WINDOW` and `FILL_KERNEL_WINDOW`. For a
kernel-only system that is correct and the `_other` vectors never fire.

### Why the obvious handler is wrong

The obvious `spill_*_other` stores the sixteen window registers to the user stack frame the window
names. Two things break it.

**It can fault.** A user stack page can be paged out, or not mapped at all, or the stack pointer can be
garbage because userspace is allowed to put garbage there. The store faults, and this code runs at
TL>0, where a fault nests one trap level deeper. Section 2.6 of the porting plan already says where
that ends: four levels in, the CPU takes a watchdog reset and reports nothing.

**It must not use ordinary stores.** The window belongs to userspace, so the access has to carry user
privilege — a kernel store would succeed against a page the user cannot write. That is what
`ASI_AS_IF_USER_PRIMARY` is for, and it does not remove the faulting problem.

### The design: spill into kernel memory, copy out at TL=0

Both problems dissolve if the spill target is kernel memory. Give each thread a save area sized for the
maximum number of live windows, have `spill_*_other` store into it and bump a count, and copy the
saved windows out to the user stack later — at TL=0, from C, where a fault is an ordinary page fault
handled by the ordinary handler.

The cost is a copy. The benefit is that the only code running at high trap level touches memory that
is always mapped and always writable, which is the same rule the TLB miss handler already lives by.
Linux and OpenBSD both arrived here; it is the standard answer rather than a clever one.

The matching `fill_*_other` reads from the user stack, which can also fault. Same treatment: the fill
handler faults into a fixup path rather than trying to be clever, because a fill that cannot be
satisfied is a thread that must be killed, not a trap to be retried.

### Where the save area lives

`struct arch_thread` is the natural home and it is already the place `arch_context` lives. The save
area has to be reachable from a trap handler with no stack, which means through `%g7` — the thread
pointer, which the trap handlers already use and which `arch_thread_set_current_thread()` already
maintains. So the addressing is `%g7 + offsetof(Thread, arch_info.window_save_area)`, one constant the
assembler cannot see and which therefore needs the same offset-verification treatment
`sparc_verify_context_layout()` already gives `arch_context`.


## 5. The syscall path

### The trap number

SPARC V9 `Tcc` produces trap types `0x100 + n` for a software trap number `n` in 0..127, and the trap
table already reserves all 256 entries from `0x100` for them. Nothing constrains the choice; the
convention on this architecture is that every operating system picks its own, which is why SunOS,
Solaris, Linux and the BSDs all use different ones.

**Proposed: `ta 0x40`, trap type `0x140`.** Mid-range, so a stray `ta` with a small immediate — the
shape a compiler or a hand-written stub is most likely to emit by accident — lands on an unhandled
entry that reports rather than on the syscall path.

### Argument passing, and the reason it is not obvious

Haiku syscalls take up to twenty parameters. SPARC V9 passes six in `%o0`–`%o5` and the rest on the
stack, and the kernel's dispatcher wants a pointer to a contiguous argument list.

The SPARC V9 ABI makes this almost free. It reserves six argument slots at `%sp + BIAS + 128`,
immediately below where stack arguments start at `%sp + BIAS + 176` — the slots exist precisely so that
a varargs callee can spill its register arguments and read the whole list as one array. A syscall stub
can do the same thing: store `%o0`–`%o5` into those slots, and the argument list is contiguous from
`%sp + BIAS + 128` regardless of how many there are.

So the stub is: store six registers, put the call index somewhere the handler can find it, `ta 0x40`.
The kernel reads the index and a pointer to the list. Two facts make this safe to read from the kernel
— the list is in the *user's* stack, so reading it is a `user_memcpy` and can fault, which at TL=0 is
fine.

### The register the index travels in

`%g1` is the conventional choice on this architecture and it is convenient here for a reason beyond
convention: the trap handlers run with the alternate global bank selected, so the user's `%g1` is still
intact and readable when the handler wants it — but it must be saved before the handler uses `%g1` as
scratch, which the existing entry paths already do into the iframe.

### The return

`%o0` and `%o1`, as the ABI says. After the handler's `save`, the user's `%o` registers are the
handler's `%i` registers, so the return value is written to `%i0` and the closing `restore` puts it
where userspace will read it. No iframe field needed, which is why the iframe has no `%o` registers in
it — a fact worth stating, because their absence looks like an omission.


## 6. Order of work

1. **Contexts.** The allocator, the primary context register on address-space switch, and the tag
   target masking. All three are done. What is *not* done is the page table root the miss handler needs
   to go with them — §3 — and until that works no user address space can be mapped at all, so this
   step is the blocker for every step below it rather than a finished one.
2. **`arch_thread_enter_userspace` plus the syscall trap.** Together, because neither is observable
   without the other. Test with a hand-built user thread — a page of user memory containing a couple
   of instructions and a `ta` — rather than waiting for a userland. A thread that never executes
   `save` needs none of item 3, which is what makes this a step and not a leap.
3. **The `_other` window handlers.** Then extend the test thread to `save` and `restore` and nest a
   few calls deep, which is what actually exercises them.
4. **Signals and TLS.** `arch_setup_signal_frame`, `arch_restore_signal_frame`,
   `arch_on_signal_stack`, `arch_thread_init_tls`.
5. **The userland.** `libroot`'s `syscalls.inc` — currently a stub that emits a label and no
   instructions, so every syscall in the system falls through into the next function — then
   `runtime_loader`, then something to run.

Steps 1 through 3 are kernel work with hand-built tests and no dependency on the image build. Step 5
depends on Phase 6's media gap, which is the reason this ordering puts it last rather than first.


## 7. Decisions recorded

| Decision | Alternative rejected | Why |
| --- | --- | --- |
| One address space, kernel Global | Separate kernel context, `ASI_*_AS_IF_USER` for user memory | §4.3: the alternative is a cross-cutting change to Haiku's shared code. Ticket #19597. |
| Mask the context off kernel tag targets | Set the Global bit in stored tags and branch on it | No load-dependent branch on the hottest path in the system |
| One TSB shared by all contexts | Per-address-space TSB, base register switched on context switch | Aliasing is a capacity problem, not correctness. Revisit with a measurement. |
| Free a context id when its address space is destroyed; fail on exhaustion | Stealing an id from an inactive address space | Exhaustion becomes 8191 *live* teams, which does not arise. Revisit with arm64's refcount scheme if it does. |
| Invalidate a context before returning its id, never after | Invalidate on allocation | The other order has a window where a new team holds an id whose translations are still cached |
| Spill user windows into a per-thread kernel save area | Store straight to the user stack via `ASI_AS_IF_USER_PRIMARY` | A fault at TL>0 nests toward a watchdog reset. Same rule the miss handler lives by. |
| `ta 0x40` for syscalls | A low trap number | A stray `ta` with a small immediate hits an unhandled entry that reports |
| Arguments in the ABI's reserved slots at `%sp + BIAS + 128` | A separate argument buffer the stub builds | The slots exist for exactly this; stack arguments already continue from there |
