/*
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the Haiku License.
 */


#include <asm_defs.h>


// The smallest frame the SPARC V9 ABI allows.
#define MINIMUM_FRAME_SIZE	176


/*	longjmp, by the same pivot the context switch uses.
 *
 *	`save` gives this routine a window of its own, and `flushw` puts every other
 *	window into the stack it belongs to. This window's %i6 and %i7 are then
 *	exactly "which stack to return onto" and "where to return to", so writing the
 *	saved pair into them turns the ordinary epilogue into the jump: `ret` goes to
 *	the new %i7 + 8, which is the instruction after the call to setjmp, and
 *	`restore` drops into a window filled from the new %i6.
 *
 *	Unwinding many frames therefore costs nothing extra. Everything between here
 *	and the target is already in memory, and the windows refill from it on demand
 *	as the target returns.
 *
 *	The return value rides out on the restore. `restore rs1, rs2, rd` computes in
 *	the old window and writes rd in the new one, so it can hand setjmp's caller
 *	its value in %o0 -- which is otherwise unreachable from here, since this
 *	window's %o0 is not that one.
 *
 *	void longjmp(jmp_buf buffer, int value)
 */
FUNCTION(siglongjmp):
FUNCTION(longjmp):
FUNCTION(_longjmp):
	save	%sp, -MINIMUM_FRAME_SIZE, %sp

	flushw

	/* setjmp() returns zero, so longjmp() must not: a caller distinguishing the
	 * two by the return value would otherwise loop forever. POSIX has longjmp
	 * substitute 1 for a value of 0.
	 */
	mov	%i1, %l1
	movrz	%l1, 1, %l1

	ldx	[%i0 + 0x00], %i6
	ldx	[%i0 + 0x08], %i7

	ret
	 restore %g0, %l1, %o0
FUNCTION_END(siglongjmp)
FUNCTION_END(longjmp)
FUNCTION_END(_longjmp)

#pragma weak longjmp=siglongjmp
