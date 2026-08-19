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
	 * miss.
	 */
	ldxa	[%g0] \pointer_asi, %g1
	mov	MMU_TAG_TARGET, %g2
	ldxa	[%g2] \mmu_asi, %g2

	/* The TSB line: tag first, then data. A single quad load would be atomic
	 * against a concurrent writer, which will matter once there is more than
	 * one CPU; there is not yet, and correctness here is easier to see.
	 */
	ldx	[%g1], %g4
	ldx	[%g1 + 8], %g5

	/* Hit detection. Stored tags carry the Global bit, which the tag target
	 * does not, so xor and then discard bit 63 by shifting it out. Anything
	 * left set means some other bit differed, and it is a miss.
	 */
	xor	%g4, %g2, %g4
	sllx	%g4, 1, %g4
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
