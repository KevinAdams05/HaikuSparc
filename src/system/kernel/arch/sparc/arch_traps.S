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

#include <arch_asm_defs.h>
#include <arch_syscall_defs.h>

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
#define TRAP_DATA_USER_PAGE_TABLE	0xc0
#define TRAP_DATA_KERNEL_STACK		0xc8
#define TRAP_DATA_WINDOW_SAVE		0xd0
#define TRAP_DATA_CURRENT_THREAD	0xd8
#define TRAP_DATA_USER_SPILL_COUNT	0xe0
#define TRAP_DATA_OTHER_SPILL_COUNT	0xe8
#define TRAP_DATA_OTHER_FILL_COUNT	0xf0
#define TRAP_DATA_WINFIXUP_ADDRESS	0xf8
#define TRAP_DATA_WINFIXUP_TRAP		0x100
#define TRAP_DATA_WINFIXUP_COUNT	0x108
#define TRAP_DATA_FAULT_TAG_ACCESS	0x110
#define TRAP_DATA_OFFSET_COUNT		21

/*	Geometry of struct sparc_window_save, from arch_thread_types.h. The count is
 *	the first word; slot N starts at base + (N + 1) * 256, so the header occupies
 *	one slot's worth and the handler indexes with a shift. Checked against the
 *	header by sparc_verify_mmu_defines().
 */
#define WINDOW_SAVE_SLOT_SHIFT		8
#define WINDOW_SAVE_SLOTS		8
#define WINDOW_SAVE_STACK_POINTER	128

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
#define IFRAME_SFSR			0x68
#define IFRAME_SFAR			0x70
#define IFRAME_TAG_ACCESS		0x78
#define IFRAME_OUT			0x80
#define IFRAME_SIZEOF			0xe0

// The data MMU's fault status and address registers.
#define MMU_SFSR			0x18
#define MMU_SFAR			0x20

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

/*	Primary address space, but with the *user's* privilege.
 *
 *	What a spill of a user window to the user's own stack has to use. A plain
 *	store from a trap handler carries kernel privilege and would write a page the
 *	user cannot, which is the user's own window data going somewhere the user was
 *	not allowed to put it -- and it hides a bug rather than reporting one.
 */
#define ASI_AS_IF_USER_PRIMARY		0x10

/*	WSTATE selects *which* spill and fill handler a trap uses: the vector is
 *	0x80 + 4 * WSTATE.NORMAL when OTHERWIN is zero, and 0xa0 + 4 * WSTATE.OTHER
 *	when it is not. Bits 2:0 are NORMAL, bits 5:3 OTHER.
 *
 *	So the kernel runs with NORMAL = 0, group zero, the handler that stores with
 *	kernel privilege to a kernel stack; and userspace runs with NORMAL = 1, group
 *	one, the handler that stores through ASI_AS_IF_USER_PRIMARY. OTHER stays 0 in
 *	both, because the cross-privilege case has one answer.
 */
// WSTATE_KERNEL and WSTATE_USER come from <arch_asm_defs.h>, so the trap entry
// and sparc_enter_userspace() cannot disagree about them.

// TTE_SOFT_ACCESSED, from arch_mmu.h.
#define TTE_SOFT_ACCESSED_BIT		0x200

/*	TSB tag geometry, also from arch_mmu.h. The context field starts at bit 48,
 *	and the VA_tag field holds VA<63:22> at bits 41:0 -- so tag bit 9 is VA<31>,
 *	which is the one bit that separates a kernel address from a user one, because
 *	KERNEL_BASE is 0x80000000 and the kernel owns everything above it.
 *
 *	Repeated here rather than included because arch_mmu.h is a C++ header. Both
 *	values are checked against it at init by sparc_verify_mmu_defines(), which
 *	derives the second from KERNEL_BASE rather than trusting either copy.
 */
// TSTATE_PRIV, from arch_int.h: PSTATE occupies TSTATE bits 20:8, so PRIV's
// 0x004 lands at 0x400.
#define TSTATE_PRIV			0x400

#define TTE_TAG_CONTEXT_SHIFT		48
#define TSB_TAG_VA_SHIFT		22
#define TSB_TAG_KERNEL_BIT		9

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

	/* Mask the context off a kernel address, because a kernel page's tag is
	 * stored with context zero while the tag target carries whatever context is
	 * loaded -- and once a user team is running, that is not zero.
	 *
	 * The Global bit in the TTE makes the *hardware* ignore the context when it
	 * matches a TLB entry. It buys nothing here: this comparison is software,
	 * and the tag target reads its own G bit as zero because the hardware cannot
	 * know whether a page is global before finding it. So without this, every
	 * kernel address would miss the fast path for as long as a user context is
	 * loaded -- correct, and slow enough to be the whole system's cost.
	 *
	 * Which kind of address it is comes out of %g1 itself, with no memory
	 * access and no branch. The tag target holds VA<63:22> at bits 41:0, and the
	 * kernel owns everything from KERNEL_BASE upwards with KERNEL_BASE a power
	 * of two, so a single bit decides: VA<31>, which is tag bit 9. Shift it to
	 * the top, take the arithmetic shift's sign extension as an all-ones or
	 * all-zeroes mask, and clear bits 63:48. Everything above the context field
	 * there is either the G bit or reserved, and the tag target reads all of
	 * them as zero, so clearing a few extra costs nothing.
	 *
	 * Note which way round this is. It masks for a *kernel* address and keeps
	 * the context for everything else, so an address below KERNEL_BASE that no
	 * team owns -- a wild pointer, a leftover firmware mapping -- compares with
	 * the context included and misses, which is the outcome it deserves.
	 *
	 * %g1 is masked rather than a copy of it, so that the slow path -- which
	 * stores %g1 as the new tag -- writes context zero for a kernel page
	 * without needing to know any of this.
	 */
	sllx	%g1, 63 - TSB_TAG_KERNEL_BIT, %g6
	srax	%g6, 63, %g6
	sllx	%g6, TTE_TAG_CONTEXT_SHIFT, %g6
	andn	%g1, %g6, %g1

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
	 * The comparison itself is a plain xor because the tag is stored in exactly
	 * the form the hardware builds its tag target -- including the context,
	 * after the masking above has reduced a kernel address's to zero. The
	 * difference goes to %g6 rather than over the top of %g4, so that the slow
	 * path can report the tag it really found instead of a value that has
	 * already been folded against the one wanted.
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

	/*	Which page table to walk: the kernel's, or the running team's.
	 *
	 *	One table per address space, so there are two roots and the address says
	 *	which applies -- the same bit the fast path used to decide about the
	 *	context, and for the same reason. %g3 holds the kernel's, put there at
	 *	cutover; the team's comes from the trap data block, where the context
	 *	switch writes it beside the context register it must agree with.
	 *
	 *	Branch-free: VA<31> shifted to the top and sign-extended is an all-ones
	 *	mask for a kernel address and an all-zeroes one for a user address, which
	 *	selects between the two roots without either being tested.
	 *
	 *	There is no zero case to handle. The field is initialised to the kernel's
	 *	own root, so a user address faulting with no team current walks the kernel
	 *	table and finds nothing -- the fault it deserves. Left at zero it would
	 *	walk from physical address zero and read the bottom of memory as a page
	 *	table.
	 *
	 *	This is the first code in the kernel to read [%g7 + offset] from the MMU
	 *	global bank, and it stayed broken until that was true of something: the
	 *	firmware clobbers %g7 in whichever bank its own PSTATE selects, and
	 *	call_open_firmware() could only ever save the normal one. See
	 *	sparc_restore_trap_globals().
	 */
	sllx	%g6, 63 - (TSB_TAG_KERNEL_BIT + TSB_TAG_VA_SHIFT), %g5
	srax	%g5, 63, %g5
	ldx	[%g7 + TRAP_DATA_USER_PAGE_TABLE], %g4
	andn	%g4, %g5, %g4
	and	%g3, %g5, %g5
	or	%g4, %g5, %g5

	/* Level 1: the segment table, indexed by VA<42:33>. */
	srlx	%g6, PT_SEGMENT_SHIFT, %g4
	and	%g4, PT_INDEX_MASK, %g4
	sllx	%g4, 3, %g4
	add	%g5, %g4, %g4
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
	/* Nothing mapped for this address -- which is not an error, it is a page
	 * fault. The page table is the authoritative record and it has no entry, so
	 * the question is one for the VM: the address may belong to an area that has
	 * not been faulted in yet, or to a copy-on-write page, or to nothing at all.
	 *
	 * This used to record the miss and stop, on the grounds that there was
	 * nothing to escalate to. There is now. Reaching the fault path from here is
	 * what makes user_memcpy() able to fail rather than hang: an unmapped user
	 * address arrives as a miss, and a miss that the table cannot answer has to
	 * become a fault or the retry loops forever.
	 *
	 * The fault entry reads the trap registers itself and takes the address from
	 * Tag Access, which is where a fast MMU trap leaves it. It also switches off
	 * the MMU globals this handler has been using, so the normal bank -- which
	 * the interrupted code's globals are still in, untouched -- is what gets
	 * saved.
	 */
	ba,a	sparc_fault_entry
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


/*	Spill a window that belongs to userspace.

	The hardware picks this instead of spill_*_normal when OTHERWIN says the
	window being spilled belongs to the other privilege level, which is exactly
	the case the kernel handler must not touch: it would store sixteen registers
	to an address userspace chose, from trap level one, where a fault nests one
	deeper.

	So the window goes to this thread's save area -- kernel memory, always mapped
	-- and the biased %sp it belongs to goes with it, which is what lets the fill
	side know whether a slot answers the frame being asked for.

	Registers: %g7 is the trap data block and %g3 the page table root, both of
	which belong to other handlers; %g1, %g2 and %g6 are this handler's.

	Twenty-seven instructions, inside the thirty-two a four-entry group allows.
*/
.macro SPILL_USER_WINDOW
	ldx	[%g7 + TRAP_DATA_WINDOW_SAVE], %g1
	ldx	[%g1], %g2

	/* Full is not survivable here and must not be written past: at most seven
	 * windows can be live, so eight slots cannot overflow unless something else
	 * is already wrong. Report rather than corrupt the thread beside it.
	 */
	cmp	%g2, WINDOW_SAVE_SLOTS
	bgeu,pn	%xcc, sparc_unhandled_trap
	 add	%g2, 1, %g6

	sllx	%g6, WINDOW_SAVE_SLOT_SHIFT, %g6
	add	%g1, %g6, %g6

	stx	%l0, [%g6 + (0 * 8)]
	stx	%l1, [%g6 + (1 * 8)]
	stx	%l2, [%g6 + (2 * 8)]
	stx	%l3, [%g6 + (3 * 8)]
	stx	%l4, [%g6 + (4 * 8)]
	stx	%l5, [%g6 + (5 * 8)]
	stx	%l6, [%g6 + (6 * 8)]
	stx	%l7, [%g6 + (7 * 8)]
	stx	%i0, [%g6 + (8 * 8)]
	stx	%i1, [%g6 + (9 * 8)]
	stx	%i2, [%g6 + (10 * 8)]
	stx	%i3, [%g6 + (11 * 8)]
	stx	%i4, [%g6 + (12 * 8)]
	stx	%i5, [%g6 + (13 * 8)]
	stx	%i6, [%g6 + (14 * 8)]
	stx	%i7, [%g6 + (15 * 8)]
	stx	%sp, [%g6 + WINDOW_SAVE_STACK_POINTER]

	add	%g2, 1, %g2
	stx	%g2, [%g1]

	/* Counted for the same reason the other one is. */
	ldx	[%g7 + TRAP_DATA_OTHER_SPILL_COUNT], %g2
	inc	%g2
	stx	%g2, [%g7 + TRAP_DATA_OTHER_SPILL_COUNT]

	saved
	retry
.endm


/*	Fill a window that belongs to userspace.

	The reverse, and conditional: a slot is only taken if it names the frame being
	asked for, because a window spilled by the kernel and a window the user itself
	pushed are indistinguishable from the trap type alone.

	If it does not match -- or nothing is saved -- the window has to come from the
	user's stack, and that load can fault for all the reasons the spill's store
	could. Not handled yet: reported, rather than read and hoped for. A user
	program that walks off its own stack currently stops the machine instead of
	dying, which is wrong but is wrong in the direction that says so.
*/
.macro FILL_USER_WINDOW
	ldx	[%g7 + TRAP_DATA_WINDOW_SAVE], %g1

	/*	Every arrival, counted before the checks below decide what to do with it.

		This handler is not expected to run at all -- see sparc_other_fill_count()
		-- so the count is the evidence for that rather than an argument, and it
		sits after the first instruction so the trap table verification still
		recognises the entry.
	 */
	ldx	[%g7 + TRAP_DATA_OTHER_FILL_COUNT], %g2
	inc	%g2
	stx	%g2, [%g7 + TRAP_DATA_OTHER_FILL_COUNT]
	ldx	[%g1], %g2

	brz,pn	%g2, sparc_unhandled_trap
	 sllx	%g2, WINDOW_SAVE_SLOT_SHIFT, %g6

	add	%g1, %g6, %g6

	ldx	[%g6 + WINDOW_SAVE_STACK_POINTER], %g4
	cmp	%g4, %sp
	bne,pn	%xcc, sparc_unhandled_trap
	 sub	%g2, 1, %g2

	ldx	[%g6 + (0 * 8)], %l0
	ldx	[%g6 + (1 * 8)], %l1
	ldx	[%g6 + (2 * 8)], %l2
	ldx	[%g6 + (3 * 8)], %l3
	ldx	[%g6 + (4 * 8)], %l4
	ldx	[%g6 + (5 * 8)], %l5
	ldx	[%g6 + (6 * 8)], %l6
	ldx	[%g6 + (7 * 8)], %l7
	ldx	[%g6 + (8 * 8)], %i0
	ldx	[%g6 + (9 * 8)], %i1
	ldx	[%g6 + (10 * 8)], %i2
	ldx	[%g6 + (11 * 8)], %i3
	ldx	[%g6 + (12 * 8)], %i4
	ldx	[%g6 + (13 * 8)], %i5
	ldx	[%g6 + (14 * 8)], %i6
	ldx	[%g6 + (15 * 8)], %i7

	stx	%g2, [%g1]

	restored
	retry
.endm


/*	Spill a user window to the user's own stack, with the user's privilege.

	Used while userspace is running: it nests deeply, runs out of windows, and the
	oldest one has to go to the frame it names. That frame is the user's, and so
	is the privilege the store should carry.

	The ASI goes in %asi rather than being named on each instruction, because the
	immediate-ASI form of `stxa` cannot use a register-plus-offset address and
	sixteen `add`s would not fit the group. %asi is part of the trap state, so
	`retry` puts the interrupted code's value back.

	A fault here is not handled yet: an unwritable or absent user stack page
	reports through the unhandled-trap path instead of being silently written by
	the kernel. Wrong, but wrong in the direction that says so.
*/
.macro SPILL_USER_STACK_WINDOW
	/*	Counted, because "is this handler reached at all" is the question the
		window work keeps raising and three instructions answer it for good.
	*/
	ldx	[%g7 + TRAP_DATA_USER_SPILL_COUNT], %g1
	inc	%g1
	stx	%g1, [%g7 + TRAP_DATA_USER_SPILL_COUNT]

	wr	%g0, ASI_AS_IF_USER_PRIMARY, %asi

	stxa	%l0, [%sp + BIAS + (0 * 8)] %asi
	stxa	%l1, [%sp + BIAS + (1 * 8)] %asi
	stxa	%l2, [%sp + BIAS + (2 * 8)] %asi
	stxa	%l3, [%sp + BIAS + (3 * 8)] %asi
	stxa	%l4, [%sp + BIAS + (4 * 8)] %asi
	stxa	%l5, [%sp + BIAS + (5 * 8)] %asi
	stxa	%l6, [%sp + BIAS + (6 * 8)] %asi
	stxa	%l7, [%sp + BIAS + (7 * 8)] %asi
	stxa	%i0, [%sp + BIAS + (8 * 8)] %asi
	stxa	%i1, [%sp + BIAS + (9 * 8)] %asi
	stxa	%i2, [%sp + BIAS + (10 * 8)] %asi
	stxa	%i3, [%sp + BIAS + (11 * 8)] %asi
	stxa	%i4, [%sp + BIAS + (12 * 8)] %asi
	stxa	%i5, [%sp + BIAS + (13 * 8)] %asi
	stxa	%i6, [%sp + BIAS + (14 * 8)] %asi
	stxa	%i7, [%sp + BIAS + (15 * 8)] %asi

	saved
	retry
.endm


/*	And back, with the same privilege for the same reason. */
.macro FILL_USER_STACK_WINDOW
	wr	%g0, ASI_AS_IF_USER_PRIMARY, %asi

	ldxa	[%sp + BIAS + (0 * 8)] %asi, %l0
	ldxa	[%sp + BIAS + (1 * 8)] %asi, %l1
	ldxa	[%sp + BIAS + (2 * 8)] %asi, %l2
	ldxa	[%sp + BIAS + (3 * 8)] %asi, %l3
	ldxa	[%sp + BIAS + (4 * 8)] %asi, %l4
	ldxa	[%sp + BIAS + (5 * 8)] %asi, %l5
	ldxa	[%sp + BIAS + (6 * 8)] %asi, %l6
	ldxa	[%sp + BIAS + (7 * 8)] %asi, %l7
	ldxa	[%sp + BIAS + (8 * 8)] %asi, %i0
	ldxa	[%sp + BIAS + (9 * 8)] %asi, %i1
	ldxa	[%sp + BIAS + (10 * 8)] %asi, %i2
	ldxa	[%sp + BIAS + (11 * 8)] %asi, %i3
	ldxa	[%sp + BIAS + (12 * 8)] %asi, %i4
	ldxa	[%sp + BIAS + (13 * 8)] %asi, %i5
	ldxa	[%sp + BIAS + (14 * 8)] %asi, %i6
	ldxa	[%sp + BIAS + (15 * 8)] %asi, %i7

	restored
	retry
.endm


FUNCTION(sparc_spill_user_stack):
	SPILL_USER_STACK_WINDOW


FUNCTION(sparc_fill_user_stack):
	FILL_USER_STACK_WINDOW


FUNCTION(sparc_spill_user):
	SPILL_USER_WINDOW


FUNCTION(sparc_fill_user):
	FILL_USER_WINDOW


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
	 nop

	/*	A trap out of userspace is not the kernel's failure and must not be
		reported as one.

		The path below is `done` into a C function, and `done` restores PSTATE
		from TSTATE -- the *trapped* context's PSTATE. For a trap taken in the
		kernel that is privileged, on the normal globals, with %g7 pointing at
		the current thread and %sp on a kernel stack, and the reporter works.
		For a trap taken in userspace it is none of those things: the function
		runs unprivileged, on the user's stack, with the user's %g7, and faults
		on its own first instruction.

		Which is how an illegal instruction in a user program presented as the
		kernel executing at a kernel address with user privilege -- a report of
		the reporting, with the trap that caused it nowhere in the log.

		So userspace goes the way every other trap from userspace goes. TRAP_TO_C
		is what makes the transition this needs: it stays at the privilege the
		trap already granted, takes a kernel stack from the trap data block, and
		reaches C at trap level zero where a signal can be raised against the
		thread that earned it.

		TSTATE, not the trap type: what makes this the wrong path is where the
		trap came from, and every trap type that reaches here can arrive from
		either side.
	 */
	rdpr	%tstate, %g1
	andcc	%g1, TSTATE_PRIV, %g0
	be,pn	%xcc, sparc_unhandled_user_entry
	 nop

	ldx	[%g7 + TRAP_DATA_REPORT_HANDLER], %g1
	brz,pn	%g1, sparc_unhandled_trap_stop
	 nop
	wrpr	%g1, 0, %tnpc
	done

sparc_unhandled_trap_stop:
	ba,a	sparc_unhandled_trap_stop
	 nop


/*	A fault taken inside a register window handler -- `winfixup`.

	The one place in the machine where a fault cannot be handled where it
	happened. Everywhere else, a trap arrives at trap level one, drops to zero,
	and runs C. Here the fault arrives at level *two*, one above a spill or fill
	that is still owed a `retry`, and dropping to zero to page something in throws
	that away: the level-one trap registers are the only record of where userspace
	was, and the C code's own TLB misses will overwrite them.

	So the spill or fill is not resumed. It is abandoned, and the *user
	instruction* that provoked it is re-executed.

	That works because of what the window traps already are. A spill trap is taken
	instead of completing a `save`, and its handler ends in `retry` -- which
	re-executes that `save`, whereupon the window is free and it succeeds. Nothing
	has happened yet that needs undoing: `saved` and `restored` are the
	instructions that move the window accounting, and a handler that faulted
	before reaching them has changed nothing but CWP, which the hardware moved and
	TSTATE still records. Abandoning is therefore free, and re-executing is
	correct rather than merely convenient.

	Which leaves one thing to arrange: that the address the spill or fill wanted
	is present by the time it runs again. Trap level one holds a complete
	description of a user instruction that faults -- so after dropping to it and
	putting CWP back, the machine is indistinguishable from one that has just
	taken an ordinary data fault out of userspace, and the ordinary fault path can
	run. It pages the address in and returns; the `retry` re-executes the user's
	`save` or `restore`; the window trap happens again against memory that is
	there.

	If the address cannot be paged in, vm_page_fault() raises SIGSEGV and the
	thread dies. So there is no loop: every pass either makes the page present or
	ends the thread.

	The reason this is not itself recursive is upstream of here. TRAP_TO_C's own
	`save` used to be able to spill to the user's stack, which meant the fault
	path this branches to could fault at the same address that brought us here,
	for ever. It hands the user's windows to the kernel *before* that save now, so
	its spills go to the save area and cannot fault. See the note at the save.

	Entered from sparc_fault_entry with whichever global bank the faulting trap
	selected. %g7 is the trap data block in all of them; %g1 and %g2 are free for
	the same reason they are free there.
*/
sparc_winfixup:
	/*	The address first, from the D-MMU's Tag Access register, because that is
		where a fast MMU trap leaves it and the next MMU access overwrites it.
		Page-aligned, which is all the VM needs.

		Recorded rather than carried in a register: everything below changes trap
		level and window, and a global that survives both is a global some other
		handler wants.
	 */
	mov	MMU_TLB_TAG_ACCESS, %g1
	ldxa	[%g1] ASI_DMMU, %g1

	/*	Tag Access is VA<63:13> *and the context id* in the low thirteen bits, so
		the register is not an address until the context is taken off. Shifted
		rather than masked because 0x1fff does not fit a thirteen-bit signed
		immediate, and two shifts beat a sethi.
	 */
	srlx	%g1, 13, %g1
	sllx	%g1, 13, %g1
	stx	%g1, [%g7 + TRAP_DATA_WINFIXUP_ADDRESS]

	/*	Then decide, and record enough first that deciding "no" can still report.

		Trap level two exactly. One is not possible -- the caller only branches
		here from above it -- and three or more means a fault inside a handler
		that was itself entered from a handler, which after the trap entry fix has
		no legitimate cause and is not something to guess at.
	 */
	rdpr	%tl, %g1
	cmp	%g1, 2
	bne,pn	%xcc, sparc_unhandled_trap
	 nop

	/*	Down to the window trap, discarding the fault. From here the level-one
		trap registers are the only state that matters and they are untouched.
	 */
	wrpr	%g0, 1, %tl

	/*	It has to be a trap out of userspace. A kernel window spilling to a
		kernel stack that is not mapped is a kernel stack overflow -- the guard
		page doing its job -- and re-executing the instruction would only hit it
		again. Report that one.
	 */
	rdpr	%tstate, %g2
	andcc	%g2, TSTATE_PRIV, %g0
	bne,pn	%xcc, sparc_unhandled_trap
	 nop

	/*	And it has to be a window trap. Spill vectors are trap types 0x80-0xbf and
		fill vectors 0xc0-0xff, so one unsigned compare covers both: nothing below
		0x80 belongs here, and nothing above 0xff exists.
	 */
	rdpr	%tt, %g1
	stx	%g1, [%g7 + TRAP_DATA_WINFIXUP_TRAP]
	sub	%g1, 0x80, %g1
	cmp	%g1, 0x80
	bgeu,pn	%xcc, sparc_unhandled_trap
	 nop

	/*	Put CWP back to the window the user instruction was in.

		The hardware moved it when it took the window trap -- forward by
		CANSAVE + 1 for a spill, back by one for a fill -- so that the handler
		would be *in* the window it had to save or load. TSTATE still holds the
		value from before, and writing it back is safe precisely because nothing
		else was changed: CANSAVE, CANRESTORE and OTHERWIN are as userspace left
		them, so the state is consistent with the instruction about to be run
		again.

		CWP is TSTATE bits 4:0.
	 */
	and	%g2, 0x1f, %g1
	wrpr	%g1, 0, %cwp

	/*	Counted. "Has this ever run" is the question a fixup path invites, and a
		counter answers it for the life of the port instead of for one boot.
	 */
	ldx	[%g7 + TRAP_DATA_WINFIXUP_COUNT], %g1
	inc	%g1
	stx	%g1, [%g7 + TRAP_DATA_WINFIXUP_COUNT]

	/*	Now an ordinary data fault out of userspace, at trap level one, with
		%tpc on the instruction to re-run. The handler differs from the general
		one only in where it gets the address: the MMU registers describe a fault
		two levels up that no longer exists, so it reads what was recorded above.
	 */
	ba,a	sparc_window_fault_entry
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
.macro TRAP_TO_C handler, raise_pil, save_outs=0
	/*	Which stack this trap's frame goes on.
	 *
	 *	For a trap from the kernel, below the frame of whatever trapped -- which
	 *	is what `save %sp, -N, %sp` does, and what this used to be
	 *	unconditionally.
	 *
	 *	For a trap from userspace it cannot be that. %sp then belongs to the user,
	 *	who chose it: the page may not be mapped, may not be writable, may not be
	 *	aligned, and may point at something the user would like the kernel to
	 *	overwrite for them. So the frame goes on this thread's kernel stack, whose
	 *	biased and already-adjusted top the context switch leaves in the trap data
	 *	block.
	 *
	 *	**Which is worked out after the `save`, not before it, and that is the
	 *	whole shape of what follows.**
	 *
	 *	`save rs1, rs2, rd` computes in the *old* window and writes rd in the new
	 *	one, so the obvious way to write this is to put the address in a global
	 *	first and name that global as the operand. That was this code, and it was
	 *	wrong: the `save` can *trap*.
	 *
	 *	CANSAVE being zero is exactly the condition that makes a trap arrive with
	 *	no window free, and then the save spills. The spill handler needs scratch
	 *	registers, the only ones it has are globals, and a trap taken at trap level
	 *	one does not get a fresh bank -- the hardware selects the alternate globals
	 *	on entry, they are already selected here, so the handler and this code are
	 *	holding the same physical registers. SPILL_USER_WINDOW leaves its last
	 *	scratch value in %g2; `retry` then re-runs the `save` with that as the
	 *	stack pointer. What lands in %sp is the number of spills this thread has
	 *	taken.
	 *
	 *	The failure is quiet and it is intermittent, because it needs a trap that
	 *	arrives with the register file full. Under a shallow call graph it never
	 *	happens; six windows deep it happens on the next trap. It presented as
	 *	corrupted locals in a dynamically linked program, which is what a stack
	 *	pointer of 46 looks like from C. %g1 was the other casualty -- it carried
	 *	TSTATE across the same instruction, so the test that decides whether the
	 *	trapped window belongs to userspace was reading a spill counter too, which
	 *	is how kernel windows got classified as the user's and flushed to the
	 *	user's stack.
	 *
	 *	So nothing is live in a global across the `save` any more. The operands
	 *	are %sp and a constant, which is right for a trap out of the kernel and
	 *	harmless for one out of userspace -- `save` only does arithmetic, and the
	 *	user's %sp is overwritten below before anything dereferences it. TSTATE is
	 *	read again on the far side rather than carried: the trap registers belong
	 *	to trap level one, the spill ran at level two, and its `retry` restored
	 *	the level it borrowed. Everything the far side needs it either re-reads or
	 *	keeps in a local, and locals are window registers -- the one kind of
	 *	register a spill cannot reach.
	 */
	rdpr	%tstate, %g1
	andcc	%g1, TSTATE_PRIV, %g0
	bne,pt	%xcc, 7f
	 nop

	/*	Out of userspace. Hand the user's live windows to the kernel, and take the
		kernel stack from the trap data block rather than trusting %sp.

		The hand-over is CANRESTORE into OTHERWIN: a move, so that
		CANSAVE + CANRESTORE + OTHERWIN stays at NWINDOWS - 2, which is the
		invariant the hardware enforces. What it buys is which handler a spill
		selects -- with OTHERWIN non-zero the vector is 0xa0 + 4 * WSTATE.OTHER,
		which is SPILL_USER_WINDOW and the per-thread save area, rather than
		0x80 + 4 * WSTATE.NORMAL and a store straight to the user's stack.

		**It happens twice, and both are load-bearing.**

		The one *before* the save is there because the save is what can spill:
		CANSAVE being zero is exactly the condition that makes a trap arrive with
		no window free. A spill selects its handler from WSTATE and OTHERWIN as
		they are at that instant, so setting them afterwards decides nothing about
		it -- and what it would otherwise pick is the handler that stores sixteen
		registers to an address userspace chose, from trap level one, where a
		fault nests one level deeper. Avoiding precisely that is why the save area
		exists, and for a long time this path walked past it: a program with one
		stack page not yet faulted in could take a fault inside a spill inside a
		trap entry, three levels down, with nothing able to page it in.

		The one *after* the save is there because the save adds a window to
		CANRESTORE, and that window is the one the trap came from -- so it belongs
		to userspace and has to be classified with the rest of them. Leaving it in
		CANRESTORE means a later spill inside this handler treats a user window as
		a kernel one and writes it to the user's stack with kernel privilege.

		Only the second one used to exist, which is why the first was easy to
		think unnecessary and the second easy to think redundant. They protect
		different instructions.

		%g2 rather than %g1: %g1 still holds TSTATE and the frame layout below
		wants it.
	 */
	rdpr	%canrestore, %g2
	wrpr	%g0, 0, %canrestore
	wrpr	%g2, 0, %otherwin
7:
	/*	Written for both kinds of trap, and unconditionally on purpose: a value
		that is merely expected to be right is one nothing re-establishes when it
		is not, and this one selects which handler a spill uses. Two cycles to
		stop depending on the previous trap's exit path having got there.
	 */
	wrpr	%g0, WSTATE_KERNEL, %wstate

	save	%sp, -(SPARC_MINIMUM_FRAME_SIZE + IFRAME_SIZEOF), %sp

	/*	Now on the far side, where two things are still owed for a trap out of
		userspace: the frame has to move off the user's stack and onto this
		thread's, and the window the trap came from has to be classified.

		TSTATE re-read rather than carried. See the note above -- a global does
		not survive this instruction, and %l0 is about to be loaded with exactly
		this value anyway by the block below.

		The stack address is loaded here, after the save, for the same reason.
		%g7 is the one global that does survive: the spill handlers take it as
		their base rather than clobbering it, which is what makes the trap data
		block reachable from inside them at all.

		And the window the trap came from, which the save just counted in
		CANRESTORE: it is the user's, so it goes where the user's windows went.
		Skipped for a trap out of the kernel, whose CANRESTORE counts kernel
		windows that the kernel handler is right to spill.

		CANRESTORE is exactly one here, which is what lets this be done with a
		single scratch register. The transfer above set it to zero; `save`
		increments it by one; and a spill taken *by* that save runs `saved`, which
		moves a window from OTHERWIN to CANSAVE and does not touch CANRESTORE. So
		incrementing OTHERWIN and zeroing CANRESTORE is exact, and it stops being
		exact only if the transfer above is ever removed -- which is the other
		half of the pair, so it will not be.

		%l1 as the scratch, and %l0 for TSTATE, because the block immediately
		below overwrites both -- so this borrows registers it does not have to
		give back.
	 */
	rdpr	%tstate, %l0
	andcc	%l0, TSTATE_PRIV, %g0
	bne,pt	%xcc, 8f
	 nop

	ldx	[%g7 + TRAP_DATA_KERNEL_STACK], %l1
	sub	%l1, (SPARC_MINIMUM_FRAME_SIZE + IFRAME_SIZEOF), %sp

	rdpr	%otherwin, %l1
	add	%l1, 1, %l1
	wrpr	%l1, 0, %otherwin
	wrpr	%g0, 0, %canrestore
8:

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
	/*	WSTATE and OTHERWIN were set before the save, not here -- see the note
		there. A trap out of userspace has already handed its live windows to the
		kernel by this point, and a trap out of the kernel needed neither.

		Spills from here on are the kernel's own business either way, and use the
		handler that stores with kernel privilege to a kernel stack.
	 */

	/*	The thread pointer kernel C code must see, read while the trap's own
		global bank is still selected -- the trap data block is reached through
		*that* bank's %g7, and the normal bank's belongs to whoever was
		interrupted.
	 *
	 *	Which is the whole point: userspace owns the normal %g7 while it runs,
	 *	because that is where SPARC keeps the thread pointer for thread local
	 *	storage. So a trap out of userspace finds a value userspace chose there,
	 *	and every thread_get_current_thread() in the handler would follow it.
	 *
	 *	%l6 carries it across the bank switch, because window registers are not
	 *	banked. It is free until the epilogue reloads %pil into it.
	 */
	ldx	[%g7 + TRAP_DATA_CURRENT_THREAD], %l6

	/* The frame sits above this window's own save area. Computed here rather
	 * than after the bank switch, because the address below has to be read while
	 * this bank is still selected and there is no spare local to park it in.
	 */
	add	%sp, SPARC_MINIMUM_FRAME_SIZE + BIAS, %l7

	/* The faulting address, from where the fault path put it rather than from
	 * the MMU. See sparc_fault_entry: by the time this runs, the register it
	 * came from has very likely been rewritten by a miss on this very store.
	 * Reached through the trap bank's %g7, so it has to happen before the switch
	 * and before %g7 is repointed at the thread.
	 */
	ldx	[%g7 + TRAP_DATA_FAULT_TAG_ACCESS], %l5
	stx	%l5, [%l7 + IFRAME_TAG_ACCESS]

	rdpr	%pstate, %l5
	andn	%l5, PSTATE_AG | PSTATE_MG | PSTATE_IG, %l5
	wrpr	%l5, 0, %pstate

	stx	%g1, [%l7 + IFRAME_G1]
	stx	%g2, [%l7 + IFRAME_G2]
	stx	%g3, [%l7 + IFRAME_G3]
	stx	%g4, [%l7 + IFRAME_G4]
	stx	%g5, [%l7 + IFRAME_G5]
	stx	%g6, [%l7 + IFRAME_G6]
	stx	%g7, [%l7 + IFRAME_G7]

	/*	And now overwrite it, after the interrupted code's value is safely in the
		frame. The epilogue restores it from there, so userspace gets its own %g7
		back on the way out and never sees the kernel's.
	 */
	mov	%l6, %g7

	/* The data MMU's fault status. Meaningless for traps that do not set them,
	 * and cheaper to record unconditionally than to decide here which those are.
	 *
	 * These two are safe to read this late, unlike the address: SFSR and SFAR
	 * are written for a genuine access exception and left alone by the fast
	 * misses the kernel resolves in the table, so nothing between the trap and
	 * here disturbs them.
	 */
	mov	MMU_SFSR, %l5
	ldxa	[%l5] ASI_DMMU, %l5
	stx	%l5, [%l7 + IFRAME_SFSR]
	mov	MMU_SFAR, %l5
	ldxa	[%l5] ASI_DMMU, %l5
	stx	%l5, [%l7 + IFRAME_SFAR]

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
.if \raise_pil
	wrpr	%g0, 15, %pil
.endif

	stx	%l0, [%l7 + IFRAME_TSTATE]
	stx	%l1, [%l7 + IFRAME_TPC]
	stx	%l2, [%l7 + IFRAME_TNPC]
	stx	%l3, [%l7 + IFRAME_TT]
	stx	%l4, [%l7 + IFRAME_Y]

.if \save_outs
	/* The trapped code's argument registers, which the `save` above turned into
	 * this window's ins. Only a system call wants these, and only because its
	 * arguments live there -- see the note on iframe::out.
	 */
	stx	%i1, [%l7 + IFRAME_OUT + (1 * 8)]
	stx	%i2, [%l7 + IFRAME_OUT + (2 * 8)]
	stx	%i3, [%l7 + IFRAME_OUT + (3 * 8)]
	stx	%i4, [%l7 + IFRAME_OUT + (4 * 8)]
	stx	%i5, [%l7 + IFRAME_OUT + (5 * 8)]
.endif

/*	%o0, %o6 and %o7 are saved for every trap, not just for system calls.
 *
 *	%o0 because the trap return may have to write it -- signal delivery hands the
 *	trampoline its frame pointer there -- and a slot that is written on the way out
 *	but filled only sometimes hands userspace kernel-stack poison in the cases it
 *	was not. A system call's result goes in the same place, which is why it is not
 *	saved twice.
 *
 *	%o6 is the trapped code's stack pointer, and a signal is delivered on the way
 *	out of whatever trap the thread happened to be in -- a timer interrupt, most
 *	of the time, never a system call. arch_setup_signal_frame() has to put the
 *	frame somewhere, and where it goes is below the interrupted stack pointer, or
 *	on the alternate stack if the thread is not already on it; both questions are
 *	%o6. Saving it only for system calls means every real signal reads whatever
 *	the last handler left on the kernel stack, which arrives as a 0xcccccccc
 *	slab-poison address rather than as a wrong answer.
 *
 *	%o7 is the trapped code's return address, which costs one more store and is
 *	what a backtrace across a trap needs.
 *
 *	%o7 is never restored on the way out -- see the note at the matching reload.
 */
	stx	%i0, [%l7 + IFRAME_OUT + (0 * 8)]
	stx	%i6, [%l7 + IFRAME_OUT + (6 * 8)]
	stx	%i7, [%l7 + IFRAME_OUT + (7 * 8)]

	/* Down to trap level zero, so the handler runs like ordinary kernel code.
	 * Interrupts stay disabled -- PSTATE.IE is still clear -- so nothing nests.
	 */
	wrpr	%g0, 0, %tl

	call	\handler
	 mov	%l7, %o0

	/*	Everything the kernel owes userspace before letting it run again: the
		parked register windows go to the user's stack, and then the signals,
		debugger stops and time accounting that Haiku expects at this boundary.
		Both live in sparc_kernel_exit(), in that order, because the signal
		handler's view of its own call chain is the windows this flush writes.

		Read TSTATE from the frame rather than from %l0, because a handler is
		allowed to change where the trap returns to -- that is how an unresolvable
		page fault becomes a jump to the thread's fault handler -- and where it
		returns to is what decides whether userspace is about to run.
	 */
	ldx	[%l7 + IFRAME_TSTATE], %l5
	andcc	%l5, TSTATE_PRIV, %g0
	bne,pt	%xcc, 5f
	 nop

	/*	Interrupts on for it, and off again afterwards.
	 *
	 *	Trap level zero is necessary for the flush to be able to fault, and it is
	 *	not sufficient. sparc_page_fault() refuses to page anything in when it
	 *	finds PSTATE.IE clear in the faulting frame, because a fault with
	 *	interrupts disabled cannot block and so cannot be resolved -- it takes
	 *	the thread's fault handler instead. user_memcpy() has one installed, so
	 *	the copy does not crash: it returns B_BAD_ADDRESS, the flush reports that
	 *	it could not write the window, and the thread returns to userspace with a
	 *	frame that was never stored. Which is silent, and wrong, and exactly what
	 *	the first userland on this port did -- its stack pages had never been
	 *	touched, so every one of them was a first-touch fault the flush was not
	 *	allowed to take.
	 *
	 *	thread_at_kernel_exit() requires them on for its own reasons, and turns
	 *	them off itself; sparc_kernel_exit() therefore returns with them off,
	 *	and the toggle below is what makes that true on the path where no signal
	 *	work happened.
	 *
	 *	PIL is left alone. It gates device interrupts; a page fault is
	 *	synchronous and arrives regardless, which is the whole point.
	 *
	 *	wrpr writes rs1 XOR rs2, and IE is known clear here -- PSTATE.IE has been
	 *	off since the trap was taken -- so this is a set, and the ANDN below is a
	 *	clear that does not care whether the callee already did it.
	 */
	rdpr	%pstate, %l5
	wrpr	%l5, PSTATE_IE, %pstate

	call	sparc_kernel_exit
	 mov	%l7, %o0

	rdpr	%pstate, %l5
	andn	%l5, PSTATE_IE, %l5
	wrpr	%l5, 0, %pstate
5:

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

.if \save_outs
	/* Back into the ins, so that `restore` hands them to the trapped window as
	 * its outs. This is how a system call returns a value.
	 */
	ldx	[%l7 + IFRAME_OUT + (0 * 8)], %i0
	ldx	[%l7 + IFRAME_OUT + (1 * 8)], %i1
	ldx	[%l7 + IFRAME_OUT + (2 * 8)], %i2
	ldx	[%l7 + IFRAME_OUT + (3 * 8)], %i3
	ldx	[%l7 + IFRAME_OUT + (4 * 8)], %i4
	ldx	[%l7 + IFRAME_OUT + (5 * 8)], %i5

	/*	out[0] again below for a return to userspace, and that is not redundant:
		a system call taken from kernel mode reaches only this one, and a signal
		delivered on the way out of any trap reaches only that one.
	 */
.endif

	/*	%o0 and %o6 come back on the way to userspace, whatever the trap was.
	 *
	 *	Because a signal changes both, and a signal is delivered on the way out of
	 *	whichever trap the thread was in. sparc_kernel_exit() has just run, and if
	 *	it queued a handler then arch_setup_signal_frame() put the frame pointer
	 *	the trampoline expects in out[0] and the stack it runs on in out[6]. A
	 *	handler that changed neither wrote back what the entry saved, so this is a
	 *	no-op in every other case -- which is the reason out[0] is now saved
	 *	unconditionally.
	 *
	 *	After the argument reload, not before: when a system call's return is the
	 *	thing being interrupted, the handler runs *instead* of the call returning,
	 *	so the signal's out[0] has to win over the call's result. The result is not
	 *	lost -- _kern_restore_signal_frame() puts the whole context back.
	 *
	 *	Writing %i6 reaches the trapped window's %o6 because a trap does not rotate
	 *	CWP: this window's ins and that window's outs are the same registers. Its
	 *	%l and %i registers are a different matter -- if it was spilled, the
	 *	`restore` below traps to a fill that reads them from the *new* stack
	 *	pointer. Harmless, because the trampoline is a C function that reads only
	 *	%o0 and %sp and overwrites the rest with its own `save`, and because
	 *	arch_setup_signal_frame() zeroes that save area so the read is of memory it
	 *	has already touched rather than of a page the fill would have to fault on.
	 *	A fault in a fill is winfixup, and winfixup does not exist yet.
	 *
	 *	%o7 as well, and it is the one that took a signal test to find. It is the
	 *	trapped window's return address, which the `restore` below brings back on
	 *	its own -- so reloading it is a no-op for every trap whose frame nobody
	 *	edited, which is all of them but one.
	 *
	 *	The exception is _kern_restore_signal_frame(). It puts a whole *different*
	 *	context into this frame, one that was interrupted somewhere else entirely,
	 *	and its return address is part of that context. Leaving %o7 alone meant
	 *	resuming the interrupted code with the *trampoline's* return address, so
	 *	the restarted call returned into the commpage and the trampoline called
	 *	_kern_restore_signal_frame() a second time, on a signal frame that was no
	 *	longer there.
	 *
	 *	This comment previously said "nothing on this path has a reason to change
	 *	it", which was true of every path that existed when it was written.
	 */
	ldx	[%l7 + IFRAME_TSTATE], %l5
	andcc	%l5, TSTATE_PRIV, %g0
	bne,pt	%xcc, 6f
	 nop
	ldx	[%l7 + IFRAME_OUT + (0 * 8)], %i0
	ldx	[%l7 + IFRAME_OUT + (6 * 8)], %i6
	ldx	[%l7 + IFRAME_OUT + (7 * 8)], %i7
6:

	/* Reload the trap state from the frame rather than from this window's
	 * locals, so that a handler can change where the trap returns to. That is
	 * not a hypothetical: it is how a page fault the VM cannot resolve is turned
	 * into a jump to the faulting thread's fault handler, and without it the
	 * handler's newIP is written into the frame and then quietly thrown away --
	 * which shows up as the same fault repeating forever.
	 */
	ldx	[%l7 + IFRAME_TSTATE], %l0
	ldx	[%l7 + IFRAME_TPC], %l1
	ldx	[%l7 + IFRAME_TNPC], %l2

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
	/*	If this returns to userspace, its spills are its own again -- and so is the
		`restore` below, which may itself trap to a fill. With WSTATE.NORMAL back
		at 1 that fill reads the user's stack with the user's privilege, which is
		what filling a user window should do.

		And the windows the kernel took custody of go back: OTHERWIN *added* to
		CANRESTORE, so a `restore` of a window that is still live succeeds rather
		than trapping to a fill for a window that was never spilled.

		Added, not moved, and that word is the whole correctness of this block.
		CANRESTORE here counts the kernel's own windows -- at least this handler's
		one -- while OTHERWIN counts the user's, and the invariant the hardware
		enforces is that CANSAVE + CANRESTORE + OTHERWIN stays at NWINDOWS - 2.
		Overwriting CANRESTORE with OTHERWIN drops the kernel's count on the
		floor and leaves the sum one short for every trap.

		It was a move, and it worked, because the entry used to hand the windows
		over *after* its own save -- so the handler's window was counted in
		CANRESTORE at that moment and went into OTHERWIN along with the user's,
		and moving them all back was exact. Handing them over before the save (as
		it must, so that save cannot spill to the user's stack) leaves the
		handler's window in CANRESTORE where it belongs, and the reversal has to
		respect it.

		Symptom of getting this wrong: the in-kernel userspace test stopped
		mid-flight with no output and no trap, because the window state drifted by
		one on every round trip until a `restore` found nothing to restore.
	 */
	andcc	%l0, TSTATE_PRIV, %g0
	bne,pt	%xcc, 9f
	 nop

	wrpr	%g0, WSTATE_USER, %wstate

	rdpr	%otherwin, %l6
	rdpr	%canrestore, %l5
	wrpr	%g0, 0, %otherwin
	add	%l6, %l5, %l6
	wrpr	%l6, 0, %canrestore

	/*	And make the next window userspace enters a clean one.

		CLEANWIN counts the windows a `save` may enter without the hardware
		raising clean_window, and the handler servicing that trap zeroes a window
		and increments it. It therefore only ever grows -- tens of thousands of
		times a boot, as the kernel works through its own call graph -- and
		nothing lowers it again. `flushw` does not: it spills windows, which says
		nothing about whether their registers have been scrubbed.

		So without this, CLEANWIN is saturated by the time any trap returns, and
		userspace's next `save` does not trap. It gets a window whose %l0-%l7 are
		still the kernel's -- and the kernel's most recent use of any window is
		this handler, so what userspace receives is TSTATE in %l0, the trapped
		%tpc and %tnpc in %l1 and %l2, and the trap number in %l3.

		That is a disclosure on its own. It also does not stay in the register
		file: userspace only writes the locals it has a use for, the rest keep the
		kernel's values, and when the window spills -- by userspace's own hand or
		by this kernel flushing the save area -- they are written to the user's
		stack. Kernel stack addresses and Thread pointers were being found there.

		sparc_enter_userspace() does this once, for the first window a thread ever
		enters, and the comment there explains the same mechanism. Once was not
		enough: CLEANWIN starts growing again the moment the kernel takes its next
		trap, which is long before userspace takes its second.

		Less one, because the `restore` below has not happened yet. It moves CWP
		back and takes CANRESTORE with it, and what has to hold is
		CLEANWIN == CANRESTORE *after* that -- that equality is what makes the
		next `save` trap. Guarded because CANRESTORE can be zero here, in which
		case the `restore` fills rather than counting down, and zero is already
		the value that forces the trap.
	 */
	brz,pn	%l6, 10f
	 nop
	sub	%l6, 1, %l6
10:
	wrpr	%l6, 0, %cleanwin
9:

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
.endm


/*	Deliberately local symbols: the table branches here with `ba,a`, and a branch
 *	to a .global symbol goes through a load-time relocation that leaves a
 *	displacement of zero -- a branch to itself, silently.
 */
sparc_interrupt_entry:
	TRAP_TO_C sparc_interrupt, 1


/*	The traps that mean "the VM needs to look at this address": an instruction or
 *	data access exception, and a write to a page mapped read-only.
 *
 *	The interrupt level is deliberately *not* raised here. A fault handler has to
 *	be able to block -- paging something in, taking a mutex -- and a thread that
 *	blocks with interrupts blocked never gets the CPU back, because the timer that
 *	would preempt whoever it is waiting for cannot fire. Whether interrupts are
 *	on is decided in C from the faulting context's own PSTATE, which is the only
 *	thing that knows whether blocking is allowed.
 */
sparc_fault_entry:
	/*	The faulting address, first, before this handler executes anything that
		could take it away.

		The D-MMU writes its Tag Access register on every miss, including the
		fast ones the kernel resolves in a handful of instructions without any of
		this running. So the register holds the faulting address only until the
		next D-MMU miss on this processor -- and the next D-MMU miss is whatever
		this handler touches first.

		Which used to be the interrupt frame. TRAP_TO_C reads Tag Access after
		writing the seven globals into the frame, and the frame is on a kernel
		stack that a thread which has just been scheduled may not have in the
		TLB. One miss on that store and the address the VM is handed is the
		kernel stack page rather than the address userspace faulted on. The VM
		refuses it -- correctly, it is a kernel address in a user fault -- and
		kills the thread, at a program counter that has nothing wrong with it.

		The comment on that read says "before anything else can overwrite them"
		and it was written in good faith; the seven stores above it are not
		obviously memory accesses when what you are reading is a list of
		registers being saved.

		It presented as an intermittent crash in dynamically linked programs and
		nowhere else, because the loader is the first thing this port runs that
		faults on a cold stack: the same instruction faults twice, once at the
		address it meant and once at whatever the kernel touched in between.

		Safe to store from here, and this is the reason the trap data block has
		its own locked D-TLB entry: a store to it cannot itself miss. %g1 is free
		-- TRAP_TO_C loads it below, and the branch to sparc_winfixup re-reads
		Tag Access for itself before anything can disturb it again.
	 */
	mov	MMU_TLB_TAG_ACCESS, %g1
	ldxa	[%g1] ASI_DMMU, %g1
	stx	%g1, [%g7 + TRAP_DATA_FAULT_TAG_ACCESS]

	/*	A fault at trap level two or deeper did not come from ordinary code: it
		came from one of the handlers in this file, and the only ones that touch
		memory which can fault are the window spill and fill paths that write and
		read the user's stack. Those cannot be handled here, because TRAP_TO_C
		drops to trap level zero and the level in between is still owed a return.

		See sparc_winfixup.
	 */
	rdpr	%tl, %g1
	cmp	%g1, 1
	bgu,pn	%xcc, sparc_winfixup
	 nop

	TRAP_TO_C sparc_page_fault, 0


/*	The same path, for a fault that has already been reduced to one by
	sparc_winfixup. Separate only because the address cannot come from the MMU
	registers -- they describe a fault at a trap level that no longer exists.
*/
sparc_window_fault_entry:
	TRAP_TO_C sparc_window_fault, 0


/*	A trap out of userspace that no handler in the table claims -- an illegal
	instruction, a misaligned access, a division by zero.

	Reached from sparc_unhandled_trap, which has already recorded everything
	about the trap in the trap data block and established that it came from
	userspace at trap level one. See the note there for why it cannot report
	these itself.

	The interrupt level is not raised, for the same reason the fault path does
	not raise it: the handler ends in raising a signal, which takes locks and can
	block.
*/
sparc_unhandled_user_entry:
	TRAP_TO_C sparc_unhandled_user_trap, 0


/*	System call entry, reached by `ta SPARC_SYSCALL_TRAP` from either privilege
 *	level.
 *
 *	The interrupt level is not raised, for the same reason the fault path does not
 *	raise it: a system call blocks, and a thread that blocks with interrupts
 *	blocked never gets the CPU back.
 *
 *	`save_outs` is what makes this different from every other trap here. A trap
 *	does not rotate CWP, so the `save` inside TRAP_TO_C turns the caller's
 *	%o0-%o5 into this window's %i0-%i5 -- the arguments -- and the closing
 *	`restore` turns them back, which is how the result gets home. Those registers
 *	are the calling convention, so they have to reach C, and the only way for
 *	window registers to reach C is through memory.
 *
 *	Nothing else is needed here. TRAP_TO_C already reloads %tpc and %tnpc from the
 *	frame on the way out, so advancing past the `ta` is one assignment in the
 *	handler rather than a second return path.
 */
/*	Two names for one address, and the reason is the hazard this file warns about
 *	twice already: a branch to a .global symbol goes through a load-time
 *	relocation that this kernel leaves unresolved, so `ba,a` to a global name
 *	assembles with a displacement of zero -- a branch to itself, silently.
 *
 *	The global name exists only so sparc_verify_trap_table() can check that the
 *	table's system call vector really does point here, which is the one coupling
 *	SPARC_SYSCALL_TRAP has that a shared header cannot cover: the table encodes
 *	the number as a *position*. The table branches to the local alias.
 *
 *	Making the entry itself global was tried, and produced exactly the documented
 *	failure -- `b,a` to its own address at trap type 0x140.
 */
FUNCTION(sparc_syscall_entry):
sparc_syscall_entry_local:
	TRAP_TO_C sparc_syscall, 0, 1


/*	Report this file's idea of the interrupt frame layout, for C++ to check.
 *
 *	void sparc_iframe_offsets(uint64 *out);   // eighteen entries
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
	mov	IFRAME_SFSR, %o1
	stx	%o1, [%o0 + (13 * 8)]
	mov	IFRAME_SFAR, %o1
	stx	%o1, [%o0 + (14 * 8)]
	mov	IFRAME_TAG_ACCESS, %o1
	stx	%o1, [%o0 + (15 * 8)]
	mov	IFRAME_OUT, %o1
	stx	%o1, [%o0 + (16 * 8)]
	mov	IFRAME_SIZEOF, %o1
	stx	%o1, [%o0 + (17 * 8)]
	retl
	 nop


/*	Report this file's copies of the MMU constants, for C++ to check.
 *
 *	Every one of these is duplicated from a header this file cannot include,
 *	because arch_mmu.h and arch_vm_translation_map.h are C++. Two copies of a
 *	number is the arrangement that drifts, and the failures here are the quiet
 *	kind: a page table shift that disagrees walks to a plausible wrong entry, and
 *	a context shift that disagrees masks the wrong bits out of a tag target and
 *	simply misses forever.
 *
 *	void sparc_mmu_defines(uint64 *out);   // eleven entries
 */
FUNCTION(sparc_mmu_defines):
	mov	PT_SEGMENT_SHIFT, %o1
	stx	%o1, [%o0 + (0 * 8)]
	mov	PT_DIRECTORY_SHIFT, %o1
	stx	%o1, [%o0 + (1 * 8)]
	mov	PT_TABLE_SHIFT, %o1
	stx	%o1, [%o0 + (2 * 8)]
	mov	PT_INDEX_MASK, %o1
	stx	%o1, [%o0 + (3 * 8)]
	mov	PT_VA_HOLE_SHIFT, %o1
	stx	%o1, [%o0 + (4 * 8)]
	mov	TTE_TAG_CONTEXT_SHIFT, %o1
	stx	%o1, [%o0 + (5 * 8)]
	mov	TSB_TAG_KERNEL_BIT, %o1
	stx	%o1, [%o0 + (6 * 8)]
	mov	TSB_TAG_VA_SHIFT, %o1
	stx	%o1, [%o0 + (7 * 8)]
	mov	WINDOW_SAVE_SLOT_SHIFT, %o1
	stx	%o1, [%o0 + (8 * 8)]
	mov	WINDOW_SAVE_SLOTS, %o1
	stx	%o1, [%o0 + (9 * 8)]
	mov	WINDOW_SAVE_STACK_POINTER, %o1
	stx	%o1, [%o0 + (10 * 8)]
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

	/*	And the interrupt bank, which was left out until something read it.
	 *
	 *	UltraSPARC-IIi gives the interrupt traps a bank of their own, selected by
	 *	PSTATE.IG (TABLE 14-12) -- that was section 22's bug, a handler running on
	 *	the wrong bank because only AG was cleared. Nothing read %g7 there, so
	 *	nothing noticed it was never written, until TRAP_TO_C started loading the
	 *	current thread out of the trap data block through whichever bank the trap
	 *	selected. An interrupt then read a garbage pointer and reported a thread
	 *	of -1.
	 */
	or	%o3, PSTATE_IG, %o4
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
	mov	TRAP_DATA_USER_PAGE_TABLE, %o1
	stx	%o1, [%o0 + (17 * 8)]
	mov	TRAP_DATA_KERNEL_STACK, %o1
	stx	%o1, [%o0 + (18 * 8)]
	mov	TRAP_DATA_WINDOW_SAVE, %o1
	stx	%o1, [%o0 + (19 * 8)]
	mov	TRAP_DATA_CURRENT_THREAD, %o1
	stx	%o1, [%o0 + (20 * 8)]
	mov	TRAP_DATA_USER_SPILL_COUNT, %o1
	stx	%o1, [%o0 + (21 * 8)]
	mov	TRAP_DATA_OTHER_SPILL_COUNT, %o1
	stx	%o1, [%o0 + (22 * 8)]
	mov	TRAP_DATA_OTHER_FILL_COUNT, %o1
	stx	%o1, [%o0 + (23 * 8)]
	mov	TRAP_DATA_WINFIXUP_ADDRESS, %o1
	stx	%o1, [%o0 + (24 * 8)]
	mov	TRAP_DATA_WINFIXUP_TRAP, %o1
	stx	%o1, [%o0 + (25 * 8)]
	mov	TRAP_DATA_WINFIXUP_COUNT, %o1
	stx	%o1, [%o0 + (26 * 8)]
	mov	TRAP_DATA_FAULT_TAG_ACCESS, %o1
	stx	%o1, [%o0 + (27 * 8)]
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


/*	One trap table entry that branches to a handler.
 *
 *	Thirty-two bytes like every other entry, padded and asserted by .org, which
 *	fails the build if a branch and its delay slot somehow grew past it.
 */
.macro TRAP_TO_HANDLER target
8:	ba,a	\target
	 nop
	.org 8b + 32
.endm


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

.macro TRAP_GROUP_SPILL_USER
8:	SPILL_USER_WINDOW
	.org 8b + 128
.endm


.macro TRAP_GROUP_SPILL_USER_STACK
8:	SPILL_USER_STACK_WINDOW
	.org 8b + 128
.endm


.macro TRAP_GROUP_FILL_USER_STACK
8:	FILL_USER_STACK_WINDOW
	.org 8b + 128
.endm


/*	A whole four-entry group of unhandled, for a WSTATE value nothing sets. */
.macro TRAP_GROUP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
	TRAP_UNHANDLED
.endm


.macro TRAP_GROUP_FILL_USER
8:	FILL_USER_WINDOW
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
	/* 0x000-0x007: reset and the instruction access error traps. */
	.rept 0x008
	TRAP_UNHANDLED
	.endr

	/* 0x008: instruction_access_exception -- an instruction fetch the MMU
	 * refused, which is a page fault on the text side.
	 */
	TRAP_TO_HANDLER sparc_fault_entry

	/* 0x009-0x023: illegal instruction, privileged action, floating point, tag
	 * overflow. Nothing handled yet, but every entry is filled: an unhandled
	 * trap must reach a handler that reports it, not blank space.
	 */
	.rept 0x01b
	TRAP_UNHANDLED
	.endr

	/* 0x024-0x027: clean_window. The most frequent trap of all. */
	TRAP_GROUP_CLEAN

	/* 0x028-0x02f: division by zero, and the rest. */
	.rept 0x008
	TRAP_UNHANDLED
	.endr

	/* 0x030: data_access_exception -- the ordinary page fault. */
	TRAP_TO_HANDLER sparc_fault_entry

	/* 0x031-0x040: alignment and the rest. */
	.rept 0x010
	TRAP_UNHANDLED
	.endr

	/* 0x041-0x04f: interrupt_level_1 through interrupt_level_15. One entry each,
	 * not four, because the levels are consecutive rather than spaced apart the
	 * way the busier traps are.
	 *
	 * All fifteen, not just the one the %TICK comparator uses. Two reasons. The
	 * SOFTINT register can post any of them and the kernel is entitled to use
	 * that. And a device interrupt reaches the processor as a packet -- trap 0x60
	 * -- which the operating system is expected to turn into a SOFTINT at the
	 * priority the device deserves, so these are the traps that servicing
	 * actually happens on. Leaving them unhandled makes a level nobody has
	 * arranged for yet stop the machine, which is the wrong answer for something
	 * the hardware can raise on its own.
	 */
	.rept 0x00f
8:	ba,a	sparc_interrupt_entry
	 nop
	.org 8b + 32
	.endr

	/* 0x050-0x05f: the rest. */
	.rept 0x010
	TRAP_UNHANDLED
	.endr

	/* 0x060: interrupt_vector -- how a *device* interrupt arrives, as a packet
	 * rather than as an asserted level. One entry, like the levels beside it.
	 *
	 * Shares the entry path with the level interrupts because the work is
	 * identical: save the globals, drop to trap level zero, raise %pil so
	 * nothing nests, call C. What differs is only what C reads to find out what
	 * happened, and sparc_interrupt() tells the two apart by the trap type in
	 * the frame.
	 *
	 * The hardware selects the *interrupt* globals for this trap rather than the
	 * alternate ones (PSTATE.IG, manual section 11.10), which the entry path
	 * already handles -- it clears AG, MG and IG together for exactly this
	 * reason.
	 */
8:	ba,a	sparc_interrupt_entry
	 nop
	.org 8b + 32

	/* 0x061-0x063: the rest. */
	.rept 0x003
	TRAP_UNHANDLED
	.endr

	/* 0x064-0x06b: the two MMU misses, which are the point of all this. */
	TRAP_GROUP_INSTRUCTION_MISS
	TRAP_GROUP_DATA_MISS

	/* 0x06c-0x06f: fast_data_access_protection -- a write to a page the TLB
	 * holds read-only. Four entries, like the misses beside it, because the trap
	 * types are spaced apart the same way.
	 */
8:	ba,a	sparc_fault_entry
	 nop
	.org 8b + 128

	/* 0x070-0x07f */
	.rept 0x010
	TRAP_UNHANDLED
	.endr

	/* 0x080-0x09f: spill_0_normal through spill_7_normal -- a kernel window.
	 * 0x0a0-0x0bf: spill_0_other through spill_7_other -- a user window, which
	 * the kernel must not store to the address userspace chose.
	 */
	TRAP_GROUP_SPILL		/* WSTATE.NORMAL 0: a kernel window */
	TRAP_GROUP_SPILL_USER_STACK	/* WSTATE.NORMAL 1: userspace's own */
	.rept 6
	TRAP_GROUP_UNHANDLED
	.endr

	TRAP_GROUP_SPILL_USER		/* WSTATE.OTHER 0: the save area */
	.rept 7
	TRAP_GROUP_UNHANDLED
	.endr

	/* 0x0c0-0x0df: fill_0_normal through fill_7_normal.
	 * 0x0e0-0x0ff: fill_0_other through fill_7_other.
	 */
	TRAP_GROUP_FILL			/* WSTATE.NORMAL 0 */
	TRAP_GROUP_FILL_USER_STACK	/* WSTATE.NORMAL 1 */
	.rept 6
	TRAP_GROUP_UNHANDLED
	.endr

	TRAP_GROUP_FILL_USER		/* WSTATE.OTHER 0 */
	.rept 7
	TRAP_GROUP_UNHANDLED
	.endr

	/* 0x100-0x13f: software traps below the system call vector.
	 *
	 * Deliberately unhandled rather than ignored. A `ta` with a small immediate
	 * is the shape a hand-written stub or a compiler is most likely to emit by
	 * accident, so it should report rather than quietly do whatever the system
	 * call vector does.
	 */
	.rept 0x040
	TRAP_UNHANDLED
	.endr

	/* 0x140: `ta 0x40`, the system call. One entry, so .org asserts the size
	 * the way the four-entry groups above do.
	 */
8:	ba,a	sparc_syscall_entry_local
	 nop
	.org 8b + 32

	/* 0x141-0x1ff: the rest of the software traps. */
	.rept 0x0bf
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
