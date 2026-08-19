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
