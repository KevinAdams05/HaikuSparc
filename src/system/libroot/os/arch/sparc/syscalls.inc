/*
** Copyright 2001-2007, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the MIT License.
*/

/*	A system call stub, and it is short because the convention was chosen to make
	it short.
 *
 *	The kernel takes the call index in %g1 and the first six arguments in
 *	%o0-%o5, which is where the C calling convention has already put them --
 *	nothing to marshal. Arguments past the sixth are read by the kernel from
 *	this frame at the offset the V9 ABI reserves for them, which is where the
 *	compiler has already put those too.
 *
 *	So the stub loads the index, traps, and returns. The kernel leaves the result
 *	in %o0, and a leaf function's %o0 is its caller's, so there is nothing to move
 *	on the way out either.
 *
 *	`retl` and not `ret`: this is a leaf, there is no `save`, and %i7 belongs to
 *	somebody else's frame. That mistake is what made setjmp fail for five phases
 *	-- see sparc-port/PROGRESS.md section 21.
 *
 *	The trap number must match SPARC_SYSCALL_TRAP in
 *	headers/private/kernel/arch/sparc/arch_int.h. A mismatch is not subtle: every
 *	system call in the system lands on a vector that reports an unhandled trap.
 *
 *	`set` rather than `mov` for the index, so the stub keeps working when the
 *	number of system calls passes what a 13-bit immediate holds.
 */
#define _SYSCALL(name, n) \
.globl name; \
.type name,%function; \
.align 4; \
name: \
	set n, %g1; \
	ta 0x40; \
	retl; \
	 nop; \
.size name, .-name

#define SYSCALL0(name, n) _SYSCALL(name, n)
#define SYSCALL1(name, n) _SYSCALL(name, n)
#define SYSCALL2(name, n) _SYSCALL(name, n)
#define SYSCALL3(name, n) _SYSCALL(name, n)
#define SYSCALL4(name, n) _SYSCALL(name, n)
#define SYSCALL5(name, n) _SYSCALL(name, n)
#define SYSCALL6(name, n) _SYSCALL(name, n)
#define SYSCALL7(name, n) _SYSCALL(name, n)
#define SYSCALL8(name, n) _SYSCALL(name, n)
#define SYSCALL9(name, n) _SYSCALL(name, n)
#define SYSCALL10(name, n) _SYSCALL(name, n)
#define SYSCALL11(name, n) _SYSCALL(name, n)
#define SYSCALL12(name, n) _SYSCALL(name, n)
#define SYSCALL13(name, n) _SYSCALL(name, n)
#define SYSCALL14(name, n) _SYSCALL(name, n)
#define SYSCALL15(name, n) _SYSCALL(name, n)
#define SYSCALL16(name, n) _SYSCALL(name, n)
#define SYSCALL17(name, n) _SYSCALL(name, n)
#define SYSCALL18(name, n) _SYSCALL(name, n)
#define SYSCALL19(name, n) _SYSCALL(name, n)
#define SYSCALL20(name, n) _SYSCALL(name, n)
