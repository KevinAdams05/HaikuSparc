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
/*	PSTATE fields, from the SPARC V9 architecture manual. AG, MG and IG select
 *	which bank of global registers is visible; at most one may be set.
 */
#define PSTATE_AG			0x001
#define PSTATE_IE			0x002
#define PSTATE_MG			0x400
#define PSTATE_IG			0x800

/*	Offsets into struct sparc_trap_data, declared in
 *	headers/private/kernel/arch/sparc/arch_mmu.h.
 *
 *	The assembler cannot see a C++ declaration, so these are a second copy of
 *	numbers that already exist. Rather than trust the two to stay in step,
 *	sparc_trap_data_offsets() below hands this file's opinion back to C++, where
 *	sparc_verify_trap_globals() compares it against offsetof(). A disagreement
 *	would otherwise mean trap handlers quietly writing over each other's fields.
 */
#define TRAP_DATA_TSB_BASE		0x00
#define TRAP_DATA_PAGE_TABLE_ROOT	0x08
#define TRAP_DATA_MISS_COUNT		0x10
#define TRAP_DATA_MISS_TAG_TARGET	0x18
#define TRAP_DATA_MISS_TSB_POINTER	0x20
#define TRAP_DATA_MISS_TAG_ACCESS	0x28
#define TRAP_DATA_MISS_ENTRY		0x30
#define TRAP_DATA_MISS_TRAP_TYPE	0x38
#define TRAP_DATA_MISS_TPC		0x40
#define TRAP_DATA_MISS_TSTATE		0x48
#define TRAP_DATA_MISS_TL		0x50
#define TRAP_DATA_REPORT_HANDLER	0x58
#define TRAP_DATA_TRAP_KIND		0x60
#define TRAP_DATA_TRAP_CALL_SITE	0x68
#define TRAP_DATA_TRAP_RETURN		0x70
#define TRAP_DATA_WINDOW_STATE		0x78
#define TRAP_DATA_LOCALS		0x80
#define TRAP_DATA_OFFSET_COUNT		17

#define SPARC_TRAP_UNRESOLVED_MISS	0
#define SPARC_TRAP_UNHANDLED		1

/*	struct iframe, from arch_int.h. sparc_verify_iframe_layout() compares these
 *	against the structure they describe.
 */
#define IFRAME_TSTATE			0x00
#define IFRAME_TPC			0x08
#define IFRAME_TNPC			0x10
#define IFRAME_TT			0x18
#define IFRAME_Y			0x20
#define IFRAME_G1			0x28
#define IFRAME_G2			0x30
#define IFRAME_G3			0x38
#define IFRAME_G4			0x40
#define IFRAME_G5			0x48
#define IFRAME_G6			0x50
#define IFRAME_G7			0x58
#define IFRAME_PIL			0x60
#define IFRAME_SIZEOF			0x70

/*	The minimum SPARC V9 stack frame, from arch_thread_types.h: 128 bytes of
 *	register save area and 48 more for outgoing arguments.
 */
#define SPARC_MINIMUM_FRAME_SIZE	176

#define SPARC_GLOBALS_MMU		0

/*	Page table geometry, from arch_vm_translation_map.h. Three levels of 1024
 *	eight-byte entries, ten bits of virtual address each, covering VA<42:13>.
 */
#define PT_SEGMENT_SHIFT		33
#define PT_DIRECTORY_SHIFT		23
#define PT_TABLE_SHIFT			13
#define PT_INDEX_MASK			1023
#define PT_VA_HOLE_SHIFT		43

#define ASI_PHYS_USE_EC			0x14

// TTE_SOFT_ACCESSED, from arch_mmu.h.
#define TTE_SOFT_ACCESSED_BIT		0x200

// The Tag Access register's offset in the MMU register block.
#define MMU_TLB_TAG_ACCESS		0x30

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
.macro TLB_MISS_HANDLER mmu_asi, pointer_asi, data_in_asi, slow_path
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
	 * kernel runs in context zero. The difference goes to %g6 rather than over
	 * the top of %g4, so that the slow path can report the tag it really found
	 * instead of a value that has already been folded against the one wanted.
	 */
	brgez,pn %g5, 1f
	 xor	%g1, %g4, %g6
	brnz,pn	%g6, 1f
	 nop

	/* Hit. Storing the data half to the Data In register is the atomic TLB
	 * write of step 3; the hardware picks the entry to replace and honours the
	 * lock bit while doing so. retry then re-runs the access that faulted.
	 */
	stxa	%g5, [%g0] \data_in_asi
	membar	#Sync
	retry

1:
	/* Miss, or a line that belongs to a colliding address. The slow path walks
	 * the page table, which is the record this is only a cache of.
	 */
	ba,a	\slow_path
	 nop
.endm


/*	Everything from here to sparc_trap_handlers_end is reachable with a trap in
 *	progress, so every page of it has to be locked in the instruction TLB before
 *	the table is installed. An instruction fetch that missed here would be a miss
 *	taken inside the miss handler, and the handler needed to service it is the
 *	code that could not be fetched.
 *
 *	The bank helpers further down are deliberately outside this range: they are
 *	called from C at TL=0, where an ordinary miss is fine.
 */
FUNCTION(sparc_trap_handlers_start):

FUNCTION(sparc_data_mmu_miss):
	TLB_MISS_HANDLER ASI_DMMU, ASI_DMMU_TSB_8KB_PTR, ASI_DTLB_DATA_IN, \
		sparc_dmmu_miss_slow


FUNCTION(sparc_instruction_mmu_miss):
	TLB_MISS_HANDLER ASI_IMMU, ASI_IMMU_TSB_8KB_PTR, ASI_ITLB_DATA_IN, \
		sparc_immu_miss_slow


/*	The TSB miss path: walk the page table.
 *
 *	Reached when the TSB line for the faulting address does not describe it --
 *	either nothing has been cached for it yet, or the line currently belongs to a
 *	different address that indexes to the same place. The second case is not an
 *	edge case. The TSB is direct-mapped on VA<25:13>, so any two regions more
 *	than 64 MB apart alias completely, and the loader's own allocations alias the
 *	kernel image exactly. That is why there is a page table underneath, and why
 *	this path is ordinary rather than exceptional.
 *
 *	The walk needs nothing mapped. Interior entries hold physical addresses and
 *	are read through ASI_PHYS_USE_EC, so no level of the table needs a TLB entry
 *	and none of them can fault -- which is what makes it safe to run at TL=1 on
 *	no stack, where a fault would nest towards a watchdog reset.
 *
 *	The root arrives in %g3, put there at cutover in both alternate global banks.
 *	A register rather than a memory location, for the same reason %g7 is one:
 *	finding out where the table is must not itself require a translation.
 *
 *	Registers: %g1 holds the tag target from the fast path, %g2 the TSB pointer
 *	the hardware formed, %g3 the root; all three are kept. %g4, %g5 and %g6 are
 *	scratch.
 *
 *	Derived from OpenBSD's PTE_GET in sys/arch/sparc64/sparc64/locore.s -- the
 *	same geometry and the same order of operations. See sparc-port/THIRD_PARTY.md.
 */
.macro TLB_MISS_SLOW_PATH mmu_asi, data_in_asi
	/* The faulting address, from the Tag Access register: VA<63:13> in the top
	 * bits, the context in the low thirteen. Those context bits sit below every
	 * index this walk takes and below the hole check, so there is nothing to
	 * mask off.
	 */
	mov	MMU_TLB_TAG_ACCESS, %g6
	ldxa	[%g6] \mmu_asi, %g6

	/* The sun4u address hole: VA<63:43> must be all ones or all zeroes. An
	 * arithmetic shift leaves -1 or 0, incrementing makes those 0 or 1, and one
	 * unsigned comparison rejects everything else. An address in the hole has no
	 * entry to find and never will have one.
	 */
	srax	%g6, PT_VA_HOLE_SHIFT, %g4
	inc	%g4
	cmp	%g4, 1
	bgu,pn	%xcc, 1f
	 nop

	/* Level 1: the segment table, indexed by VA<42:33>. */
	srlx	%g6, PT_SEGMENT_SHIFT, %g4
	and	%g4, PT_INDEX_MASK, %g4
	sllx	%g4, 3, %g4
	add	%g3, %g4, %g4
	ldxa	[%g4] ASI_PHYS_USE_EC, %g4
	brz,pn	%g4, 1f

	/* Level 2: the directory, indexed by VA<32:23>. */
	 srlx	%g6, PT_DIRECTORY_SHIFT, %g5
	and	%g5, PT_INDEX_MASK, %g5
	sllx	%g5, 3, %g5
	add	%g4, %g5, %g5
	ldxa	[%g5] ASI_PHYS_USE_EC, %g5
	brz,pn	%g5, 1f

	/* Level 3: the leaf, indexed by VA<22:13>. Its entries are TTE data halves
	 * already, so what comes out of here is what goes into the TLB.
	 */
	 srlx	%g6, PT_TABLE_SHIFT, %g4
	and	%g4, PT_INDEX_MASK, %g4
	sllx	%g4, 3, %g4
	add	%g5, %g4, %g4
	ldxa	[%g4] ASI_PHYS_USE_EC, %g5

	/* Valid is bit 63, so an invalid entry is a non-negative one. */
	brgez,pn %g5, 1f
	 nop

	/* Record that the page has been touched. The hardware tracks nothing of the
	 * sort, so this bit being set here is the only reason the VM can ever see a
	 * page as accessed. A plain store rather than a compare-and-swap, because
	 * there is one CPU and no other writer.
	 */
	or	%g5, TTE_SOFT_ACCESSED_BIT, %g5
	stxa	%g5, [%g4] ASI_PHYS_USE_EC

	/* Refill the TSB line, so the next miss on this address takes the fast path.
	 * Data before tag: a reader seeing the new tag with the old data would accept
	 * a translation that was never in the table, whereas the reverse merely
	 * misses again and comes back here.
	 */
	stx	%g5, [%g2 + 8]
	stx	%g1, [%g2]

	/* And into the TLB. The same three instructions the fast path ends with,
	 * because the leaf entry was already in TTE form.
	 */
	stxa	%g5, [%g0] \data_in_asi
	membar	#Sync
	retry

1:
	/* Nothing mapped for this address. Record everything about it first: every
	 * store goes to the trap data block, whose page is locked in the TLB for
	 * exactly this reason.
	 */
	stx	%g1, [%g7 + TRAP_DATA_MISS_TAG_TARGET]
	stx	%g2, [%g7 + TRAP_DATA_MISS_TSB_POINTER]
	stx	%g6, [%g7 + TRAP_DATA_MISS_TAG_ACCESS]
	stx	%g5, [%g7 + TRAP_DATA_MISS_ENTRY]

	rdpr	%tt, %g6
	stx	%g6, [%g7 + TRAP_DATA_MISS_TRAP_TYPE]
	rdpr	%tpc, %g6
	stx	%g6, [%g7 + TRAP_DATA_MISS_TPC]
	rdpr	%tstate, %g6
	stx	%g6, [%g7 + TRAP_DATA_MISS_TSTATE]

	/* Counted last, so a non-zero count means the record above it is complete
	 * rather than half written.
	 */
	stx	%o7, [%g7 + TRAP_DATA_TRAP_CALL_SITE]
	stx	%i7, [%g7 + TRAP_DATA_TRAP_RETURN]

	mov	SPARC_TRAP_UNRESOLVED_MISS, %g5
	stx	%g5, [%g7 + TRAP_DATA_TRAP_KIND]

	ldx	[%g7 + TRAP_DATA_MISS_COUNT], %g5
	add	%g5, 1, %g5
	stx	%g5, [%g7 + TRAP_DATA_MISS_COUNT]

	/* Now get out of trap context and say so.
	 *
	 * Stopping here would be correct but useless: the machine would sit at a
	 * known address with a complete record in memory and no way to print it, and
	 * every unmapped address for the rest of the port would look like an
	 * unexplained hang. Instead, return to TL=0 at a C function that panics with
	 * what was just recorded.
	 *
	 * done restores TSTATE -- the normal global bank, the interrupted CWP and its
	 * stack -- and sets the PC from TNPC, so overwriting TNPC first is what
	 * redirects it. The instruction that faulted is not re-run, which would
	 * matter if this were recoverable; it is not.
	 *
	 * Only from TL=1. Deeper than that, the level below is one of our own
	 * handlers rather than ordinary kernel code, and returning into it with
	 * different registers than it left would produce a second failure that told
	 * us nothing about the first.
	 */
	rdpr	%tl, %g6
	stx	%g6, [%g7 + TRAP_DATA_MISS_TL]
	cmp	%g6, 1
	bne,pn	%xcc, 2f
	 ldx	[%g7 + TRAP_DATA_REPORT_HANDLER], %g6

	brz,pn	%g6, 2f
	 nop
	wrpr	%g6, 0, %tnpc
	done

2:	ba,a	2b
	 nop
.endm


/*	Deliberately local symbols. A branch to a .global symbol goes through a
 *	relocation even when the target sits in the same section, because the linker
 *	must allow for interposition. The kernel is linked as a shared object, so ld
 *	leaves an R_SPARC_WDISP22 for load time rather than resolving it, and the
 *	branch sits in the image with a displacement of zero -- a branch to itself,
 *	silently. Keeping these local lets the assembler resolve the displacement
 *	outright.
 */
sparc_dmmu_miss_slow:
	TLB_MISS_SLOW_PATH ASI_DMMU, ASI_DTLB_DATA_IN


sparc_immu_miss_slow:
	TLB_MISS_SLOW_PATH ASI_IMMU, ASI_ITLB_DATA_IN


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
 *	It records what happened into the trap data block and then leaves trap
 *	context to report it, exactly as the unresolved-miss path does. Spinning at a
 *	named symbol was the previous behaviour and it is not good enough: every trap
 *	the kernel does not yet handle -- a protection fault, an alignment fault, an
 *	access exception, a Tcc -- looked identical to a hang, with the evidence
 *	sitting in registers that nothing could print.
 *
 *	The registers are read before anything else, because %tt and friends are
 *	stacked per trap level and a nested trap would push them out of reach.
 *
 *	Only from TL=1 can it return. Deeper than that the level below is one of our
 *	own handlers rather than ordinary kernel code, so it stops instead -- at a
 *	named symbol, which is still the most a debugger needs.
 */
sparc_unhandled_trap:
	rdpr	%tt, %g1
	stx	%g1, [%g7 + TRAP_DATA_MISS_TRAP_TYPE]
	rdpr	%tpc, %g1
	stx	%g1, [%g7 + TRAP_DATA_MISS_TPC]
	rdpr	%tstate, %g1
	stx	%g1, [%g7 + TRAP_DATA_MISS_TSTATE]

	/* The faulting address for the traps that have one. SFAR is the D-MMU's
	 * Synchronous Fault Address register at offset 0x20; for a trap with no
	 * address of its own it simply holds whatever the last one left, which the
	 * reporter accounts for by naming the trap type alongside it.
	 */
	mov	0x20, %g1
	ldxa	[%g1] ASI_DMMU, %g1
	stx	%g1, [%g7 + TRAP_DATA_MISS_TAG_ACCESS]

	/* A trap leaves CWP alone, so %o7 and %i7 still belong to the code that
	 * faulted: %o7 is where its last call came from, %i7 where its own frame
	 * returns to. For a jump through a null function pointer that pair is the
	 * whole diagnosis, and neither is recoverable once the window is reused.
	 */
	stx	%o7, [%g7 + TRAP_DATA_TRAP_CALL_SITE]
	stx	%i7, [%g7 + TRAP_DATA_TRAP_RETURN]

	/* The trapped window's own locals. A trap does not rotate CWP, so these are
	 * the interrupted code's %l registers, and a corrupted one is otherwise
	 * invisible: the report can name the faulting instruction but not the
	 * register it read through.
	 */
	stx	%l0, [%g7 + TRAP_DATA_LOCALS + (0 * 8)]
	stx	%l1, [%g7 + TRAP_DATA_LOCALS + (1 * 8)]
	stx	%l2, [%g7 + TRAP_DATA_LOCALS + (2 * 8)]
	stx	%l3, [%g7 + TRAP_DATA_LOCALS + (3 * 8)]
	stx	%l4, [%g7 + TRAP_DATA_LOCALS + (4 * 8)]
	stx	%l5, [%g7 + TRAP_DATA_LOCALS + (5 * 8)]
	stx	%l6, [%g7 + TRAP_DATA_LOCALS + (6 * 8)]
	stx	%l7, [%g7 + TRAP_DATA_LOCALS + (7 * 8)]

	/* And the window-state registers, packed one byte each: a spill or fill
	 * handler that miscounts shows up here and nowhere else.
	 */
	rdpr	%cwp, %g1
	rdpr	%cansave, %g2
	sllx	%g2, 8, %g2
	or	%g1, %g2, %g1
	rdpr	%canrestore, %g2
	sllx	%g2, 16, %g2
	or	%g1, %g2, %g1
	rdpr	%cleanwin, %g2
	sllx	%g2, 24, %g2
	or	%g1, %g2, %g1
	rdpr	%otherwin, %g2
	sllx	%g2, 32, %g2
	or	%g1, %g2, %g1
	stx	%g1, [%g7 + TRAP_DATA_WINDOW_STATE]

	mov	SPARC_TRAP_UNHANDLED, %g1
	stx	%g1, [%g7 + TRAP_DATA_TRAP_KIND]

	rdpr	%tl, %g1
	stx	%g1, [%g7 + TRAP_DATA_MISS_TL]
	cmp	%g1, 1
	bne,pn	%xcc, sparc_unhandled_trap_stop
	 ldx	[%g7 + TRAP_DATA_REPORT_HANDLER], %g1

	brz,pn	%g1, sparc_unhandled_trap_stop
	 nop
	wrpr	%g1, 0, %tnpc
	done

sparc_unhandled_trap_stop:
	ba,a	sparc_unhandled_trap_stop
	 nop


/*	Interrupt entry.
 *
 *	Entered at trap level 1 with the alternate globals selected and PSTATE.IE
 *	already cleared by the hardware, so nothing can interrupt this until it
 *	returns. The interrupted window is untouched -- a trap does not rotate CWP --
 *	which is what makes the shape below work: `save` here takes a window of our
 *	own and makes the interrupted one the previous window, so `restore` on the
 *	way out brings it back, filled from the stack if a spill got there first.
 *
 *	The order is forced by two facts. The trap registers only exist at trap
 *	level 1, so they have to be read before dropping to zero; and C has to run at
 *	trap level zero, because a window spill or a TLB miss inside it must be
 *	handled by the ordinary table rather than nesting towards MAXTL.
 *
 *	Everything between the two `wrpr %tl` writes therefore lives in this window's
 *	locals, which survive both because they are window registers.
 *
 *	Deliberately a local symbol, for the same reason the miss slow paths are: the
 *	table branches here with `ba,a`, and a branch to a .global symbol goes through
 *	a load-time relocation that leaves a displacement of zero -- a branch to
 *	itself, silently.
 */
sparc_interrupt_entry:
	save	%sp, -(SPARC_MINIMUM_FRAME_SIZE + IFRAME_SIZEOF), %sp

	/* The trap registers first, while they still exist. */
	rdpr	%tstate, %l0
	rdpr	%tpc, %l1
	rdpr	%tnpc, %l2
	rdpr	%tt, %l3
	rd	%y, %l4

	/* Back to the normal global bank. Two reasons: the interrupted code's
	 * globals are in it and have to be saved, and C expects the thread pointer
	 * in %g7, which is the normal bank's.
	 *
	 * All three selector bits, not just AG. UltraSPARC-IIi adds interrupt and
	 * MMU global sets to SPARC V9's two, selected by PSTATE.IG at bit 11 and
	 * PSTATE.MG at bit 10 alongside AG at bit 0 (TABLE 14-12, printed p.201).
	 * Clearing only AG would leave a different bank selected than intended,
	 * which is worse than not switching at all: the handler would save one
	 * bank's globals and restore them into another.
	 */
	rdpr	%pstate, %l5
	andn	%l5, PSTATE_AG | PSTATE_MG | PSTATE_IG, %l6
	wrpr	%l6, 0, %pstate

	/* The frame sits above this window's own save area. */
	add	%sp, SPARC_MINIMUM_FRAME_SIZE + BIAS, %l7

	stx	%g1, [%l7 + IFRAME_G1]
	stx	%g2, [%l7 + IFRAME_G2]
	stx	%g3, [%l7 + IFRAME_G3]
	stx	%g4, [%l7 + IFRAME_G4]
	stx	%g5, [%l7 + IFRAME_G5]
	stx	%g6, [%l7 + IFRAME_G6]
	stx	%g7, [%l7 + IFRAME_G7]

	/* Raise the interrupt level so nothing can nest, and remember what it was.
	 *
	 * PSTATE.IE is already clear, so this looks redundant -- and it is not.
	 * Rescheduling from this handler switches to another thread and comes back,
	 * and the interrupt-enable bit is not something a switch can be relied upon
	 * to leave alone. An interrupt taken while this handler still owes a trap
	 * return writes its own trap level over ours, and the two returns then drive
	 * the trap level below zero. Blocking at the level rather than at the enable
	 * bit does not depend on anyone else's discipline.
	 *
	 * 15 rather than 14: PIL blocks interrupts at or below its own level, and
	 * the tick arrives at 14.
	 */
	rdpr	%pil, %l5
	stx	%l5, [%l7 + IFRAME_PIL]
	wrpr	%g0, 15, %pil

	stx	%l0, [%l7 + IFRAME_TSTATE]
	stx	%l1, [%l7 + IFRAME_TPC]
	stx	%l2, [%l7 + IFRAME_TNPC]
	stx	%l3, [%l7 + IFRAME_TT]
	stx	%l4, [%l7 + IFRAME_Y]

	/* Down to trap level zero, so the handler runs like ordinary kernel code.
	 * Interrupts stay disabled -- PSTATE.IE is still clear -- so nothing nests.
	 */
	wrpr	%g0, 0, %tl

	call	sparc_interrupt
	 mov	%l7, %o0

	/* Back up to trap level 1: `retry` is meaningless below it, and %tpc and
	 * friends do not exist to be written.
	 */
	wrpr	%g0, 1, %tl

	/* Everything restored from this window's locals and the frame, because
	 * `restore` below takes both away.
	 */
	ldx	[%l7 + IFRAME_G1], %g1
	ldx	[%l7 + IFRAME_G2], %g2
	ldx	[%l7 + IFRAME_G3], %g3
	ldx	[%l7 + IFRAME_G4], %g4
	ldx	[%l7 + IFRAME_G5], %g5
	ldx	[%l7 + IFRAME_G6], %g6
	ldx	[%l7 + IFRAME_G7], %g7

	ldx	[%l7 + IFRAME_PIL], %l6
	wrpr	%l6, 0, %pil

	wr	%l4, 0, %y

	/* Point the saved CWP at the window `restore` is about to land on, rather
	 * than at the one the trap was taken in.
	 *
	 * `retry` sets CWP from TSTATE, and TSTATE holds the *numeric* window index
	 * from the moment of the trap. That is the same window only as long as
	 * nothing renumbers the register file in between -- and rescheduling from
	 * this handler does exactly that: the switch flushes every window, the
	 * thread runs somewhere else, and when it comes back its frames have been
	 * filled into whichever windows were free. The handler is then at a
	 * different CWP than it started at, and restoring the recorded one lands on
	 * a window holding somebody else's registers.
	 *
	 * That was the whole of the preemption bug. It showed up as faults in
	 * ordinary kernel code with %i7 still pointing into this handler's caller --
	 * code running in a window that was not the one it thought.
	 *
	 * Computed before the restore, because afterwards these locals are gone.
	 * NWINDOWS is 8, so the arithmetic wraps with a mask of 7; CWP is TSTATE
	 * bits 4:0.
	 */
	rdpr	%cwp, %l6
	sub	%l6, 1, %l6
	and	%l6, 7, %l6
	andn	%l0, 0x1f, %l0
	or	%l0, %l6, %l0

	wrpr	%l0, 0, %tstate
	wrpr	%l1, 0, %tpc
	wrpr	%l2, 0, %tnpc

	/* `restore` may fill from the stack, which traps to level 2 and comes back;
	 * `retry` then re-executes the instruction the interrupt landed on, with
	 * PSTATE -- including IE -- and CWP restored from %tstate.
	 */
	restore
	retry


/*	Report this file's idea of the interrupt frame layout, for C++ to check.
 *
 *	void sparc_iframe_offsets(uint64 *out);   // fourteen entries
 */
FUNCTION(sparc_iframe_offsets):
	mov	IFRAME_TSTATE, %o1
	stx	%o1, [%o0 + (0 * 8)]
	mov	IFRAME_TPC, %o1
	stx	%o1, [%o0 + (1 * 8)]
	mov	IFRAME_TNPC, %o1
	stx	%o1, [%o0 + (2 * 8)]
	mov	IFRAME_TT, %o1
	stx	%o1, [%o0 + (3 * 8)]
	mov	IFRAME_Y, %o1
	stx	%o1, [%o0 + (4 * 8)]
	mov	IFRAME_G1, %o1
	stx	%o1, [%o0 + (5 * 8)]
	mov	IFRAME_G2, %o1
	stx	%o1, [%o0 + (6 * 8)]
	mov	IFRAME_G3, %o1
	stx	%o1, [%o0 + (7 * 8)]
	mov	IFRAME_G4, %o1
	stx	%o1, [%o0 + (8 * 8)]
	mov	IFRAME_G5, %o1
	stx	%o1, [%o0 + (9 * 8)]
	mov	IFRAME_G6, %o1
	stx	%o1, [%o0 + (10 * 8)]
	mov	IFRAME_G7, %o1
	stx	%o1, [%o0 + (11 * 8)]
	mov	IFRAME_PIL, %o1
	stx	%o1, [%o0 + (12 * 8)]
	mov	IFRAME_SIZEOF, %o1
	stx	%o1, [%o0 + (13 * 8)]
	retl
	 nop


FUNCTION(sparc_trap_handlers_end):


/*	Point the trap handlers at their per-CPU data.
 *
 *	%g7 has to be set in two banks, because the hardware chooses the bank by
 *	trap type: the MMU miss traps get the MMU globals, most others get the
 *	alternate globals (UltraSPARC-IIi manual, section 15.3.2). Selecting a bank
 *	means writing PSTATE, and the out registers are not part of any global bank,
 *	so they are what carries the value across the switch.
 *
 *	Interrupts come off for the duration. An interrupt taken while a non-normal
 *	bank is selected would run its handler against these globals and clobber
 *	them, and the window is only a handful of instructions, so closing it is
 *	cheaper than reasoning about it.
 *
 *	%g3 takes the page table root, physical, for the same reason: the slow path
 *	walks the table with physical loads, and finding out where the table is must
 *	not itself require a translation.
 *
 *	void sparc_set_trap_globals(struct sparc_trap_data *data,
 *		phys_addr_t pageTableRoot);
 */
FUNCTION(sparc_set_trap_globals):
	rdpr	%pstate, %o2
	andn	%o2, PSTATE_IE, %o3

	or	%o3, PSTATE_MG, %o4
	wrpr	%o4, 0, %pstate
	mov	%o0, %g7
	mov	%o1, %g3

	/* Straight from the MMU bank to the alternate one: the single write clears
	 * MG and sets AG together, so no third bank is ever selected.
	 */
	or	%o3, PSTATE_AG, %o4
	wrpr	%o4, 0, %pstate
	mov	%o0, %g7
	mov	%o1, %g3

	/* Restores the normal bank and the caller's interrupt state at once. */
	wrpr	%o2, 0, %pstate
	retl
	 nop


/*	Read %g7 back out of one of the alternate banks, so the write above can be
 *	checked rather than assumed.
 *
 *	uint64 sparc_read_trap_globals(int bank);
 */
/*	Report this file's idea of where each trap data field lives, so C++ can check
 *	it against the structure the fields were declared in.
 *
 *	void sparc_trap_data_offsets(uint64 *out);   // TRAP_DATA_OFFSET_COUNT entries
 */
FUNCTION(sparc_trap_data_offsets):
	mov	TRAP_DATA_TSB_BASE, %o1
	stx	%o1, [%o0 + (0 * 8)]
	mov	TRAP_DATA_PAGE_TABLE_ROOT, %o1
	stx	%o1, [%o0 + (1 * 8)]
	mov	TRAP_DATA_MISS_COUNT, %o1
	stx	%o1, [%o0 + (2 * 8)]
	mov	TRAP_DATA_MISS_TAG_TARGET, %o1
	stx	%o1, [%o0 + (3 * 8)]
	mov	TRAP_DATA_MISS_TSB_POINTER, %o1
	stx	%o1, [%o0 + (4 * 8)]
	mov	TRAP_DATA_MISS_TAG_ACCESS, %o1
	stx	%o1, [%o0 + (5 * 8)]
	mov	TRAP_DATA_MISS_ENTRY, %o1
	stx	%o1, [%o0 + (6 * 8)]
	mov	TRAP_DATA_MISS_TRAP_TYPE, %o1
	stx	%o1, [%o0 + (7 * 8)]
	mov	TRAP_DATA_MISS_TPC, %o1
	stx	%o1, [%o0 + (8 * 8)]
	mov	TRAP_DATA_MISS_TSTATE, %o1
	stx	%o1, [%o0 + (9 * 8)]
	mov	TRAP_DATA_MISS_TL, %o1
	stx	%o1, [%o0 + (10 * 8)]
	mov	TRAP_DATA_REPORT_HANDLER, %o1
	stx	%o1, [%o0 + (11 * 8)]
	mov	TRAP_DATA_TRAP_KIND, %o1
	stx	%o1, [%o0 + (12 * 8)]
	mov	TRAP_DATA_TRAP_CALL_SITE, %o1
	stx	%o1, [%o0 + (13 * 8)]
	mov	TRAP_DATA_TRAP_RETURN, %o1
	stx	%o1, [%o0 + (14 * 8)]
	mov	TRAP_DATA_WINDOW_STATE, %o1
	stx	%o1, [%o0 + (15 * 8)]
	mov	TRAP_DATA_LOCALS, %o1
	stx	%o1, [%o0 + (16 * 8)]
	retl
	 nop


FUNCTION(sparc_read_trap_globals):
	rdpr	%pstate, %o1
	andn	%o1, PSTATE_IE, %o2

	cmp	%o0, SPARC_GLOBALS_MMU
	be,pt	%icc, 1f
	 or	%o2, PSTATE_AG, %o3
	ba,pt	%icc, 2f
	 nop
1:	or	%o2, PSTATE_MG, %o3

2:	wrpr	%o3, 0, %pstate
	mov	%g7, %o0
	wrpr	%o1, 0, %pstate
	retl
	 nop


/*	The same, for the page table root in %g3.
 *
 *	uint64 sparc_read_trap_page_table(int bank);
 */
FUNCTION(sparc_read_trap_page_table):
	rdpr	%pstate, %o1
	andn	%o1, PSTATE_IE, %o2

	cmp	%o0, SPARC_GLOBALS_MMU
	be,pt	%icc, 1f
	 or	%o2, PSTATE_AG, %o3
	ba,pt	%icc, 2f
	 nop
1:	or	%o2, PSTATE_MG, %o3

2:	wrpr	%o3, 0, %pstate
	mov	%g3, %o0
	wrpr	%o1, 0, %pstate
	retl
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
8:	TLB_MISS_HANDLER ASI_DMMU, ASI_DMMU_TSB_8KB_PTR, ASI_DTLB_DATA_IN, \
		sparc_dmmu_miss_slow
	.org 8b + 128
.endm

.macro TRAP_GROUP_INSTRUCTION_MISS
8:	TLB_MISS_HANDLER ASI_IMMU, ASI_IMMU_TSB_8KB_PTR, ASI_ITLB_DATA_IN, \
		sparc_immu_miss_slow
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

	/* 0x028-0x04d: interrupt levels 1 to 13, plus watchdog, alignment and the
	 * rest. None handled yet.
	 */
	.rept 0x026
	TRAP_UNHANDLED
	.endr

	/* 0x04e: interrupt_level_14, which is where the %TICK comparison lands --
	 * one entry, not four, because the interrupt levels are consecutive rather
	 * than spaced apart the way the busier traps are.
	 */
8:	ba,a	sparc_interrupt_entry
	 nop
	.org 8b + 32

	/* 0x04f-0x063: interrupt level 15 and the rest. */
	.rept 0x015
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
