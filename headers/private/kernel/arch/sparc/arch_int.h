/*
 * Copyright 2005-2021, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Axel Dörfler <axeld@pinc-software.de>
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 * 		Adrien Destugues <pulkomandy@pulkomandy.tk>
 */
#ifndef _KERNEL_ARCH_SPARC_INT_H
#define _KERNEL_ARCH_SPARC_INT_H

#include <SupportDefs.h>

#include <arch_asm_defs.h>
#include <arch_syscall_defs.h>

#define NUM_IO_VECTORS	256


/*	What an interrupt saves about the code it interrupted.

	Short, because most of the interrupted state saves itself. A trap does not
	rotate CWP, so the interrupted window is still in the register file, and the
	`save` the handler does to get a window of its own makes it the previous one
	-- which `restore` brings back, from the stack if a spill got there first. So
	no %l, %i or %o register appears here.

	The globals do, because they are shared across every window and the
	interrupted code was in the middle of using them. So do the trap registers,
	which have to be read out before the handler drops to trap level zero to run
	C, since trap level zero has no %tpc, %tnpc or %tstate of its own.

	Offsets are duplicated in arch_traps.S, which cannot read this header;
	sparc_verify_iframe_layout() compares the two.
*/
struct iframe {
	uint64	tstate;
	uint64	tpc;
	uint64	tnpc;
	uint64	tt;
	uint64	y;
	uint64	g1;
	uint64	g2;
	uint64	g3;
	uint64	g4;
	uint64	g5;
	uint64	g6;
	uint64	g7;
	uint64	pil;
	uint64	sfsr;
	uint64	sfar;
		// The data MMU's Synchronous Fault Status and Address registers, read on
		// entry rather than by the handler. They hold the most recent fault and
		// would otherwise be overwritten by any fault the handler itself took --
		// including a perfectly ordinary one, since a fault handler runs with
		// traps enabled.
		//
		// Only meaningful for the traps that set them. An instruction access
		// exception has no address register of its own; %tpc is the address.
	uint64	tagAccess;
		// Where a *fast* MMU trap puts its address. The protection trap is one
		// of those -- the "fast" in fast_data_access_protection means it skips
		// the fault status registers -- so SFAR says nothing about it and this
		// is the only place its address appears.
	uint64	out[8];
		// The trapped code's %o0-%o7.
		//
		// A trap does not rotate CWP, so the handler's `save` makes the trapped
		// window the previous one and its outs become the handler's ins -- which
		// is why C cannot see them without this: they are register-window state,
		// not memory.
		//
		// out[0..5] are filled only for a system call, whose arguments arrive in
		// %o0-%o5 and whose result goes back in %o0. Those six are the calling
		// interface, and copying them here is what lets the handler be written
		// in C. They are restored on the way out, so writing out[0] sets what
		// the caller sees. Every other trap skips them: the interrupt path alone
		// takes tens of thousands of traps a boot and has no use for them.
		//
		// out[6] and out[7] are filled for *every* trap and restored for none.
		//
		// out[6] is the trapped code's stack pointer -- biased, as %sp always is
		// on V9, so it is 2047 below the frame it points at. Two things need it,
		// and only one of them is a system call. A call with more than six
		// arguments reads the rest off the caller's stack; and signal delivery,
		// which happens on the way out of whichever trap the thread was in when
		// the signal arrived, has to decide where to put the signal frame. That
		// is nearly always a timer interrupt and nearly never a system call, so
		// filling this one conditionally makes it poison exactly when it
		// matters. out[7] is the return address, for backtraces across a trap.
	uint64	syscallTpc;
	uint64	syscallTnpc;
	uint64	syscallArg0;
		// What a system call needs in order to be able to run twice.
		//
		// A signal arriving while a thread is blocked in an interruptible system
		// call makes the call return B_INTERRUPTED, and if the handler was
		// installed with SA_RESTART the kernel runs the call again rather than
		// reporting that. Running it again means going back to the `ta` -- and by
		// the time anything knows to do that, sparc_syscall() has copied %tnpc
		// over %tpc to step past it and the dispatcher has written the result
		// over the first argument.
		//
		// Neither is recoverable by arithmetic. %tpc is not %tpc - 4, for the
		// same reason the step past the `ta` is a copy rather than an addition:
		// a `ta` in a delay slot does not follow that rule.
		//
		// Kept in the frame rather than in arch_thread because system calls
		// nest. A handler interrupting a restartable call can make calls of its
		// own, and the outer call's restart is decided only after they have
		// returned; one copy per thread would be overwritten by the inner call.
		//
		// Written only by sparc_syscall(), read only when the thread is flagged
		// for a restart.
};

#define IFRAME_SIZEOF	224
	// sizeof(struct iframe) rounded up to a 16-byte multiple, which is what
	// stack frames have to be aligned to.

/*	The system call.

	SPARC V9's Tcc instructions produce trap types 0x100 + n for a software trap
	number n in 0..127, and the architecture leaves the choice to the operating
	system -- which is why SunOS, Solaris, Linux and the BSDs all use different
	ones. Haiku on sparc uses 0x40, so trap type 0x140.

	Mid-range on purpose. A `ta` with a small immediate is the shape a
	hand-written stub or a compiler is most likely to emit by accident, and it
	should land on an entry that reports rather than on this one.

	The number itself lives in <asm_defs.h>, because libroot's stubs need it too
	and they are assembly. This file only derives the trap type from it.

	The calling convention is the ABI's own, which is what makes it cheap: the
	index in %g1, up to six arguments in %o0-%o5, the result in %o0. A trap does
	not rotate CWP, so the handler's `save` makes those registers its %i's and
	the closing `restore` puts them back -- no copying either way. Arguments
	beyond six live where the ABI puts them, on the caller's stack, which the
	kernel reaches with user_memcpy() like any other user pointer.
*/
#define TRAP_SYSCALL				(0x100 + SPARC_SYSCALL_TRAP)

// A call index the kernel answers itself, for testing the trap path before there
// is a dispatcher or a userland to reach it from. Deliberately absurd, so it can
// never collide with a real syscall number.
#define SPARC_SYSCALL_TEST_ECHO		0xecc0

// And one the userspace test program uses. Small enough to be a `mov` immediate,
// because that program is hand-written assembly copied into a page.
#define SPARC_SYSCALL_TEST_EXIT		0x555

// Trap types 0x41 through 0x4f are interrupt_level_1 through _15, one entry
// each rather than the groups of four the busier traps get.
#define TRAP_INTERRUPT_LEVEL_BASE	0x40
#define TRAP_INTERRUPT_LEVEL_14		0x4e

/*	How a device interrupt arrives, and it is not one of the levels above.
 *
 *	sun4u delivers a device interrupt as an interrupt *packet* rather than by
 *	asserting a level: the bridge sends a vector, the processor takes this trap,
 *	and the handler reads the vector out of an ASI register to find out what
 *	happened. The manual calls the mechanism "Mondo" (chapter 11). The level
 *	traps above are for the processor's own SOFTINT register, which is how the
 *	%TICK comparator and software-posted interrupts arrive.
 *
 *	So this is a second, independent interrupt path, and the two share only the
 *	entry code.
 */
#define TRAP_INTERRUPT_VECTOR		0x60

// Traps that mean "this address needs the VM's attention".
#define TRAP_INSTRUCTION_ACCESS		0x08
#define TRAP_DATA_ACCESS		0x30
#define TRAP_INSTRUCTION_MMU_MISS	0x64
#define TRAP_DATA_MMU_MISS		0x68
#define TRAP_DATA_PROTECTION		0x6c
	// The three "fast" MMU traps reach this handler too, by way of the miss
	// slow path: a miss the page table cannot answer is a page fault.

/*	What a device interrupt is called on sun4u.
 *
 *	An Interrupt Number Register value -- an INR -- is eleven bits: a five-bit
 *	Interrupt Group Number and a six-bit Interrupt Number Offset. The INO is what
 *	identifies the source; the IGN identifies which bridge it came through, and
 *	on UltraSPARC-IIi it is fixed at 0x1f and not programmable (manual chapter
 *	11, "Compatibility Note").
 *
 *	This port uses the bare INO as its interrupt vector, which fits Haiku's
 *	NUM_IO_VECTORS with room to spare and keeps the number a driver sees equal to
 *	the one the firmware's interrupt-map published. The IGN is added and removed
 *	at the two points that talk to the hardware.
 */
#define INR_IGN_SHIFT			6
#define INR_IGN_MASK			0x7c0
#define INR_INO_MASK			0x03f
#define SPARC_IIi_IGN			(0x1f << INR_IGN_SHIFT)

// INOs below this are PCI slot interrupts, identified by bus, slot and pin.
// From this one up they are the on-board devices -- and the two halves live in
// different register files, which is the whole reason the distinction matters
// here. TABLE 19-28.
#define INO_FIRST_OBIO			0x20

// An interrupt mapping register. Bit 31 is what gates delivery: with it clear
// the interrupt is held, not lost. The target-processor field is read-only zero
// on UltraSPARC-IIi. FIGURE 11-2.
#define INTMAP_VALID			(1ULL << 31)

// An interrupt clear register is write-only, and its low two bits are the state
// machine for that INO. Writing IDLE after servicing is not optional: the state
// machine stays in RECEIVED otherwise and that INO never fires again.
// TABLE 19-34.
#define INTCLR_IDLE			0
#define INTCLR_RECEIVED			1
#define INTCLR_PENDING			3

// Where the processor reports an incoming interrupt packet. BUSY says a vector
// has been received and must be cleared by writing zero, or nothing further is
// delivered. Sections 11.10.4 and 11.10.5.
#define ASI_INTR_RECEIVE		0x49
#define ASI_INTR_DATA			0x7f
#define INTR_RECEIVE_BUSY		(1ULL << 5)
#define INTR_DATA_0			0x40
	// On UltraSPARC-IIi only data 0 carries anything, and only its low eleven
	// bits: the INR. Data 1 and 2 always read zero.


// Synchronous Fault Status Register fields, from the UltraSPARC-IIi manual's
// I-/D-SFSR description. Only the ones this port reads.
#define SFSR_FAULT_VALID		0x001
#define SFSR_WRITE			0x004
#define SFSR_PRIVILEGED			0x008
	// PSTATE.PRIV of the access that faulted, not of the handler.

// PSTATE lives in TSTATE bits 20:8, so its own bits are shifted by eight.
#define TSTATE_PSTATE_SHIFT		8
#define TSTATE_PRIV			(0x004 << TSTATE_PSTATE_SHIFT)
#define TSTATE_IE			(0x002 << TSTATE_PSTATE_SHIFT)

// The condition codes live in TSTATE bits 39:32 -- the two four-bit sets, xcc
// above icc. Everything a signal handler is allowed to see of TSTATE, and
// nothing it is not.
#define TSTATE_CCR_SHIFT		32
#define TSTATE_CCR_MASK			0xffULL

static inline void
arch_int_enable_interrupts_inline(void)
{
	int tmp;
	asm volatile(
		"rdpr %%pstate, %0\n"
		"or %0, 2, %0\n"
		"wrpr %0, %%pstate\n"
		: "=r" (tmp)
	);
}


static inline int
arch_int_disable_interrupts_inline(void)
{
	int flags;
	int tmp;
	asm volatile(
		"rdpr %%pstate, %0\n"
		"andn %0, 2, %1\n"
		"wrpr %1, %%pstate\n"
		: "=r" (flags), "=r" (tmp)
	);
	return flags & 2;
}


static inline void
arch_int_restore_interrupts_inline(int oldState)
{
	if (oldState)
		arch_int_enable_interrupts_inline();
}


static inline bool
arch_int_are_interrupts_enabled_inline(void)
{
	int flags;
	asm volatile(
		"rdpr %%pstate, %0\n"
		: "=r" (flags)
	);

	return flags & 2;
}


// map the functions to the inline versions
#define arch_int_enable_interrupts()	arch_int_enable_interrupts_inline()
#define arch_int_disable_interrupts()	arch_int_disable_interrupts_inline()
#define arch_int_restore_interrupts(status)	\
	arch_int_restore_interrupts_inline(status)
#define arch_int_are_interrupts_enabled()	\
	arch_int_are_interrupts_enabled_inline()


#ifdef __cplusplus
// In arch_int.cpp. Called once, from arch_int_init_post_device_manager().
extern void sparc_test_preemption();
extern void sparc_test_user_memory();
#endif


#endif /* _KERNEL_ARCH_SPARC_INT_H */
