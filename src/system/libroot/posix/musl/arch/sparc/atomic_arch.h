// style-check: donor
//
// Deliberately follows musl's conventions rather than Haiku's, to match every
// sibling under musl/arch/: no copyright header, no include guard, and musl's
// "volatile int *p" pointer style. Keeping this file shaped like the others is
// what makes it comparable against upstream musl.

#define a_barrier a_barrier
static inline void a_barrier()
{
	// SPARC V9 running under TSO already orders load-load, load-store and
	// store-store, so store-load is the only ordering a barrier has to add.
	//
	// The membar sits in the delay slot of an always-taken branch to work
	// around erratum 51, which affects UltraSPARC I, II and IIi: a membar
	// (or any instruction that synchronizes on the store buffer draining)
	// issued late in the delay slot of a mispredicted control transfer can
	// stop instruction issue entirely. See appendix K of the UltraSPARC-IIi
	// User's Manual, and the same workaround in Linux's
	// arch/sparc/include/asm/barrier_64.h.
	__asm__ __volatile__ (
		"ba,pt	%%xcc, 1f\n\t"
		" membar #StoreLoad\n"
		"1:"
		: : : "memory");
}

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	// casa [rs1] #ASI_P, rs2, rd: rs2 is compared against the word at [rs1],
	// and rd is exchanged with memory on a match, or loaded from memory on a
	// mismatch. Either way rd ends up holding the previous contents, which is
	// exactly what a_cas must return. "cas" is the synthetic form that
	// supplies the primary ASI. See the SPARC V9 Architecture Manual A.9.
	__asm__ __volatile__ (
		"cas [%2], %3, %0"
		: "+r"(s), "=m"(*p)
		: "r"(p), "r"(t), "m"(*p));
	return s;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	// casx is the 64-bit form, for pointers.
	__asm__ __volatile__ (
		"casx [%2], %3, %0"
		: "+r"(s), "=m"(*(volatile void **)p)
		: "r"(p), "r"(t), "m"(*(volatile void **)p));
	return s;
}
