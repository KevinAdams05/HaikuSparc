/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <arch_thread_types.h>
#include <interrupts.h>


status_t
arch_int_init(kernel_args *args)
{
	return B_OK;
}


status_t
arch_int_init_post_vm(kernel_args *args)
{
	return B_OK;
}


status_t
arch_int_init_post_device_manager(struct kernel_args *args)
{
	// Runs inside main2(), which is the first thread the scheduler ever picks.
	// That matters: nothing is scheduled before scheduler_start(), as main.cpp
	// says where it spawns this thread, so the obvious earlier hooks -- including
	// arch_platform_init_post_thread(), where this was first put -- can create
	// threads and resume them and watch them never run.
	sparc_test_context_switch();

	return B_OK;
}


status_t
arch_int_init_io(kernel_args* args)
{
	return B_OK;
}


void
arch_int_enable_io_interrupt(int32 irq)
{
}


void
arch_int_disable_io_interrupt(int32 irq)
{
}


int32
arch_int_assign_to_cpu(int32 irq, int32 cpu)
{
	// Not yet supported.
	return 0;
}
