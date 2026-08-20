/*
 * Copyright 2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_SPARC_DEBUG_H
#define _KERNEL_ARCH_SPARC_DEBUG_H


#include <SupportDefs.h>


/*	What the debugger records about where it was called from.

	One field, because on SPARC the rest of the machine state is already on the
	stack: the register windows are flushed there, and every frame's save area
	holds the sixteen registers that matter. A stack pointer is enough to find
	all of it.
*/
struct arch_debug_registers {
	addr_t	sp;
};


#ifdef __cplusplus
// In arch_debug.cpp. Called once, from arch_int_init_post_device_manager().
extern void sparc_test_backtrace();
#endif


#endif	// _KERNEL_ARCH_SPARC_DEBUG_H
