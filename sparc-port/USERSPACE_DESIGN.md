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
| 1 | MMU contexts, a TSB comparison that survives them, and a per-address-space page table root | **Done** |
| 2 | The syscall trap, and entering userspace | **Done** — `ta 0x40`, and an instruction runs unprivileged |
| 3 | Register windows across the privilege boundary | All 64 spill/fill vectors point at the kernel handlers |
| 4 | TLS — **done**, §4b's decision made it three lines. Signal frames | `arch_setup_signal_frame` and friends are stubs |
| 5 | A userland to run | `libroot`'s `syscalls.inc` emits no instructions at all; plus Phase 6's packaging gap |

The system call path is no longer a stub either: `syscall_dispatcher()` is wired, and userspace has
called `system_time()` and got a real answer.

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


## 3. The page table root, and the register the firmware was eating

This was not on the list in section 1, and it should have been. Contexts decide which mappings the
*hardware* will match. They say nothing about which page table the TLB miss handler walks — and that
handler walks exactly one, whose physical root is put in `%g3` at cutover and never changes. A user
address space gets a table of its own, so its mappings would never be found.

The fix is small and now works. The same address bit the fast path tests for the tag masking says which
root applies; the kernel's is in `%g3`; the running team's lives in the trap data block beside the
context register it has to agree with, written by the same function so the two cannot drift. Six
instructions, branch-free — sign-extend the bit into a mask and select between the two roots with
`andn`/`and`/`or`.

Getting there took a day, because it hung the machine and the hang was three layers away from its
cause. The path is worth recording, since the same shape will recur.

### What it looked like

The boot proceeded normally for sixty-three seconds and stopped inside `user_memcpy()` on the
deliberate probe against an unmapped user address — the only user-half access the whole boot makes.
Nothing was printed after it. Twelve boots of bisecting produced a result that made no sense: every
variant that *used* the loaded root hung, every variant that *ignored* it worked, including a
branchless one that computed the same value the working ones used. The disassembly was right each time.
The loaded value was recorded by the handler itself and printed as `0x0`. `MOVcc` and `MOVR` were both
verified working by a direct probe.

At that point the conclusion drawn was "internally contradictory, causes eliminated" — and reverting was
right, but the diagnosis was not finished. The mistake was reading *silence* as a hang.

### What it actually was

The machine was not hung. `info registers` on the live guest through the QEMU monitor put the PC at
`sparc_unhandled_trap_stop` — a `b,a` to itself — with `%tl` at 2. That is this port's own designed
behaviour: an unhandled trap at TL>1 parks at a named symbol rather than nesting toward a watchdog
reset. The trap data block had the whole record, and `-d int` had the register file at the faulting
instruction:

```
Unaligned Memory Access (v=0034)   pc: 801b80d0   tl: 1   pstate: 00000414  (MG set)
%g3 = 0000000000d92000   the kernel page table root, correct
%g4 = 0000000000000016   the unaligned address
%g5 = 0000000000000016   the "selected root"
%g6 = 0000000040000000   Tag Access -- the probe address
%g7 = 00000000ffe80018   NOT the trap data block
```

`%g7` in the **MMU global bank** held an address inside Open Firmware's own image — one of the locked
512 KB firmware pages. So `ldx [%g7 + 0xc0]` read the firmware's memory, got `0x16`, and used it as a
page table root.

`call_open_firmware()` already knew the firmware eats `%g6` and `%g7`; it saves and restores them, and
carries a long comment about a timer interrupt that once read `%g7` as an OpenBIOS address. But that
save is ordinary C at trap level zero, so it saves the **normal** bank, and the firmware's writes land
in whichever bank its own PSTATE selects. The MMU and alternate banks are separate register files and
had been rotting since the cutover.

Nothing noticed for four phases because nothing read them. The miss handlers keep the trap data pointer
in `%g7` and the page table root in `%g3` and use neither — the fast path reads MMU registers, the slow
path walks physical addresses out of `%g3`, and the paths that *do* use `%g7`, the fault entry and the
unhandled-trap reporter, run on the alternate bank, which the firmware happened not to disturb. The
root selection was the first code in the kernel ever to read `[%g7 + offset]` from the MMU bank.

`sparc_restore_trap_globals()` re-establishes both banks after every client call, by calling the same
function the cutover called so the two cannot drift, and reports once what it found:

```
sparc_mmu: %g7 in the MMU bank after a firmware call was 0xffe80018,
           trap data is at 0x8024f800 -- CLOBBERED, and restored each call
```

### Three lessons, all cheap

**Silence is not a hang.** This port builds a diagnostic for exactly this case — record the trap and
park at a named symbol — and it worked perfectly. One `info registers` would have ended the
investigation on the first boot instead of the twelfth.

**Bisecting told the truth and it was still misleading.** "Every variant that uses the loaded value
fails" was correct, complete, and pointed at the value rather than at the register it was loaded
through. The correlation was real; the inference from it was not.

**A verification that runs once verifies once.** `sparc_verify_trap_globals()` checks `%g7` in both
banks at boot, passes, and then the value rots. Anything the firmware can reach needs re-establishing
or re-checking, not proving once.


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

### Reaching them: the OTHERWIN transfer, which is not written

The handlers exist and are verified. **They are also unreachable**, and this is the
part to do next rather than a detail.

The hardware picks `spill_*_other` over `spill_*_normal` when `OTHERWIN` is
non-zero, and `OTHERWIN` is software-managed state. Nothing in this port writes
it — the only reference anywhere is a *read*, in the unhandled-trap reporter. So
it is zero always, the `_other` vectors never fire, and a user window spilled
today goes through the kernel handler straight to the user's stack. That is why
an eight-deep `save`/`restore` in the userspace test passes: it is exercising
`spill_*_normal`, which works for a cooperative program and is the hole this
design exists to close.

What reaching them takes is the standard transfer, on kernel entry from
userspace, after `TRAP_TO_C`'s own `save`:

```
	rdpr	%canrestore, %g1
	wrpr	%g0, 0, %canrestore
	wrpr	%g1, 0, %otherwin
```

The user's live windows become "other", so spilling any of them selects the
handler that knows not to touch the user's stack. `CANSAVE + CANRESTORE +
OTHERWIN = NWINDOWS - 2` is preserved, because the move is a move.

And the reverse immediately before the closing `restore`, which is not optional:
that `restore` returns *into* the trapped window, and with `OTHERWIN` non-zero
and `CANRESTORE` zero it would trap to `fill_*_other` and try to fill a window
that is still live in the register file.

**The hazard, and the reason this was not rushed:** the C handler between those
two points can block. A page fault pages something in; a system call sleeps. The
context switch does `flushw`, which spills every window and renumbers the
register file — and §29's preemption bug was exactly this, `retry` restoring a
`CWP` that no longer meant the same window. `OTHERWIN` has to survive that, which
means it belongs in the saved context alongside `PSTATE` and `PIL` rather than
only in the registers. Getting it wrong gives an intermittent fault in ordinary
kernel code, which is the worst failure mode this port has produced so far and
took two sessions to find last time.

### And WSTATE, which the first version of this section missed

Working the accounting through properly turns up something the design above got
wrong, and it is worth stating because it changes what has to be written.

`OTHERWIN` is not the only selector. The trap type for a spill is
`0x80 + 4 × WSTATE.NORMAL` when `OTHERWIN` is zero and
`0xa0 + 4 × WSTATE.OTHER` when it is not — which is why there are eight groups of
each rather than one. `WSTATE` is how the operating system says *which* handler a
spill should use, and it has a normal half and an other half precisely so that
"spilling my own window" and "spilling the other privilege level's window" can be
answered differently.

The consequence: **`spill_*_normal` needs a user variant too.** A user program that
nests deeply spills its own windows while running unprivileged. `OTHERWIN` is zero
then — the kernel is not involved — so the hardware picks `_normal`, and today
that is the kernel handler storing to the user's stack with kernel privilege. The
store lands in the right place and skips the page protections, which is wrong for
the same reason the `_other` case was: it should be an `ASI_AS_IF_USER_PRIMARY`
store that faults if the user cannot write there.

So `WSTATE.NORMAL` wants one value while the kernel runs and another while
userspace does, set on the way in and out alongside the `OTHERWIN` transfer.

### And the save area has to be emptied before returning to userspace

The other thing the accounting forces. Leaving user windows in the kernel's save
area across a return does not work: on the way out the kernel restores
`CANRESTORE` from `OTHERWIN`, and a `restore` past the live windows then traps to
a fill with `OTHERWIN` at zero — the `_normal` vector, which is not where the
saved copies are.

Which is what §4's "copy it out later at TL=0" means concretely: before returning
to userspace, whatever is in the save area is written to the user's stack from
C, where a fault is an ordinary page fault and can page the stack in or kill the
thread. Linux calls this `fault_in_user_windows` and does it in its return path
for exactly this reason. `thread_at_kernel_exit()` is the natural hook here.

So the full piece is four parts, and **all four are done**:

1. **The `_other` handlers and the save area.**
2. **`WSTATE` and the `OTHERWIN` transfer** on kernel entry and exit.
3. **A user variant of the `_normal` spill and fill**, storing through
   `ASI_AS_IF_USER_PRIMARY`.
4. **Flushing the save area to the user stack at TL=0** before returning to
   userspace.

Both spill paths are counted, so reachability is a measurement rather than an
argument:

```
sparc_int: window spills -- 2 to the user's stack, 1 parked by the kernel
sparc_int: userspace returned 0x5ac of 0x5ac -- ran in userspace
```

Two that userspace took itself, against a `CANSAVE` of six after eight nested
saves; one the kernel took on userspace's behalf, parked and then flushed. The
test still returns the value it chose, so the fill side works too.

### One prediction this section got wrong

It said `OTHERWIN` and `WSTATE` would have to live in `arch_context`, because the
handler between entry and exit can block and a context switch does `flushw`.
Neither does, and the reason is worth keeping.

`WSTATE` never needs saving because a context switch always happens *inside* the
kernel, so the value at switch time is always the kernel's, and the exit path sets
the user's again on the way out. And `OTHERWIN` needs no saving because `flushw`
is exactly what resolves it: with `OTHERWIN` non-zero those spills go to the save
area, which is per-thread, and drain `OTHERWIN` to zero on the way. **The state
that had to survive survives as data rather than as register contents** — which is
what the save area was for in the first place.

### What is still missing

A fault *during* a spill or fill. Both new handlers can take one — an absent or
unwritable user stack page — and neither is fixed up; they report through the
unhandled-trap path. That is what Linux's `winfixup` exists for, and it is the
next thing here rather than something to leave implicit.


### Where the save area lives

`struct arch_thread` is the natural home and it is already the place `arch_context` lives. The save
area has to be reachable from a trap handler with no stack, which means through `%g7` — the thread
pointer, which the trap handlers already use and which `arch_thread_set_current_thread()` already
maintains. So the addressing is `%g7 + offsetof(Thread, arch_info.window_save_area)`, one constant the
assembler cannot see and which therefore needs the same offset-verification treatment
`sparc_verify_context_layout()` already gives `arch_context`.


## 4a. Open: the map lock's holder goes to zero

The boot panics after the mount, in `launch_daemon`'s address space teardown:

```
PANIC: mutex_destroy(): the lock (0x8100def8) is held by 0, not by the caller
```

Reachable now only because `Map()` finally succeeds. Before the page table root
was allocated, every `Map()` failed immediately, so a user map's lock was taken
and released and nothing else happened; the map was then destroyed cleanly. Now
the map is actually used, and by the time it is destroyed its lock's holder is 0
rather than the -1 an unheld mutex carries.

**Not caused by the userspace work.** It happens with `sparc_test_userspace()`
disabled, so it is `launch_daemon`'s address space and not the probe's.

What has been eliminated, so the next attempt does not repeat it:

- **Object corruption.** Instrumenting the destructor printed the lock's own
  address, matching the panic exactly, with `holder` reading **-1** and
  `recursion` 0 — an initialised, unheld lock. The object is intact.
- **The value changing across the report.** Re-read immediately after the
  `dprintf` and compared: unchanged. So the same field reads -1 from the
  destructor and 0 from `mutex_destroy()` a call later, which is the part that
  does not fit any model yet.
- **Double destruction.** One destructor report per boot.
- **A `KDEBUG` layout disagreement** between `SPARCVMTranslationMap.cpp` and
  `lock.cpp`. `fLock.lock.holder` only compiles when `KDEBUG` is on, and
  `mutex_destroy()`'s check is `#if KDEBUG`, so both translation units see the
  same struct.
- **A spinlock write spilling into the neighbouring field.** `holder` sits
  immediately after the mutex's `spinlock` and `mutex_destroy()` acquires it, so
  a 64-bit atomic on a 32-bit field would zero `holder` exactly as observed. It
  does not: both `try_acquire_spinlock()` and `release_spinlock()` compile to
  `swap [%i0], %g1`, which is 32-bit.

Then a second round, all negative, all measured:

- **The struct layout, measured rather than reasoned about.** The destructor
  printed `KDEBUG 1, offsetof(mutex, holder) 20, sizeof(mutex) 32,
  sizeof(spinlock) 4`, and `lock.cpp`'s own code in the binary reads `holder` at
  `[%i0 + 0x14]` and `flags` at `[%i0 + 0x18]`. Both translation units agree, and
  `0x8100def8 + 0x14` is exactly the address the destructor printed. **It is the
  same word.**
- **`mutex_init_etc()` failing to initialise it.** It sets `holder = -1` under
  `KDEBUG`, so a never-locked mutex is not the answer.
- **A thread id of 0 being stored as the holder.** It does happen —
  `thread_get_current_thread()->id` is 0 for the early boot thread, and the
  kernel map's lock is taken 868 times that way during
  `vm_translation_map_init_post_area()`. But those balance, and nothing reports a
  bad id at the time the user map is used.
- **The page table teardown corrupting a later object.** The panic survives
  making `_FreePageTable()` leak instead of free.
- **The trap entry's kernel stack switch.** The panic survives reverting it to the
  old unconditional `save`.
- **The userspace probe.** The panic survives disabling it entirely.
- **Memory visibility.** `mutex_destroy()` reads the holder *after*
  `acquire_spinlock()`'s full `membar`, and the destructor's read does not — the
  one asymmetry between them. Reading the address twice with
  `memory_full_barrier()` in between gives **-1 both times**.

So: address `0x8100df0c` reads -1, reads -1 again after a barrier, and reads 0
from `mutex_destroy()` one call later. Between those reads are two function
prologues, three stores into `mutex_destroy()`'s own stack frame, a `wrpr
%pstate` to disable interrupts, and `acquire_spinlock()` — whose only write is a
32-bit `swap` at `+0x10`, four bytes below the holder, verified in the binary.

### And then the actual shape of it

Instrumenting `mutex_destroy()` itself — printing the raw words it sees on entry
and again after taking the spinlock — changed the question entirely:

```
mutex_destroy: 0x8100cb98 after spinlock holder -1, raw[4] 0x1 raw[5] 0xffffffff
error starting "/boot/system/servers/launch_daemon" error = -1
mutex_destroy: 0x8100cb98 entry holder 0, raw[4] 0x0 raw[5] 0x0
PANIC
```

**`mutex_destroy()` runs twice on the same address**, and the second time the
memory is *zeroed* — spinlock and holder both 0. The first call is the healthy
one, with holder -1, which is exactly the value the destructor's own report kept
showing. So nothing corrupts a holder: **the report and the panic were looking at
two different destructions**, and every measurement above was consistent all
along. The apparent contradiction was mine.

`operator delete` is called with size 0x48, which is `sizeof(SPARCVMTranslationMap)`,
so the object really is freed between the two. Either the same map is destroyed
twice, or the address is reused by something whose lock was never initialised —
and a zeroed mutex distinguishes those: `mutex_init_etc()` writes -1, so zeroed
memory means no constructor ran there.

Both live in the `launch_daemon` failure path, in shared code
(`VMAddressSpace::~VMAddressSpace()` → `delete fTranslationMap`). It became
reachable when `Map()` started succeeding, because before that a team failed
earlier and took a different path out.

**Next step**, and it is now a narrow question rather than a hunt: find the second
caller. A generation counter in the map object, bumped in the constructor and
checked in the destructor, separates double-destroy from address reuse in one
boot; the destructor already carries a magic field in the experiment that got
this far, and it read *valid* on the one destruction it saw, which is the
remaining thing that does not add up. After that, `%i7` from the second call's
frame names the caller directly.


## 4b. The %g7 decision, settled

`%g7` was wanted by two owners. In the kernel it is the thread pointer —
`thread_get_current_thread()` is a read of it, and every C trap handler needs it.
In userspace it is *also* the thread pointer, because that is where SPARC keeps
one for thread local storage. There is one `%g7` per register bank, and the normal
bank is the one both use.

So `sparc_enter_userspace()` originally left the globals alone and said why:
zeroing `%g7` made the first trap out of userspace dereference null, and leaving
it meant userspace could read the kernel's thread pointer and — worse — write it,
so that the next trap's handler followed a value userspace chose.

Three ways out were available:

| Option | Cost |
| --- | --- |
| Load the kernel's pointer on trap entry from the trap data block | Two instructions per trap |
| Give userspace a different register, say `%g6` | Diverges from what every SPARC toolchain and libc expects |
| Stop using `%g7` in the kernel; derive the `Thread*` from the kernel stack pointer | Arithmetic on the hottest accessor in the kernel |

**The first, and it needed no new machinery.** The trap data block is per-CPU and
is reached through the *trap's own* global bank — a separate register file
userspace has no instruction to touch — and it already carries exactly this class
of state: the kernel stack, the user page table root, the window save area. The
current thread is the fourth of the same kind.

`TRAP_TO_C` reads it before switching to the normal bank, carries it across in
`%l6` (window registers are not banked), and installs it into `%g7` *after* the
interrupted code's value has gone into the iframe — so the epilogue hands
userspace its own `%g7` back and never leaks the kernel's.
`arch_thread_set_current_thread()` publishes it, so the register and the block
cannot drift.

With that, `sparc_enter_userspace()` clears all seven globals. Zero for now
because there is no thread local storage yet; when there is, the thread's TLS
pointer goes in `%g7` there and nothing else changes. **That is the TLS blocker
gone.**

### What it turned up

The interrupt global bank's `%g7` had never been initialised. UltraSPARC-IIi gives
the interrupt traps a bank of their own selected by `PSTATE.IG` — §22's bug was a
handler running on the wrong one because only `AG` was cleared — and
`sparc_set_trap_globals()` wrote the MMU and alternate banks only. Nothing had
ever *read* `%g7` there, so nothing noticed.

The first interrupt after this change read a garbage pointer out of it and
reported `thread -1 "?"` before taking an alignment fault. Fixed by setting all
three banks, which is also what `sparc_restore_trap_globals()` should have been
covering since §32.


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

1. **Contexts.** The allocator, the primary context register on address-space switch, the tag target
   masking, and the per-address-space page table root the miss handler needs to go with them. **Done.**
   A user address space can now be mapped and its mappings found.
2. **`arch_thread_enter_userspace` plus the syscall trap. Done.** An instruction runs in userspace,
   in its own context-tagged address space, and traps back with a value it chose. Three things had to
   go with it: the page table root allocated on demand, the trap entry choosing a kernel stack rather
   than the user's, and user page table teardown. One thing came out of it and is still open — §4a.
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
| Re-establish the trap global banks after every firmware call | Saving and restoring three banks' worth around each one | Idempotent, two stores per bank, and it calls the same function the cutover did so it cannot drift |
| Arguments in the ABI's reserved slots at `%sp + BIAS + 128` | A separate argument buffer the stub builds | The slots exist for exactly this; stack arguments already continue from there |
