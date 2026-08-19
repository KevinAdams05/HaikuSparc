/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <stddef.h>

#include <arch_thread_types.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <util/AutoLock.h>


extern "C" void sparc_iframe_offsets(uint64 *out);


/*	The SOFTINT register, ASR 22, and the two write-only aliases that set and
	clear bits in it without a read-modify-write (UltraSPARC-IIi manual TABLE
	11-9, printed p.123). Bits 15:1 are software interrupts at the matching
	levels; bit 0 is TICK_INT, which the %TICK comparison sets and which arrives
	as a level-14 interrupt.
*/
#define SOFTINT_TICK		(1ULL << 0)
#define SOFTINT_LEVEL_14	(1ULL << 14)

/*	Reached by ASR number rather than by name. The assembler rejects %softint,
	%set_softint, %clear_softint and %tick_cmpr with "architecture mismatch"
	under the architecture the kernel is compiled for, while accepting the same
	registers addressed numerically. The numbers are from the ASR table on
	printed p.83: SET_SOFTINT is 20, CLEAR_SOFTINT 21, SOFTINT 22 and
	TICK_CMPR 23.
*/


static inline uint64
sparc_read_softint()
{
	uint64 value;
	asm volatile("rd %%asr22, %0" : "=r"(value));
	return value;
}


static inline void
sparc_clear_softint(uint64 bits)
{
	asm volatile("wr %0, 0, %%asr21" : : "r"(bits));
}


/*!	Checks that the assembly and arch_int.h agree about struct iframe.

	The entry path reaches the frame through numeric offsets, and two copies of
	the same numbers drift. Here the drift would be silent and specific: the
	handler would restore the interrupted code's %g3 from where its %g4 was
	saved, and the interrupted code would resume with plausible-looking garbage.
*/
static void
sparc_verify_iframe_layout()
{
	uint64 assembler[14];
	sparc_iframe_offsets(assembler);

	const uint64 declared[14] = {
		offsetof(iframe, tstate), offsetof(iframe, tpc),
		offsetof(iframe, tnpc), offsetof(iframe, tt), offsetof(iframe, y),
		offsetof(iframe, g1), offsetof(iframe, g2), offsetof(iframe, g3),
		offsetof(iframe, g4), offsetof(iframe, g5), offsetof(iframe, g6),
		offsetof(iframe, g7), offsetof(iframe, pil), IFRAME_SIZEOF,
	};

	for (int i = 0; i < 14; i++) {
		if (assembler[i] != declared[i]) {
			panic("sparc iframe layout %d: arch_traps.S says %#" B_PRIx64
				", arch_int.h says %#" B_PRIx64, i, assembler[i], declared[i]);
		}
	}

	if (sizeof(iframe) > IFRAME_SIZEOF || IFRAME_SIZEOF % 16 != 0)
		panic("sparc iframe size %d does not fit a 16-byte aligned frame",
			(int)sizeof(iframe));
}


/*!	Every interrupt the kernel takes, from sparc_interrupt_entry().

	Runs at trap level zero on the interrupted thread's kernel stack, with
	PSTATE.IE still clear -- the hardware cleared it on the trap and nothing here
	sets it, so this cannot nest.

	Only the timer exists so far. The level-14 handler has to check both
	SOFTINT<14> and TICK_INT, because the two share a level: section 14.5.1 of
	the manual says so outright, and treating the level as implying the source
	would misreport a software interrupt as a clock tick the day one is used.
*/
extern "C" void
sparc_interrupt(struct iframe *frame)
{
	uint64 pending = sparc_read_softint();

	if ((pending & SOFTINT_TICK) != 0) {
		// Cleared before the handler runs, not after: the handler reprograms
		// the comparator, and clearing afterwards could drop a tick that
		// arrived in between.
		sparc_clear_softint(SOFTINT_TICK);
		timer_interrupt();
	}

	if ((pending & SOFTINT_LEVEL_14) != 0)
		sparc_clear_softint(SOFTINT_LEVEL_14);

	if ((pending & ~(SOFTINT_TICK | SOFTINT_LEVEL_14)) != 0) {
		panic("sparc: interrupt with nothing to service it, trap %#" B_PRIx64
			", softint %#" B_PRIx64, frame->tt, pending);
	}

	// The scheduler cannot be invoked from inside the trap: it would switch
	// stacks with a trap frame still on this one. Haiku's convention is to note
	// the request and act on it here, on the way out, which is also where a
	// thread's post-interrupt callback runs.
	if (debug_debugger_running())
		return;

	// Preemption from here is deliberately not done yet, and this is the honest
	// state of it rather than an oversight.
	//
	// Rescheduling from inside the interrupt means switching stacks with a trap
	// frame still on this one, and two problems showed up in that order. The
	// first was the trap level: this handler drops to level zero to run C, and a
	// second interrupt arriving before it returns wrote its own trap level over
	// this one, so the two returns drove the level below zero and QEMU stopped
	// with "Trap 0x0064 while trap level (-1) >= MAXTL". Raising %pil to 15 for
	// the duration fixed that, and it is the right thing regardless -- blocking
	// at the interrupt level does not depend on anyone else's discipline about
	// PSTATE.IE.
	//
	// What remains is window state. With preemption enabled the kernel faults
	// inside ordinary code -- BOpenHashTable::Insert, in the runs seen -- with
	// %i7 still pointing into this handler's caller, which means the register
	// window it is running in is not the one it thinks. The entry and exit paths
	// are correct for a handler that returns to what it interrupted; something
	// about them is not correct for one that returns somewhere else entirely.
	//
	// Without it, the timer still runs: system_time() advances, timer_interrupt()
	// fires, and Haiku's timers and timeouts work. What does not happen is a
	// thread being taken off the CPU against its will, so scheduling stays
	// cooperative, as it was before this file existed.
	(void)thread_get_current_thread;

}


status_t
arch_int_init(kernel_args *args)
{
	sparc_verify_iframe_layout();
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
