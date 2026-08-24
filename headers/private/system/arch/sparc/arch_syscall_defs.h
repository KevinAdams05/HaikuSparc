/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_ARCH_SPARC_SYSCALL_DEFS_H
#define _SYSTEM_ARCH_SPARC_SYSCALL_DEFS_H


/*	How userspace asks the kernel for something.

	SPARC V9's Tcc instructions produce trap types 0x100 + n for a software trap
	number n, and the architecture leaves the choice to the operating system. This
	is Haiku's.

	It lives in a header of its own -- not in a kernel header, and not in
	<asm_defs.h> -- because four separate places need it and they are not all the
	same kind of file: the kernel's trap table places its handler at 0x100 + this,
	the trap entry derives TRAP_SYSCALL from it, libroot's system call stubs are
	assembly that traps with it, and the commpage's signal trampoline is C that
	does. <asm_defs.h> would have been the obvious home and is not usable from
	C++, because it defines SYMBOL and so does elf_priv.h.

	A disagreement between any two of those four would send system calls to a
	vector that reports an unhandled trap. So there is one definition.

	Nothing but preprocessor definitions may go in here, for the same reason.
*/
#define SPARC_SYSCALL_TRAP		0x40


#endif	/* _SYSTEM_ARCH_SPARC_SYSCALL_DEFS_H */
