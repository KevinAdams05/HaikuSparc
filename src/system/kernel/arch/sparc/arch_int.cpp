/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <stddef.h>
#include <string.h>

#include <arch_debug.h>
#include <arch_thread_types.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <vm/vm.h>
#include <vm/vm_priv.h>
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
	uint64 assembler[17];
	sparc_iframe_offsets(assembler);

	const uint64 declared[17] = {
		offsetof(iframe, tstate), offsetof(iframe, tpc),
		offsetof(iframe, tnpc), offsetof(iframe, tt), offsetof(iframe, y),
		offsetof(iframe, g1), offsetof(iframe, g2), offsetof(iframe, g3),
		offsetof(iframe, g4), offsetof(iframe, g5), offsetof(iframe, g6),
		offsetof(iframe, g7), offsetof(iframe, pil), offsetof(iframe, sfsr),
		offsetof(iframe, sfar), offsetof(iframe, tagAccess), IFRAME_SIZEOF,
	};

	for (int i = 0; i < 17; i++) {
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
	sparc_test_user_memory();

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


/*!	The traps that mean the VM has to look at an address.

	Runs at trap level zero on the faulting thread's own stack, which is what
	makes it possible to block here at all -- and blocking is normal for a page
	fault, since resolving one can mean paging something in or taking a mutex.

	Interrupts are re-enabled if the faulting context had them enabled, and not
	otherwise. That is not caution for its own sake: a thread that blocks with
	interrupts disabled never gets the CPU back, because the timer that would
	preempt whoever it is waiting for cannot fire. And a context that faulted
	*with* interrupts already off is one that must not block -- user_memcpy() is
	the case that matters -- which is exactly what the checks below handle
	instead.
*/
extern "C" void
sparc_page_fault(struct iframe *frame)
{
	uint64 trap = frame->tt;
	bool isInstructionMiss = (trap & ~3) == TRAP_INSTRUCTION_MMU_MISS;
	bool isDataMiss = (trap & ~3) == TRAP_DATA_MMU_MISS;
	bool isProtection = (trap & ~3) == TRAP_DATA_PROTECTION;
	bool isExecute = trap == TRAP_INSTRUCTION_ACCESS || isInstructionMiss;
	bool isUser = (frame->tstate & TSTATE_PRIV) == 0;

	// Where the address comes from depends on which trap this is. The "fast" MMU
	// traps -- the two misses and the protection trap -- leave it in Tag Access
	// rather than in a fault address register, which is most of what makes them
	// fast.
	addr_t address;
	if (isDataMiss || isInstructionMiss || isProtection) {
		address = (addr_t)(frame->tagAccess & ~(uint64)0x1fff);
	} else if (trap == TRAP_INSTRUCTION_ACCESS) {
		// An instruction fetch has no address register of its own; it faulted on
		// the address it was fetching from.
		address = (addr_t)frame->tpc;
	} else {
		address = (addr_t)frame->sfar;
	}

	// A protection trap is a write by definition -- it is what the hardware
	// raises when a store finds a read-only entry. Otherwise SFSR says, and it
	// is written for TLB misses too: SFSR's fault type has a bit for exactly
	// that case.
	bool isWrite = isProtection || (frame->sfsr & SFSR_WRITE) != 0;

	Thread *thread = thread_get_current_thread();

	// Faulting inside the kernel debugger is not recoverable by paging, and
	// blocking there would hang the machine with everything else stopped. The
	// debugger installs a handler for exactly this.
	if (debug_debugger_running()) {
		if (thread != NULL && thread->fault_handler != NULL) {
			debug_set_page_fault_info(address, (addr_t)frame->tpc,
				isWrite ? DEBUG_PAGE_FAULT_WRITE : 0);
			frame->tpc = (uint64)(addr_t)thread->fault_handler;
			frame->tnpc = frame->tpc + 4;
			return;
		}

		panic("sparc: page fault in the debugger with no fault handler, "
			"address %#" B_PRIxADDR " from pc %#" B_PRIx64, address,
			frame->tpc);
		return;
	}

	// A fault with interrupts already disabled cannot be resolved by blocking,
	// so it had better be a fault somebody was expecting. user_memcpy() and the
	// rest of user_access() set a handler precisely so that a bad user address
	// becomes an error return rather than a dead kernel.
	if ((frame->tstate & TSTATE_IE) == 0) {
		if (thread != NULL && thread->fault_handler != NULL) {
			frame->tpc = (uint64)(addr_t)thread->fault_handler;
			frame->tnpc = frame->tpc + 4;
			return;
		}

		panic("sparc: page fault with interrupts disabled and no fault handler, "
			"address %#" B_PRIxADDR " from pc %#" B_PRIx64 " (trap %#" B_PRIx64
			", sfsr %#" B_PRIx64 ")", address, frame->tpc, frame->tt,
			frame->sfsr);
		return;
	}

	enable_interrupts();

	addr_t newInstructionPointer = 0;
	vm_page_fault(address, (addr_t)frame->tpc, isWrite, isExecute, isUser,
		&newInstructionPointer);

	if (newInstructionPointer != 0) {
		frame->tpc = (uint64)newInstructionPointer;
		frame->tnpc = frame->tpc + 4;
	}

	disable_interrupts();
}


// #pragma mark - the user memory test


/*!	Verifies the shared address space model, and that a bad user address is an
	error rather than a dead kernel.

	The porting plan singles this out as the thing to check early: section 4.3
	chose one address space with kernel mappings marked Global and user mappings
	tagged by context, precisely so that no shared Haiku code needs changing. If
	that model did not hold -- if IS_USER_ADDRESS and IS_KERNEL_ADDRESS could not
	both be simple range checks -- the scope of userspace support would be
	completely different.

	The second half is the page fault handler doing its job. user_access() sets
	thread->fault_handler and longjmps out of a fault, which means a bad address
	has to arrive as a fault the handler recognises. Before there was a handler
	this would have been an unhandled trap and a panic; before setjmp worked it
	would have been worse than that.
*/
void
sparc_test_user_memory()
{
	// The two ranges must not overlap, and each must recognise its own.
	bool ranges = IS_KERNEL_ADDRESS(KERNEL_BASE)
		&& IS_KERNEL_ADDRESS(KERNEL_TOP)
		&& !IS_KERNEL_ADDRESS(USER_BASE)
		&& IS_USER_ADDRESS(USER_BASE)
		&& IS_USER_ADDRESS(USER_TOP)
		&& !IS_USER_ADDRESS(KERNEL_BASE);

	dprintf("sparc_int: address space: user %#lx-%#lx, kernel %#lx-%#lx -- %s\n",
		(addr_t)USER_BASE, (addr_t)USER_TOP, (addr_t)KERNEL_BASE,
		(addr_t)KERNEL_TOP, ranges ? "disjoint" : "WRONG");

	if (!ranges) {
		panic("sparc: the user and kernel address ranges overlap or do not "
			"recognise their own addresses");
		return;
	}

	char buffer[16];
	const char source[16] = "sparc user copy";

	// A copy that must work, so that the failures below mean something. A kernel
	// address is legitimate here: user_memcpy() only refuses a range that
	// *crosses* the user/kernel boundary, not one entirely on either side, and
	// kernel code does use it for kernel-to-kernel copies.
	status_t good = user_memcpy(buffer, source, sizeof(buffer));
	bool copied = good == B_OK && memcmp(buffer, source, sizeof(buffer)) == 0;

	// A user address that is certainly not mapped. Not the low megabyte, which
	// looks unmapped and is not -- the boot loader's identity mapping covers
	// everything below 0x802000, and it survives into the kernel's page table.
	// A gigabyte up is past everything the loader touched and still well below
	// KERNEL_BASE.
	status_t unmapped = user_memcpy(buffer, (const void*)0x40000000,
		sizeof(buffer));

	// And a range straddling the boundary, which has to be refused by the range
	// check rather than by faulting: that check is what stops userland handing
	// the kernel a pointer that starts in its own space and ends in the
	// kernel's.
	status_t straddling = user_memcpy(buffer,
		(const void*)(KERNEL_BASE - sizeof(buffer) / 2), sizeof(buffer));

	bool ok = copied && unmapped == B_BAD_ADDRESS
		&& straddling == B_BAD_ADDRESS;

	dprintf("sparc_int: user_memcpy good %#x, unmapped %#x, straddling %#x "
		"-- %s\n", good, unmapped, straddling,
		ok ? "faults caught" : "WRONG");

	if (!ok) {
		panic("sparc: user_memcpy gave %#x for a valid copy, %#x for an "
			"unmapped user address and %#x for a straddling range; wanted OK, "
			"B_BAD_ADDRESS, B_BAD_ADDRESS", good, unmapped, straddling);
	}
}
