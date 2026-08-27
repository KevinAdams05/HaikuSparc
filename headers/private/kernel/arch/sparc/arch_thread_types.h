/*
** Copyright 2001, Travis Geiselbrecht. All rights reserved.
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_ARCH_SPARC_THREAD_TYPES_H
#define KERNEL_ARCH_SPARC_THREAD_TYPES_H


#include <arch_cpu.h>
#include <kernel.h>


// The smallest stack frame the SPARC V9 ABI allows: 128 bytes for the sixteen
// window registers a spill writes, then 48 more for the six outgoing argument
// slots and the hidden structure-return pointer. Frames are 16-byte aligned and
// 176 is a multiple of 16, so it doubles as the alignment unit.
#define SPARC_MINIMUM_FRAME_SIZE	176

// SPARC V9 stack bias. Duplicated in arch_traps.S, which cannot see this header.
#define SPARC_STACK_BIAS			2047

// PSTATE fields a thread's context cares about (TABLE 14-12, printed p.201).
#define SPARC_PSTATE_IE				0x002
#define SPARC_PSTATE_PRIV			0x004
#define SPARC_PSTATE_PEF			0x010


#define	IFRAME_TRACE_DEPTH 4

struct iframe_stack {
	struct iframe *frames[IFRAME_TRACE_DEPTH];
	int32	index;
};

/*	Everything a voluntary context switch has to remember about a thread.

	It is a short list, and the reason is the register windows. The SPARC ABI's
	callee-saved registers are the window registers -- %l0-%l7 and %i0-%i7 -- and
	a context switch flushes every live window to its own stack frame before
	changing anything. So the registers save themselves, into the stack they
	belong to, and what is left to record is where that stack is and where to
	resume.

	The floating-point registers need no saving either, which is worth stating
	because it looks like an omission. Every %f register is caller-saved in the
	SPARC V9 ABI, so a compiler has already spilled anything live across a call --
	and a voluntary switch happens inside one. Preemption is different, but that
	arrives with the interrupt frame, which saves what it interrupts.
*/
struct arch_context {
	addr_t	sp;
		// The stack pointer to resume on, biased: SPARC V9 keeps %sp at the
		// frame address minus 2047, so the register save area is at sp + 2047.
	addr_t	pc;
		// Where to resume, less eight. The switch returns with "ret", which
		// jumps to %i7 + 8, and storing the adjusted value here keeps that
		// arithmetic in one place instead of splitting it across C and assembly.
	uint64	pstate;
		// PSTATE, for its interrupt-enable bit above all.
	uint64	pil;
		// The processor interrupt level.
		//
		// These two are per-CPU registers, not per-thread ones, and leaving
		// them out was a real bug rather than an omission of detail. A thread
		// switched out from inside an interrupt handler has interrupts disabled
		// and the interrupt level raised; the thread switched *to* expects its
		// own. Without saving them, whichever thread ran last decides both for
		// everybody -- so a thread could resume with interrupts enabled inside a
		// handler that still owed a trap return, and a nested interrupt would
		// then drive the trap level below zero.
};

/*	Where a user register window goes when the kernel has to spill it.

	Not to the user's stack, which is where it belongs and where it cannot be
	written from. A spill handler runs at trap level one or deeper, and a store to
	the user's stack can fault there -- the page may be paged out, may be
	read-only, or the stack pointer may be whatever userspace felt like putting in
	%sp. A fault at that trap level nests one deeper, and section 2.6 of the
	porting plan says where that ends.

	So the handler writes here instead: kernel memory, always mapped, always
	writable, and per-thread so a context switch changes the pointer rather than
	the contents. The fill side reads it back, and the stack pointer recorded
	alongside each window is what makes that safe -- a fill only takes a slot that
	names the frame being asked for.

	Eight slots because there are eight windows and at most seven can be live, so
	the ninth spill cannot happen. 256 bytes a slot rather than the 136 the
	contents need, so the handler indexes with a shift instead of a multiply.
*/
#define SPARC_WINDOW_SAVE_SLOTS		8
#define SPARC_WINDOW_SAVE_SLOT_SIZE	256
#define SPARC_WINDOW_SAVE_REGISTERS	16

struct sparc_window_save {
	uint64	count;
		// Slots in use. First, so the handler reaches it without an offset.
	uint64	_padding[31];
		// Rounds the header to one slot, so slot N is at base + (N + 1) * 256.

	uint64	slots[SPARC_WINDOW_SAVE_SLOTS][SPARC_WINDOW_SAVE_SLOT_SIZE
		/ sizeof(uint64)];
		// Each slot: %l0-%l7, then %i0-%i7, then the biased %sp the window
		// belongs to. The rest is padding.
};

#define SPARC_WINDOW_SAVE_STACK_POINTER	(SPARC_WINDOW_SAVE_REGISTERS * 8)

// architecture specific thread info
struct arch_thread {
	struct arch_context	context;

	void	*interrupt_stack;

	// used to track interrupts on this thread
	struct iframe_stack	iframes;

	/*	An interrupted system call's restart state, carried across a signal.
	 *
	 *	Restarting one means going back to its `ta` with its first argument in
	 *	%o0, and iframe holds both -- but the frame that reaches the trap return
	 *	after a handler has run is `_kern_restore_signal_frame`'s, not the
	 *	interrupted call's, and its copies describe the wrong system call. Taking
	 *	them from there sends the thread into the middle of the commpage
	 *	trampoline with a signal frame pointer in %o0.
	 *
	 *	They cannot be recomputed on the far side, either. The trap return could
	 *	step %tpc back one instruction, which is what x86 does, but a `ta` in a
	 *	delay slot has a %tnpc that is not %tpc + 4 and the pair is then not
	 *	recoverable from the advanced values -- which is why sparc_syscall()
	 *	copies them rather than computing them.
	 *
	 *	So they are kept here, per thread, between the signal frame being built
	 *	and being restored.
	 *
	 *	**One interrupted call at a time**, which is the limit of this. A handler
	 *	that itself makes a restartable call and is itself interrupted would
	 *	overwrite these before the outer restore reads them. Haiku carries its own
	 *	restart state in the signal frame precisely to avoid that, and this cannot
	 *	join it there without a second field the generic structure does not have.
	 */
	uint64	signalSyscallTpc;
	uint64	signalSyscallTnpc;
	uint64	signalSyscallArg0;

	// See sparc_window_save. Aligned so a slot never straddles a cache line.
	struct sparc_window_save	windowSave __attribute__((aligned(16)));
};

struct arch_team {
	// gcc treats empty structures as zero-length in C, but as if they contain
	// a char in C++. So we have to put a dummy in to be able to use the struct
	// from both in a consistent way.
	char	dummy;
};

struct arch_fork_arg {
	// gcc treats empty structures as zero-length in C, but as if they contain
	// a char in C++. So we have to put a dummy in to be able to use the struct
	// from both in a consistent way.
	char	dummy;
};

// In arch_thread.cpp. Called once, from arch_platform_init_post_thread().
extern void sparc_test_context_switch();
extern void sparc_test_thread_wait();


#endif	/* KERNEL_ARCH_SPARC_THREAD_TYPES_H */

