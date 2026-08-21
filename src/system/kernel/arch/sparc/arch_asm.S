/*
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>


/*	Duplicated from arch_thread_types.h, which the assembler cannot read.
 *	sparc_verify_context_layout() compares these against the structure they
 *	describe, so the two copies cannot drift apart unnoticed.
 */
#define SPARC_MINIMUM_FRAME_SIZE	176

// PSTATE fields, from arch_thread_types.h, which this file cannot include.
// Checked against it by sparc_verify_context_layout().
#define PSTATE_IE			0x002
#define PSTATE_PRIV			0x004
#define PSTATE_PEF			0x010


/*	%g2 and %g3 are the SPARC V9 "application" globals and %g6/%g7 are reserved
 *	for system software, so the assembler insists they be declared before use.
 *	Scratch is right for sparc_enter_userspace(), which never returns: the values
 *	it leaves in them belong to nobody.
 */
.register %g2, #scratch
.register %g3, #scratch
.register %g6, #scratch
.register %g7, #scratch
#define CONTEXT_SP			0
#define CONTEXT_PC			8
#define CONTEXT_PSTATE			16
#define CONTEXT_PIL			24


.text


/* status_t arch_cpu_user_memcpy(void *to, const void *from, size_t size, addr_t *faultHandler) */
FUNCTION(_arch_cpu_user_memcpy):
	// TODO
	retl
	nop
FUNCTION_END(_arch_cpu_user_memcpy)


/* status_t arch_cpu_user_memset(void *to, char c, size_t count, addr_t *faultHandler) */
FUNCTION(_arch_cpu_user_memset):
	// TODO
	retl
	nop
FUNCTION_END(_arch_cpu_user_memset)


/* ssize_t arch_cpu_user_strlcpy(void *to, const void *from, size_t size, addr_t *faultHandler) */
FUNCTION(_arch_cpu_user_strlcpy):
	// TODO
	retl
	nop
FUNCTION_END(_arch_cpu_user_strlcpy)


/*	The register-window context switch.
 *
 *	void sparc_context_switch(struct arch_context *from,
 *		struct arch_context *to);
 *
 *	The whole switch is four stores and loads, and it works because of one trick:
 *	this routine's own window is the pivot.
 *
 *	"save" gives us a window of our own, so %i0 and %i1 are the arguments, %i6 is
 *	the caller's stack pointer and %i7 is the address to return to. "flushw" then
 *	writes every window except this one out to the stack frame it belongs to, so
 *	the outgoing thread's registers are all in its own stack, where a fill will
 *	find them again.
 *
 *	That leaves this window as the only live one, and its %i6 and %i7 are exactly
 *	"which stack to return onto" and "where to return to". Overwrite them with the
 *	incoming thread's pair and the ordinary function epilogue does the switch:
 *	"ret" jumps to the new %i7 + 8, and "restore" in its delay slot drops into a
 *	window that has to be filled from the new %i6 -- the incoming thread's stack.
 *
 *	So the registers are never copied anywhere. They are pushed to one stack and
 *	pulled from another by the same spill and fill handlers that service every
 *	other window trap, which is why this is short and why it could not have been
 *	written before those worked.
 *
 *	%fp is %i6 and the assembler accepts either name; %i6 is used here because
 *	what matters is its position in the window, not its role as a frame pointer.
 */
FUNCTION(sparc_context_switch):
	save	%sp, -SPARC_MINIMUM_FRAME_SIZE, %sp

	/* Every window but this one, onto the stack it came from. This is what makes
	 * changing stacks safe, and it is not free -- it is a burst of spill traps.
	 */
	flushw

	stx	%i6, [%i0 + CONTEXT_SP]
	stx	%i7, [%i0 + CONTEXT_PC]

	/* PSTATE and PIL are per-CPU registers that behave as though they were
	 * per-thread, so the switch has to carry them. A thread suspended inside an
	 * interrupt handler has interrupts off and the interrupt level raised, and
	 * the thread resumed in its place must not inherit either.
	 */
	rdpr	%pstate, %l0
	stx	%l0, [%i0 + CONTEXT_PSTATE]
	rdpr	%pil, %l1
	stx	%l1, [%i0 + CONTEXT_PIL]

	ldx	[%i1 + CONTEXT_SP], %i6
	ldx	[%i1 + CONTEXT_PC], %i7

	ldx	[%i1 + CONTEXT_PIL], %l1
	wrpr	%l1, 0, %pil
	ldx	[%i1 + CONTEXT_PSTATE], %l0
	wrpr	%l0, 0, %pstate

	ret
	 restore
FUNCTION_END(sparc_context_switch)


/*	Where a thread that has never run before begins.
 *
 *	Reached the first time sparc_context_switch() selects a thread, by way of the
 *	fabricated stack frame arch_thread_init_kthread_stack() left for it: the fill
 *	that follows "restore" loads this window from that frame, which is how the
 *	function to call and its argument arrive in %l0 and %l1 rather than being
 *	passed in the usual way.
 *
 *	%l registers specifically, because the fill puts the frame's first two words
 *	into %l0 and %l1, and using %i0 and %i1 instead would collide with the
 *	incoming-argument positions that a "save" further down would reuse.
 *
 *	Does not return. common_thread_entry() ends by exiting the thread, and there
 *	is nowhere to return to in any case -- the frame below this one is fabricated
 *	and terminates the chain.
 */
FUNCTION(sparc_thread_entry):
	mov	%l1, %o0
	jmpl	%l0, %o7
	 nop

	/* Only reachable if the entry function returns, which it must not. Stopping
	 * is better than falling into whatever follows in the text section.
	 */
1:	ba,a	1b
	 nop
FUNCTION_END(sparc_thread_entry)


/*	Report this file's idea of the context layout, so C++ can check it.
 *
 *	void sparc_context_offsets(uint64 *out);   // five entries
 */
FUNCTION(sparc_context_offsets):
	mov	CONTEXT_SP, %o1
	stx	%o1, [%o0 + (0 * 8)]
	mov	CONTEXT_PC, %o1
	stx	%o1, [%o0 + (1 * 8)]
	mov	SPARC_MINIMUM_FRAME_SIZE, %o1
	stx	%o1, [%o0 + (2 * 8)]
	mov	CONTEXT_PSTATE, %o1
	stx	%o1, [%o0 + (3 * 8)]
	mov	CONTEXT_PIL, %o1
	stx	%o1, [%o0 + (4 * 8)]
	retl
	 nop
FUNCTION_END(sparc_context_offsets)


/*	Drops into userspace, and does not come back.

	void sparc_enter_userspace(addr_t entry, addr_t stackPointer,
		uint64 arg1, uint64 arg2);

	The stack pointer is biased -- SPARC V9 keeps %sp at the frame address minus
	2047 -- because that is the form the hardware wants and the caller is the one
	that knows the frame it built.

	Everything that has to survive is moved into globals first, because the
	window registers are precisely what is about to be thrown away.
*/
FUNCTION(sparc_enter_userspace):
	mov	%o0, %g1			// entry
	mov	%o1, %g2			// stack pointer, biased
	mov	%o2, %g4			// arg1
	mov	%o3, %g5			// arg2

	/*	Nothing of the kernel's may be left in a register window.

		Two reasons, and the second is the one that bites. Userspace can read
		any window below the current one with `restore`, so kernel values left
		there are readable. And a spill from userspace writes a whole window to
		the user's stack -- so a kernel value still sitting in one would be
		written out to memory the user owns, whether it reads it or not.
	*/
	flushw

	/*	Trap level 1, where %tpc, %tnpc and %tstate exist to be written. `retry`
		takes it back to zero.

		`retry` and not `done`, and the difference is the whole of where userspace
		starts. RETRY sets PC from TPC and NPC from TNPC; DONE sets PC from TNPC
		and NPC from TNPC + 4 -- it exists to *skip* the instruction that trapped,
		which is what a system call return wants and the opposite of what an entry
		wants. Written with `done` first, this skipped the entry point's first
		instruction and userspace began one instruction in, with whatever this
		code had left in %g1.
	*/
	wrpr	%g0, 1, %tl

	wrpr	%g1, 0, %tpc
	add	%g1, 4, %g1
	wrpr	%g1, 0, %tnpc

	/*	TSTATE, which is what `done` restores PSTATE and CWP from: CWP in bits
		4:0, PSTATE in bits 20:8. Privilege dropped, FPU usable, and interrupts
		left exactly as the caller had them -- a thread being started for real
		arrives here with them on, and a caller that wants userspace to run
		uninterrupted can have that by asking for it rather than by editing this.
		CCR and ASI are left zero -- a thread that has never run has nothing in
		them, and handing userspace a condition code the kernel happened to leave
		behind is a leak for no benefit.

		CWP is this window's, so `done` returns into the window this code is
		running in. That is what makes the three moves below work: they write
		the registers userspace will start with, directly.
	*/
	rdpr	%pstate, %g3
	andn	%g3, PSTATE_PRIV, %g3
	or	%g3, PSTATE_PEF, %g3
	sllx	%g3, 8, %g3
	rdpr	%cwp, %g6
	or	%g3, %g6, %g3
	wrpr	%g3, 0, %tstate

	mov	%g2, %sp
	mov	%g4, %o0
	mov	%g5, %o1

	/*	The globals are not cleared, and %g7 is the reason: it holds the kernel's
		thread pointer, and the trap handlers read it to find out which thread
		took the trap. Zeroing it here would make the first trap out of userspace
		dereference null.

		So userspace can see it, and userspace changing it would make the next
		trap read the wrong thread. That is a real hole and it is where thread
		local storage will have to be dealt with -- SPARC userspace wants %g7 for
		its own thread pointer, which is the same register for the same reason.
		Noted rather than papered over; nothing runs in userspace yet that could
		exploit it. See sparc-port/USERSPACE_DESIGN.md.
	*/
	retry


/*	The program that runs in userspace, to prove userspace runs.

	Copied into a user page by sparc_test_userspace(), so it must be position
	independent and touch nothing but its own registers -- no stack, no globals
	other than the one the calling convention gives it, and no memory at all.

	The two constants have to agree with SPARC_SYSCALL_TRAP and
	SPARC_SYSCALL_TEST_EXIT in arch_int.h, which this file cannot include. A
	disagreement is caught by the test itself rather than by a check: the wrong
	trap number lands on an unhandled vector and the wrong index falls through to
	the "unimplemented call" path, and either way the test does not pass.

	It reports a value the kernel did not choose, so that "the trap happened"
	and "the trap happened where I meant it to" are distinguishable.
*/
#define USER_TEST_SYSCALL_TRAP		0x40
#define USER_TEST_SYSCALL_EXIT		0x555
#define USER_TEST_MAGIC			0x5ac

FUNCTION(sparc_user_test_program):
	mov	USER_TEST_SYSCALL_EXIT, %g1
	mov	USER_TEST_MAGIC, %o0
	ta	USER_TEST_SYSCALL_TRAP

	/*	Not reached: the handler does not return here. Spinning rather than
		falling through, because what follows in the page is whatever the page
		had in it.
	*/
1:	ba,a	1b
	 nop
FUNCTION(sparc_user_test_program_end):
