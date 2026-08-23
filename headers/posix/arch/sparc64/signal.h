/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ARCH_SPARC64_SIGNAL_H_
#define _ARCH_SPARC64_SIGNAL_H_


/*
 * Architecture-specific structure passed to signal handlers
 */

#if __sparc64__

struct vregs
{
	// ulong g0; // always 0, so no need to save
	ulong g1;
	ulong g2;
	ulong g3;
	ulong g4;
	ulong g5;
	ulong g6;
	ulong g7;
	ulong o0;
	ulong o1;
	ulong o2;
	ulong o3;
	ulong o4;
	ulong o5;
	ulong sp;
	ulong o7;
	ulong l0;
	ulong l1;
	ulong l2;
	ulong l3;
	ulong l4;
	ulong l5;
	ulong l6;
	ulong l7;
	ulong i0;
	ulong i1;
	ulong i2;
	ulong i3;
	ulong i4;
	ulong i5;
	ulong fp;
	ulong i7;

	// Where to resume, and both halves of it: SPARC needs the next program
	// counter as well as the current one, because an interrupted instruction can
	// be in a delay slot and the following instruction is then not pc + 4.
	ulong pc;
	ulong npc;

	// The condition codes and the multiply/divide register, which are part of
	// the interrupted computation. Taken from TSTATE rather than exposing TSTATE
	// itself, which also carries privileged fields no signal handler should see.
	ulong ccr;
	ulong y;

	// TODO: sparc: Fix floats in vregs
};


#endif /* __sparc64__ */

#endif /* _ARCH_SPARC64_SIGNAL_H_ */
