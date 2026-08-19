/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Alexander von Gluck IV <kallisti5@unixzen.com>
 */
#ifndef _KERNEL_ARCH_SPARC_ATOMIC_H
#define _KERNEL_ARCH_SPARC_ATOMIC_H


/*	SPARC V9 runs under Total Store Order, which already guarantees load-load,
	load-store and store-store ordering. Store-load is the only ordering a
	barrier has to add, so only the full barrier needs an instruction.

	The read and write barriers still need the compiler told, which is what the
	empty asm with a "memory" clobber does. That is not a formality: these were
	previously empty function bodies, which let the compiler hoist a load out of
	a loop that was waiting for another thread to store -- the exact thing a
	barrier exists to prevent, and invisible in a single-threaded boot.

	The full barrier's membar sits in the delay slot of an always-taken branch to
	work around erratum 51, which affects UltraSPARC I, II and IIi: a membar
	issued late in the delay slot of a mispredicted control transfer can stop
	instruction issue entirely. Appendix K of the UltraSPARC-IIi User's Manual,
	and the same workaround in Linux's arch/sparc/include/asm/barrier_64.h. The
	musl header src/system/libroot/posix/musl/arch/sparc/atomic_arch.h carries the
	same code for userspace.
*/
static inline void
memory_read_barrier_inline(void)
{
	__asm__ __volatile__("" : : : "memory");
}


static inline void
memory_write_barrier_inline(void)
{
	__asm__ __volatile__("" : : : "memory");
}


static inline void
memory_full_barrier_inline(void)
{
	__asm__ __volatile__(
		"ba,pt	%%xcc, 1f\n\t"
		" membar #StoreLoad\n"
		"1:"
		: : : "memory");
}


#define memory_read_barrier		memory_read_barrier_inline
#define memory_write_barrier	memory_write_barrier_inline
#define memory_full_barrier		memory_full_barrier_inline


#endif	// _KERNEL_ARCH_SPARC_ATOMIC_H

