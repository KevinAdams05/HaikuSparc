/* Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>


/*	setjmp, for a machine where the callee-saved registers save themselves.
 *
 *	`flushw` writes every window except this one out to the stack frame it
 *	belongs to, so by the time it returns, everything longjmp() will need to put
 *	back is in memory. That leaves two things worth recording: which stack, and
 *	where to resume.
 *
 *	A leaf function, so `retl` and not `ret`. That distinction is the whole of
 *	what was wrong before: `ret` is jmpl %i7+8, and %i7 belongs to the caller's
 *	caller, so a leaf using it returns two frames up instead of one. The previous
 *	version was a bare `ret` with nothing in its delay slot either, which meant
 *	the first instruction of whatever followed in the text section executed on the
 *	way out.
 *
 *	int setjmp(jmp_buf buffer)
 */
FUNCTION(setjmp):
FUNCTION(_setjmp):
FUNCTION(sigsetjmp):
	flushw

	stx	%sp, [%o0 + 0x00]
	stx	%o7, [%o0 + 0x08]

	retl
	 clr	%o0
FUNCTION_END(setjmp)
FUNCTION_END(_setjmp)
FUNCTION_END(sigsetjmp)

	/* The signal mask is not saved, which is what sigsetjmp's second argument
	 * asks about. Nothing in the kernel wants it and userland does not run yet;
	 * when it does, this is where setjmp_save_sigs.c gets called from.
	 */
