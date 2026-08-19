/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*	The sun4u trap table, and the TLB miss fast path.
 *
 *	NOT YET INSTALLED. %tba still points at Open Firmware's table and the
 *	firmware is still servicing every trap. This file is built so that it is
 *	syntax-checked and reviewable, and so that installing it later is a change
 *	to one register rather than a change to a thousand lines. Nothing here runs
 *	until sparc_install_trap_table() exists and is called.
 *
 *	Table geometry, from the UA2005 spec FIGURE 12-4 and confirmed against
 *	QEMU's vectoring code:
 *
 *	    entry address = %tba | (TL > 0 ? 0x4000 : 0) | (trap_type << 5)
 *
 *	so the table is 32 KB: 512 entries of 32 bytes for TL = 0, then 512 more
 *	for TL > 0, and %tba must be 32 KB aligned.
 *
 *	The handlers below implement the refill sequence from the UltraSPARC-IIi
 *	User's Manual section 15.3.1. Every step of that sequence has already been
 *	verified from C against this machine -- the index arithmetic agrees with
 *	the pointer the hardware forms, a TTE built by that code maps memory when
 *	loaded through ASI_DTLB_DATA_IN, and the tag comparison finds the right
 *	entries. What follows is a transliteration of logic that is known to work,
 *	which is the only sane way to write code that runs with no stack.
 */

#define FUNCTION(x) .global x; .type x,@function; x

/* ASIs, from Table 6-10 of the UltraSPARC-IIi User's Manual. */
#define ASI_IMMU			0x50
#define ASI_IMMU_TSB_8KB_PTR		0x51
#define ASI_ITLB_DATA_IN		0x54
#define ASI_DMMU			0x58
#define ASI_DMMU_TSB_8KB_PTR		0x59
#define ASI_DTLB_DATA_IN		0x5c
#define ASI_NUCLEUS_QUAD_LDD		0x24

/* Register offsets within ASI_IMMU / ASI_DMMU. */
#define MMU_TAG_TARGET			0x00

/* Trap types we handle, from TABLE 15-3. */
#define TT_INSTRUCTION_MMU_MISS		0x64
#define TT_DATA_MMU_MISS		0x68


/*	%g2 and %g3 are the SPARC V9 "application" globals and %g6/%g7 are reserved
 *	for system software, so the assembler insists they be declared before use.
 *	Scratch is right here: on an MMU trap the hardware has swapped in the MMU
 *	global bank, so these are a private set that belongs to the handler and to
 *	nothing else.
 */
.register %g2, #scratch
.register %g3, #scratch
.register %g6, #scratch
.register %g7, #scratch


.text

/*	The TLB miss fast path.
 *
 *	Entered with the MMU globals selected by hardware -- see section 15.3.2,
 *	which lists fast_instruction_access_MMU_miss and fast_data_access_MMU_miss
 *	among the traps that get them. That is what makes this safe to write with
 *	no stack and no saving: %g1 through %g7 here are a private bank, and
 *	nothing the interrupted code owns is at risk.
 *
 *	Do not add anything to this path that can fault. A miss taken while
 *	handling a miss nests to TL2, and a spill from a call would nest further;
 *	four levels in, the machine takes a watchdog reset with no diagnostic. The
 *	TSB and this code must be locked in the TLB for the same reason -- see the
 *	design note, section 2.6.
 */
.macro TLB_MISS_HANDLER mmu_asi, pointer_asi, data_in_asi
	/* The pointer the hardware formed from the faulting address, and the tag
	 * it wants compared. Both are reads from MMU alternate space; neither can
	 * miss. The tag target sits at offset zero in the MMU register block, so
	 * %g0 addresses it directly.
	 */
	ldxa	[%g0] \pointer_asi, %g2
	ldxa	[%g0] \mmu_asi, %g1

	/* The TSB line, tag and data together. A quad load is atomic against a
	 * writer updating the pair, which a tag load followed by a separate data
	 * load is not -- and the window between them is exactly where a stale
	 * pairing would be picked up. %g4 takes the tag, %g5 the data.
	 */
	ldda	[%g2] ASI_NUCLEUS_QUAD_LDD, %g4

	/* Valid is bit 63 of the data half, so the entry is invalid precisely
	 * when the data is non-negative. Checking this matters: a zeroed TSB line
	 * has a zero tag, which would otherwise match a faulting access to a low
	 * address and load a null translation.
	 *
	 * The comparison itself is a plain xor because sparc_tsb_insert() stores
	 * tags in exactly the form the hardware builds its tag target, and the
	 * kernel runs in context zero.
	 */
	brgez,pn %g5, 1f
	 xor	%g1, %g4, %g4
	brnz,pn	%g4, 1f
	 nop

	/* Hit. Storing the data half to the Data In register is the atomic TLB
	 * write of step 3; the hardware picks the entry to replace and honours the
	 * lock bit while doing so. retry then re-runs the access that faulted.
	 */
	stxa	%g5, [%g0] \data_in_asi
	membar	#Sync
	retry

1:
	/* Miss. The slow path resolves this from the authoritative translation
	 * map and refills the TSB. It does not exist yet, and cannot be reached
	 * while this table is uninstalled.
	 */
	ba,a	sparc_tsb_miss_trap
	 nop
.endm


FUNCTION(sparc_data_mmu_miss):
	TLB_MISS_HANDLER ASI_DMMU, ASI_DMMU_TSB_8KB_PTR, ASI_DTLB_DATA_IN


FUNCTION(sparc_instruction_mmu_miss):
	TLB_MISS_HANDLER ASI_IMMU, ASI_IMMU_TSB_8KB_PTR, ASI_ITLB_DATA_IN


/*	Placeholder for the TSB miss path.
 *
 *	Spins deliberately rather than returning. Returning would re-run the access
 *	that faulted, take the same miss, and spin just as hard but without saying
 *	where. A stopped machine at a known address is the more debuggable failure,
 *	and gdb can be attached to it -- which is how the early panic in this port
 *	was found.
 *
 *	Deliberately a local symbol. A branch to a .global symbol goes through a
 *	relocation even when the target sits in the same section, because the
 *	linker must allow for interposition. The kernel is linked as a shared
 *	object, so ld leaves an R_SPARC_WDISP22 for load time rather than
 *	resolving it, and the branch sits in the image with a displacement of
 *	zero -- a branch to itself. The miss path would spin in place instead of
 *	reaching the handler, and nothing would say so. Keeping the symbol local
 *	lets the assembler resolve the displacement outright.
 */
sparc_tsb_miss_trap:
	ba,a	sparc_tsb_miss_trap
	 nop


/*	Window spill and fill.
 *
 *	These are the busiest handlers in the system: a boot takes roughly ten
 *	thousand spills and as many fills, all currently serviced by the firmware.
 *
 *	Spill and fill traps are allotted four table entries each rather than one,
 *	because the trap type encodes the window number in its low two bits. That
 *	is 128 bytes, or thirty-two instructions, which is what makes it possible
 *	to do the whole job inline instead of branching somewhere with no stack to
 *	branch with.
 *
 *	On a spill the hardware has already rotated CWP to the window that needs
 *	saving, so %sp names that window's frame directly. A SPARC V9 frame keeps
 *	the register save area at %sp + BIAS: eight locals, then eight ins.
 *
 *	Derived from OpenBSD's sys/arch/sparc64/sparc64/locore.s, which is where
 *	the shape of these handlers and the clean-window sequence come from. See
 *	sparc-port/THIRD_PARTY.md.
 */

/* SPARC V9 stack bias: %sp holds the frame address minus 2047. */
#define BIAS	2047


/*	Save the current window to its stack frame.
 *
 *	"saved" tells the hardware the window is no longer live, which is what
 *	updates CANSAVE and CANRESTORE; "retry" then re-runs the save that trapped.
 */
.macro SPILL_KERNEL_WINDOW
	stx	%l0, [%sp + BIAS + (0 * 8)]
	stx	%l1, [%sp + BIAS + (1 * 8)]
	stx	%l2, [%sp + BIAS + (2 * 8)]
	stx	%l3, [%sp + BIAS + (3 * 8)]
	stx	%l4, [%sp + BIAS + (4 * 8)]
	stx	%l5, [%sp + BIAS + (5 * 8)]
	stx	%l6, [%sp + BIAS + (6 * 8)]
	stx	%l7, [%sp + BIAS + (7 * 8)]
	stx	%i0, [%sp + BIAS + (8 * 8)]
	stx	%i1, [%sp + BIAS + (9 * 8)]
	stx	%i2, [%sp + BIAS + (10 * 8)]
	stx	%i3, [%sp + BIAS + (11 * 8)]
	stx	%i4, [%sp + BIAS + (12 * 8)]
	stx	%i5, [%sp + BIAS + (13 * 8)]
	stx	%i6, [%sp + BIAS + (14 * 8)]
	stx	%i7, [%sp + BIAS + (15 * 8)]
	saved
	retry
.endm


/*	Restore the current window from its stack frame. */
.macro FILL_KERNEL_WINDOW
	ldx	[%sp + BIAS + (0 * 8)], %l0
	ldx	[%sp + BIAS + (1 * 8)], %l1
	ldx	[%sp + BIAS + (2 * 8)], %l2
	ldx	[%sp + BIAS + (3 * 8)], %l3
	ldx	[%sp + BIAS + (4 * 8)], %l4
	ldx	[%sp + BIAS + (5 * 8)], %l5
	ldx	[%sp + BIAS + (6 * 8)], %l6
	ldx	[%sp + BIAS + (7 * 8)], %l7
	ldx	[%sp + BIAS + (8 * 8)], %i0
	ldx	[%sp + BIAS + (9 * 8)], %i1
	ldx	[%sp + BIAS + (10 * 8)], %i2
	ldx	[%sp + BIAS + (11 * 8)], %i3
	ldx	[%sp + BIAS + (12 * 8)], %i4
	ldx	[%sp + BIAS + (13 * 8)], %i5
	ldx	[%sp + BIAS + (14 * 8)], %i6
	ldx	[%sp + BIAS + (15 * 8)], %i7
	restored
	retry
.endm


/*	Hand a freshly rotated window to code that may read it before writing.
 *
 *	This is the most frequent trap of all -- around fifty thousand per boot --
 *	because the kernel is constantly entering new windows. Zero the locals and
 *	the outs, bump CLEANWIN so the hardware knows one more window is safe, and
 *	retry.
 *
 *	%o7 is used as the scratch for CLEANWIN and then zeroed along with the
 *	rest, so nothing is left behind.
 */
.macro CLEAN_KERNEL_WINDOW
	clr	%l0
	mov	%l0, %l1
	mov	%l0, %l2
	rdpr	%cleanwin, %o7
	inc	%o7
	mov	%l0, %l3
	wrpr	%g0, %o7, %cleanwin
	mov	%l0, %l4
	mov	%l0, %l5
	mov	%l0, %l6
	mov	%l0, %l7
	mov	%l0, %o0
	mov	%l0, %o1
	mov	%l0, %o2
	mov	%l0, %o3
	mov	%l0, %o4
	mov	%l0, %o5
	mov	%l0, %o6
	mov	%l0, %o7
	retry
.endm


FUNCTION(sparc_spill_kernel):
	SPILL_KERNEL_WINDOW


FUNCTION(sparc_fill_kernel):
	FILL_KERNEL_WINDOW


FUNCTION(sparc_clean_window):
	CLEAN_KERNEL_WINDOW


/*	Everything the kernel does not handle yet.
 *
 *	The hard constraint is that this cannot fault. It runs at whatever trap
 *	level the machine reached, possibly inside another handler, with no
 *	guarantee that any particular stack is mapped -- and a fault here nests one
 *	level deeper. Four levels in, the CPU takes a watchdog reset and says
 *	nothing at all, which is the single worst outcome available.
 *
 *	So it touches no memory. The trap state is read with rdpr, which cannot
 *	fault, into the private global bank the hardware supplies on the way in --
 *	MMU globals for the MMU traps, the alternate set otherwise, but either way
 *	registers that belong to the handler and to nothing else. Then it stops.
 *
 *	Zero-filling unused table entries would have been the easy alternative and
 *	is a trap: zero is an illegal instruction, so an unhandled trap would take
 *	another trap immediately, and that is the watchdog reset again with the
 *	original cause thrown away.
 *
 *	Reading the result, in order of convenience:
 *
 *	  - QEMU's -d int trace prints the whole register file at every trap, so
 *	    the values are in the log without attaching anything.
 *	  - gdb: break in, and the registers below say what happened. The spin is
 *	    at a named symbol, so $pc identifies this handler immediately.
 *	  - On hardware with neither, the machine stops rather than resetting,
 *	    which at least distinguishes a trap from a hang.
 *
 *	Recording to memory as well would let a later boot recover this, but it
 *	needs a page locked in the TLB to be fault-proof, and that has to wait
 *	until the table is installed and the locking is in place.
 *
 *	    %g1  trap type          (%tt)
 *	    %g2  trapped pc         (%tpc)
 *	    %g3  trapped next pc    (%tnpc)
 *	    %g4  trap state         (%tstate)
 *	    %g5  trap level         (%tl)
 *
 *	%g6 and %g7 are left alone: they are reserved for system software, and a
 *	handler that stops the machine has no business disturbing more than it
 *	must.
 */
sparc_unhandled_trap:
	rdpr	%tt, %g1
	rdpr	%tpc, %g2
	rdpr	%tnpc, %g3
	rdpr	%tstate, %g4
	rdpr	%tl, %g5
sparc_unhandled_trap_stop:
	ba,a	sparc_unhandled_trap_stop
	 nop


/*	One trap table entry, for a trap with no handler.
 *
 *	Exactly eight instructions, because an ordinary entry is 32 bytes. The
 *	branch annuls its delay slot, so only the first instruction ever runs and
 *	the rest is padding that keeps the table's geometry exact.
 *
 *	The target is a local symbol on purpose. A branch to a global one goes
 *	through a relocation that the shared-object link defers to load time,
 *	leaving a displacement of zero -- a branch to itself. That failure is
 *	silent, and it has already happened once in this file.
 */
.macro TRAP_UNHANDLED
	ba,a	sparc_unhandled_trap
	 nop
	nop
	nop
	nop
	nop
	nop
	nop
.endm


/*	Eight entries' worth, for filling runs of unused trap types. */
.macro TRAP_UNHANDLED_8
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
.endm


/*	Table geometry is enforced with .org, not with an assembler .if.
 *
 *	The obvious check -- bracket an entry with two labels and .if their
 *	difference is not 32 -- does not work: gas evaluates .if during its first
 *	pass, before a forward label is known, so it fails on a correct entry. That
 *	was tried, and it failed on an entry measured at exactly 32 bytes.
 *
 *	When the table is built, each handler is followed by
 *
 *	    .org sparc_trap_table + (next_trap_type * 32)
 *
 *	which pads the gap and, because .org refuses to move backwards, fails the
 *	build outright if a handler ran past the space its trap type owns. That is
 *	the assertion worth having: overflowing an entry misaligns everything after
 *	it, so every later trap would vector into the middle of some other handler.
 */


/*	Groups of four entries.
 *
 *	The trap types that matter most are spaced four apart, so only the first of
 *	each group of four is ever vectored to and a handler may use all 128 bytes.
 *	Each group pads itself to that size with .org, which also asserts the fit:
 *	.org refuses to move backwards, so a handler that outgrows its group fails
 *	the build instead of silently displacing everything after it.
 *
 *	The padding is unreachable -- every handler ends in retry -- so what it
 *	contains does not matter.
 */
.macro TRAP_GROUP_CLEAN
8:	CLEAN_KERNEL_WINDOW
	.org 8b + 128
.endm

.macro TRAP_GROUP_SPILL
8:	SPILL_KERNEL_WINDOW
	.org 8b + 128
.endm

.macro TRAP_GROUP_FILL
8:	FILL_KERNEL_WINDOW
	.org 8b + 128
.endm

.macro TRAP_GROUP_DATA_MISS
8:	TLB_MISS_HANDLER ASI_DMMU, ASI_DMMU_TSB_8KB_PTR, ASI_DTLB_DATA_IN
	.org 8b + 128
.endm

.macro TRAP_GROUP_INSTRUCTION_MISS
8:	TLB_MISS_HANDLER ASI_IMMU, ASI_IMMU_TSB_8KB_PTR, ASI_ITLB_DATA_IN
	.org 8b + 128
.endm


/*	One half of the trap table: 512 entries of 32 bytes, so 16 KB.
 *
 *	The same layout serves TL = 0 and TL > 0. They could differ -- a spill taken
 *	inside a trap handler is a different situation from one taken in ordinary
 *	kernel code -- but while the port is kernel-only both are saving a kernel
 *	window to a kernel stack, so one layout does for both.
 *
 *	Likewise the _normal and _other variants of spill and fill share a handler.
 *	They separate when userspace arrives and a spill has to know whether the
 *	stack it is writing belongs to a user process.
 */
.macro TRAP_TABLE_HALF
	/* 0x000-0x023: reset, instruction access, illegal instruction, privileged
	 * action, floating point, tag overflow. Nothing handled yet, but every
	 * entry is filled: an unhandled trap must reach a handler that reports it,
	 * not run into blank space.
	 */
	.rept 0x024
	TRAP_UNHANDLED
	.endr

	/* 0x024-0x027: clean_window. The most frequent trap of all. */
	TRAP_GROUP_CLEAN

	/* 0x028-0x063: interrupt levels, and the traps for watchdog, alignment
	 * and the rest.
	 */
	.rept 0x03c
	TRAP_UNHANDLED
	.endr

	/* 0x064-0x06b: the two MMU misses, which are the point of all this. */
	TRAP_GROUP_INSTRUCTION_MISS
	TRAP_GROUP_DATA_MISS

	/* 0x06c-0x06f: fast_data_access_protection. A write to a read-only page,
	 * which wants the slow path rather than a refill, so it is left unhandled
	 * for now and will report itself if it happens.
	 */
	.rept 0x004
	TRAP_UNHANDLED
	.endr

	/* 0x070-0x07f */
	.rept 0x010
	TRAP_UNHANDLED
	.endr

	/* 0x080-0x09f: spill_0_normal through spill_7_normal.
	 * 0x0a0-0x0bf: spill_0_other through spill_7_other.
	 */
	.rept 8
	TRAP_GROUP_SPILL
	.endr
	.rept 8
	TRAP_GROUP_SPILL
	.endr

	/* 0x0c0-0x0df: fill_0_normal through fill_7_normal.
	 * 0x0e0-0x0ff: fill_0_other through fill_7_other.
	 */
	.rept 8
	TRAP_GROUP_FILL
	.endr
	.rept 8
	TRAP_GROUP_FILL
	.endr

	/* 0x100-0x1ff: the software traps, reached by the Tcc instructions. Haiku
	 * will want one of these for system calls eventually.
	 */
	.rept 0x100
	TRAP_UNHANDLED
	.endr
.endm


/*	The table.
 *
 *	32 KB and 32 KB aligned, because the hardware forms an entry address as
 *	%tba | (TL > 0 ? 0x4000 : 0) | (trap_type << 5) and simply ignores the low
 *	bits of %tba. A misaligned table would not be diagnosed; it would just
 *	vector every trap into the wrong place.
 *
 *	The two .org directives are assertions rather than padding: each half must
 *	come out at exactly 16 KB, and .org fails the build if a half has overrun.
 *	An assembler .if cannot be used for this -- gas evaluates .if before
 *	addresses are assigned, so it rejects even a correct table. That was tried
 *	twice.
 */
.align 32768
FUNCTION(sparc_trap_table):
	TRAP_TABLE_HALF
	.org sparc_trap_table + 0x4000
	TRAP_TABLE_HALF
	.org sparc_trap_table + 0x8000
FUNCTION(sparc_trap_table_end):
