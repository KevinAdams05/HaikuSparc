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
#define CONTEXT_SP			0
#define CONTEXT_PC			8


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

	ldx	[%i1 + CONTEXT_SP], %i6
	ldx	[%i1 + CONTEXT_PC], %i7

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
 *	void sparc_context_offsets(uint64 *out);   // three entries
 */
FUNCTION(sparc_context_offsets):
	mov	CONTEXT_SP, %o1
	stx	%o1, [%o0 + (0 * 8)]
	mov	CONTEXT_PC, %o1
	stx	%o1, [%o0 + (1 * 8)]
	mov	SPARC_MINIMUM_FRAME_SIZE, %o1
	stx	%o1, [%o0 + (2 * 8)]
	retl
	 nop
FUNCTION_END(sparc_context_offsets)
