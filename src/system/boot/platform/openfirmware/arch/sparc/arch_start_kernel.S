/*
 * Copyright 2005, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#define FUNCTION(x) .global x; .type x,@function; x

/*	status_t arch_start_kernel(struct kernel_args *kernelArgs,
		addr_t kernelEntry, addr_t kernelStackTop);

	o0	- kernelArgs
	o1	- kernelEntry
	o2	- kernelStackTop
*/
FUNCTION(arch_start_kernel):
	save  %sp, -176, %sp

	// Now we have
	// - Kernel args in i0
	// - Kernel entry in i1
	// - Kernel stack top in i2

	// Set the stack, adding the stack bias
	sub %i2, 2047, %sp // apply stack bias

	// We need to call the kernel with:
	// o0 = kernel args
	// o1 = current CPU
	mov %i0, %o0
	// Current CPU is always 0 on boot
	mov %g0, %o1

	// Enter the kernel with interrupts disabled, which is what it expects:
	// smp_trap_non_boot_cpus() asserts !are_interrupts_enabled() before the
	// kernel has any means of reporting the failure. Open Firmware runs with
	// them enabled and nothing else clears them on the way in, so the very
	// first assertion the kernel makes used to fail.
	// PSTATE.IE is bit 1 (UltraSPARC-IIi User's Manual, TABLE 14-12).
	rdpr %pstate, %g1
	andn %g1, 2, %g1
	wrpr %g1, 0, %pstate

	// Jump into the kernel, store return address in o7
	jmpl %i1, %o7
	nop // branch delay slot...

	// We should never get here, but in case the kernel returns, attempt to
	// exit cleanly.

	// Forward the return code to the caller
	mov %o0, %i0

	// Since we saved our context at the start of this function,
	// a simple "return" should do the job and restore the previous register
	// window and put us back on the old stack
	return %i7 + 8
	nop // Branch delay slot...
