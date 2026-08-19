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
};

#define IFRAME_SIZEOF	112
	// sizeof(struct iframe) rounded up to a 16-byte multiple, which is what
	// stack frames have to be aligned to.

// Trap types 0x41 through 0x4f are interrupt_level_1 through _15, one entry
// each rather than the groups of four the busier traps get.
#define TRAP_INTERRUPT_LEVEL_BASE	0x40
#define TRAP_INTERRUPT_LEVEL_14		0x4e

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


#endif /* _KERNEL_ARCH_SPARC_INT_H */
