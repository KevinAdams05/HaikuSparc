/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <stddef.h>

#include <arch_debug.h>
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

	// Interrupts are off -- the trap cleared PSTATE.IE and the entry path raised
	// %pil to 15 -- and they stay that way across the reschedule. Restoring the
	// state explicitly afterwards is not a formality: rescheduling switches to
	// another thread and comes back later, and what the interrupt-enable bit
	// holds on the way back is whatever the other thread left it as.
	Thread *thread = thread_get_current_thread();
	cpu_status state = disable_interrupts();

	if (thread->post_interrupt_callback != NULL) {
		void (*callback)(void *) = thread->post_interrupt_callback;
		void *data = thread->post_interrupt_data;

		thread->post_interrupt_callback = NULL;
		thread->post_interrupt_data = NULL;

		restore_interrupts(state);
		callback(data);
	} else if (thread->cpu->invoke_scheduler) {
		SpinLocker schedulerLocker(thread->scheduler_lock);
		scheduler_reschedule(B_THREAD_READY);
		schedulerLocker.Unlock();
		restore_interrupts(state);
	} else {
		restore_interrupts(state);
	}

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
	sparc_test_preemption();
	sparc_test_backtrace();

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


// #pragma mark - the preemption test


static int32 sSpinnerCount;
static bool sStopSpinner;


/*!	Spins, and never gives up the CPU on purpose.

	No yield, no blocking call, nothing that reaches the scheduler. The only way
	this thread can lose the processor is for a timer interrupt to take it away,
	which is the point.
*/
static status_t
sparc_preemption_spinner(void *data)
{
	while (!sStopSpinner)
		atomic_add(&sSpinnerCount, 1);

	return B_OK;
}


/*!	Proves a periodic tick preempts a busy loop.

	The context switch test alternates two threads with thread_yield(), which
	demonstrates that switching works but says nothing about preemption: both
	threads there ask to be switched away. This one has neither thread ask.

	A spinner is started, and then this thread busy-waits for it without
	yielding. If nothing can take the CPU away from a running thread, whichever
	of the two started first keeps it and the counter stays at zero forever. A
	non-zero counter means the timer interrupt took the processor from one thread
	and gave it to the other, which is the whole of Phase 4.

	The deadline uses system_time(), which is the other half of this phase, so a
	failure of either shows up here.
*/
void
sparc_test_preemption()
{
	sSpinnerCount = 0;
	sStopSpinner = false;

	thread_id spinner = spawn_kernel_thread(sparc_preemption_spinner,
		"sparc spinner", B_NORMAL_PRIORITY, NULL);
	if (spinner < 0) {
		dprintf("sparc_int: could not spawn the preemption test\n");
		return;
	}

	resume_thread(spinner);

	bigtime_t start = system_time();
	const bigtime_t kDeadline = 2000000;

	while (atomic_get(&sSpinnerCount) == 0
		&& system_time() - start < kDeadline) {
		// Deliberately empty, and deliberately without a yield.
	}

	bigtime_t elapsed = system_time() - start;
	int32 count = atomic_get(&sSpinnerCount);
	sStopSpinner = true;

	dprintf("sparc_int: spinner reached %" B_PRId32 " after %" B_PRIdBIGTIME
		" us without either thread yielding -- %s\n", count, elapsed,
		count > 0 ? "preempted" : "NOT PREEMPTED");

	if (count == 0) {
		panic("sparc: a busy loop was never preempted in %" B_PRIdBIGTIME
			" us; the timer interrupt is not reaching the scheduler", elapsed);
	}
}
