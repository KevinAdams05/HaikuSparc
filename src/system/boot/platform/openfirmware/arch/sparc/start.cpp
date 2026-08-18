/*
 * Copyright 2003-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2011, Alexander von Gluck, kallisti5@unixzen.com
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */


#include "start.h"

#include "machine.h"


void
determine_machine(void)
{
	gMachine = MACHINE_UNKNOWN;
}


/*!	Enables the floating-point unit.

	SPARC V9 gates floating point behind two independent enables, and an FP
	instruction encountered while either is clear raises an fp_disabled trap:
	PSTATE.PEF (bit 4) and FPRS.FEF (bit 2). See the UltraSPARC-IIi User's
	Manual section A.4, "Floating-Point Control".

	Open Firmware does not leave them set for a client program, and the loader
	does contain floating point -- the boot menu formats partition sizes with
	"%f", and GCC also emits FP register loads to move small structures around,
	which is how this first showed up. Note that the VIS graphics instructions
	share the FP register file and are gated by the same two bits.
*/
static void
enable_floating_point(void)
{
	uint64 pstate;
	asm volatile("rdpr %%pstate, %0" : "=r"(pstate));
	asm volatile("wrpr %0, 0, %%pstate" : : "r"(pstate | 0x010));
	asm volatile("wr %%g0, 0x4, %%fprs" : : : );
}


extern "C" void __attribute__((section(".text.start")))
_start(int _reserved, int _argstr, int _arglen, int _unknown,
	void *openFirmwareEntry)
{
	// According to the sparc bindings, OpenFirmware should have created
	// a stack of 8kB or higher for us at this point, and window traps are
	// operational so it's possible to call the openFirmwareEntry safely.
	// The bss segment is already cleared by the firmware as well.

	enable_floating_point();
		// before any code that might touch an FP register

	call_ctors();
		// call C++ constructors before doing anything else

	start(openFirmwareEntry);
}

