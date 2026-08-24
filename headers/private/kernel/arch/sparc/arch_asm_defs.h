/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_SPARC_ASM_DEFS_H
#define _KERNEL_ARCH_SPARC_ASM_DEFS_H


/*	Constants shared by more than one of the kernel's SPARC assembly files, and
	by the C that has to agree with them.

	Nothing here may be anything but a preprocessor definition: this is included
	from .S files as well as from C++, which is the whole point of it existing.
	Anything that needs a type belongs in arch_int.h or arch_mmu.h instead.
*/

/*	Which spill and fill handler a trap uses.

	WSTATE selects between the eight groups of each vector: the spill trap type is
	0x80 + 4 * WSTATE.NORMAL when OTHERWIN is zero and 0xa0 + 4 * WSTATE.OTHER
	when it is not. The kernel runs on group zero, whose handler stores with
	kernel privilege to a kernel stack; userspace runs on group one, whose handler
	stores through ASI_AS_IF_USER_PRIMARY so a stack the user cannot write faults
	instead of being written anyway.

	Defined once because the two values are set in different files -- the trap
	entry puts the kernel's back, sparc_enter_userspace() hands the user's over --
	and a disagreement between them would route a spill through a handler the
	other side did not intend, silently, since both work for a program whose stack
	happens to be present and writable.
*/
#define WSTATE_KERNEL			0
#define WSTATE_USER			1


#endif	/* _KERNEL_ARCH_SPARC_ASM_DEFS_H */
