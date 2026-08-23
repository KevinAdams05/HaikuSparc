/* Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */


#include <stddef.h>
#include <string.h>

#include <arch_cpu.h>
#include <arch/thread.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <thread.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>

#include "SPARCVMTranslationMap.h"


extern "C" void sparc_context_switch(struct arch_context *from,
	struct arch_context *to);
extern "C" void sparc_thread_entry();
extern "C" void sparc_context_offsets(uint64 *out);
extern "C" void sparc_enter_userspace(addr_t entry, addr_t stackPointer,
	addr_t arg1, addr_t arg2, addr_t tlsPointer);


status_t
arch_thread_init(struct kernel_args *args)
{
	// Initialize the static initial arch_thread state (sInitialState).
	// Currently nothing to do, i.e. zero initialized is just fine.

	return B_OK;
}


status_t
arch_team_init_team_struct(Team *team, bool kernel)
{
	// Nothing to do. The structure is empty.
	return B_OK;
}


status_t
arch_thread_init_thread_struct(Thread *thread)
{
	// A Thread comes out of the slab allocator uninitialised -- 0xcc bytes, in a
	// debug build -- and arch_info has no constructor, so anything in it that is
	// read before being written has to be set here. This function existed for
	// exactly that and did nothing; the body was a commented-out memcpy.
	//
	// The window save area's count is such a field, and it is read by a spill
	// handler at trap level one. Left as poison it says the area holds 0xcc...
	// windows, the handler declines to write past the end -- correctly -- and
	// reports; the report then needs a register window of its own, cannot get
	// one because the spill it would take is the one that just failed, and the
	// machine ends up alternating between two trap levels forever.
	thread->arch_info.windowSave.count = 0;

	// Same reasoning, same allocator. Nothing pushed an interrupt frame before
	// this port started keeping the stack, so nobody noticed the index started as
	// poison; anything that reads it -- signal frames, the user debugger -- would
	// index off the end of a four-entry array.
	thread->arch_info.iframes.index = 0;

	return B_OK;
}


/*!	Checks that the assembly and this file agree about struct arch_context.

	The switch routine reaches the structure through numeric offsets, because the
	assembler cannot see a C++ declaration. Two copies of the same numbers is
	exactly the arrangement that drifts, and the failure would be a context switch
	that saves the stack pointer over the resume address -- which does not fault,
	it just returns to a stack address and executes whatever is there.
*/
static void
sparc_verify_context_layout()
{
	uint64 assembler[5];
	sparc_context_offsets(assembler);

	const uint64 declared[5] = {
		offsetof(arch_context, sp),
		offsetof(arch_context, pc),
		SPARC_MINIMUM_FRAME_SIZE,
		offsetof(arch_context, pstate),
		offsetof(arch_context, pil),
	};

	for (int i = 0; i < 5; i++) {
		if (assembler[i] != declared[i]) {
			panic("sparc context layout %d: arch_asm.S says %#" B_PRIx64
				", arch_thread_types.h says %#" B_PRIx64, i, assembler[i],
				declared[i]);
		}
	}

	if (SPARC_MINIMUM_FRAME_SIZE % 16 != 0)
		panic("sparc stack frames must be 16-byte aligned");
}


/*!	Fabricates a stack for a thread that has never run.

	The first switch to this thread will do what it does to any other: return
	with "ret; restore", which fills a window from the stack pointer it finds in
	the saved context. For a thread that has run before, that window was written
	there by a spill. For one that has not, it has to be written here.

	So this builds the frame a spill would have left, and puts the entry function
	and its argument in the first two words -- the positions a fill loads into %l0
	and %l1, which is where sparc_thread_entry() expects to find them.

	The frame address is what has to be 16-byte aligned, not the stack pointer:
	SPARC V9 biases %sp by 2047, so the two cannot both be aligned and it is the
	frame that the hardware cares about.
*/
void
arch_thread_init_kthread_stack(Thread* thread, void* _stack, void* _stackTop,
	void (*function)(void*), const void* data)
{
	sparc_verify_context_layout();

	// The frame sits at the top of the stack, below nothing.
	addr_t frame = ROUNDDOWN((addr_t)_stackTop - SPARC_MINIMUM_FRAME_SIZE, 16);
	uint64* saveArea = (uint64*)frame;

	memset(saveArea, 0, SPARC_MINIMUM_FRAME_SIZE);

	// A spill writes %l0-%l7 then %i0-%i7, in that order, so these are %l0 and
	// %l1 and the two after the locals are %i6 and %i7.
	saveArea[0] = (uint64)(addr_t)function;
	saveArea[1] = (uint64)(addr_t)data;

	// %i6 and %i7 -- the frame pointer and return address of the window
	// sparc_thread_entry() runs in. Both zero, which is not laziness: it
	// terminates a stack walk at the bottom of the thread rather than letting a
	// backtrace wander into whatever the memory used to hold. The entry function
	// never returns, so the return address is never used.
	saveArea[14] = 0;
	saveArea[15] = 0;

	thread->arch_info.context.sp = frame - SPARC_STACK_BIAS;

	// Less eight, because "ret" jumps to %i7 + 8.
	thread->arch_info.context.pc = (addr_t)&sparc_thread_entry - 8;

	// Privileged, FPU usable, interrupts *disabled*, interrupt level zero.
	//
	// Disabled is not a conservative guess, it is the contract. Every context
	// switch in Haiku happens at the same kind of point -- inside
	// scheduler_reschedule(), holding the thread's scheduler lock with
	// interrupts off -- and a thread being scheduled for the first time arrives
	// in exactly that state. common_thread_entry() then does the matching
	// release_spinlock() and enable_interrupts() itself.
	//
	// Starting one with interrupts on instead trips Haiku's own check
	// immediately: "release_spinlock: attempt to release lock with interrupts
	// enabled".
	thread->arch_info.context.pstate = SPARC_PSTATE_PRIV | SPARC_PSTATE_PEF;
	thread->arch_info.context.pil = 0;
}


/*!	Empties the window save area onto the user's stack.

	Called from the trap return path when the trap is going back to userspace, and
	the whole reason it is called from C rather than done in the spill handler is
	the trap level. Here it is zero, so a store to the user's stack that faults is
	an ordinary page fault -- pageable, killable, ordinary. In the spill handler it
	is one or deeper, where the same fault nests toward the watchdog reset section
	2.6 describes.

	It has to happen before the return, not lazily. On the way out the kernel gives
	CANRESTORE back from OTHERWIN, and a `restore` past the windows that are still
	live traps to a fill -- which reads the user's stack, because that is where a
	user window is supposed to be. If the copies were still sitting in here it
	would read whatever the stack held instead.

	Failure is reported and the slot dropped rather than retried. A user stack the
	kernel cannot write is a thread that should die, and killing it belongs with
	the signal work rather than here; losing the window is at least visible.
*/
extern "C" void
sparc_flush_user_windows()
{
	Thread* thread = thread_get_current_thread();
	if (thread == NULL)
		return;

	sparc_window_save& save = thread->arch_info.windowSave;
	if (save.count == 0)
		return;

	for (uint64 slot = 0; slot < save.count; slot++) {
		uint64* registers = save.slots[slot];
		addr_t stackPointer
			= (addr_t)registers[SPARC_WINDOW_SAVE_STACK_POINTER / sizeof(uint64)];

		status_t status = user_memcpy((void*)(stackPointer + SPARC_STACK_BIAS),
			registers, SPARC_WINDOW_SAVE_REGISTERS * sizeof(uint64));
		if (status != B_OK) {
			dprintf("sparc: could not flush a user window to %#" B_PRIxADDR
				": %s\n", stackPointer + SPARC_STACK_BIAS, strerror(status));
		}
	}

	save.count = 0;
}


/*!	Says where this thread's thread local storage lives.

	The block itself is not ours: thread.cpp carves TLS_SIZE off the top of the
	user stack area and leaves user_stack_size excluding it, so the block starts
	exactly where the stack ends. Every architecture computes the same address;
	what differs is how userspace is told about it.

	On SPARC it is told in %g7, which is where the platform keeps a thread pointer
	and which the kernel gave up needing when the trap entry started reading the
	current thread out of the trap data block instead. See sparc_enter_userspace().
*/
status_t
arch_thread_init_tls(Thread *thread)
{
	thread->user_local_storage
		= thread->user_stack_base + thread->user_stack_size;
	return B_OK;
}


void
arch_thread_context_switch(Thread *from, Thread *to)
{
	// Which team's low-half mappings the MMU should match from here on.
	//
	// The kernel's half needs nothing done to it: those mappings are Global, so
	// the hardware matches them whatever this register holds. That is the
	// shared-address-space decision in section 4.3 of the porting plan paying
	// off -- the switch is one store rather than a page table reload, and kernel
	// code keeps running through it without a flush.
	//
	// A kernel thread has no address space of its own and inherits whatever was
	// loaded, which is harmless for the same reason: it has no low-half mappings
	// to be confused about. Writing the kernel's id for it would cost a store
	// and buy nothing.
	// Where a trap out of userspace will build its frame. Per-thread, because
	// each thread has its own kernel stack, and recorded here rather than derived
	// in the trap entry because the entry runs on every interrupt.
	sparc_set_kernel_stack(to->kernel_stack_top);

	// And where the user window spill handler should put windows it cannot write
	// to the user's stack. Per-thread, so this is a pointer swap rather than a
	// copy -- see sparc_window_save.
	sparc_set_window_save(&to->arch_info.windowSave);

	VMAddressSpace* addressSpace = to->team->address_space;
	if (addressSpace != NULL) {
		SPARCVMTranslationMap* map = static_cast<SPARCVMTranslationMap*>(
			addressSpace->TranslationMap());
		if (map->Context() != SPARC_KERNEL_CONTEXT)
			sparc_switch_address_space(map->Context(), map->PageTable());
	}

	// The thread pointer is not set here either. The scheduler writes %g7 through
	// arch_thread_set_current_thread() immediately before calling this, so by the
	// time the switch runs it already names the incoming thread.
	sparc_context_switch(&from->arch_info.context, &to->arch_info.context);
}


void
arch_thread_dump_info(void *info)
{
}


/*!	Drops this thread into userspace at \a entry, and does not come back.

	The frame address is what has to be 16-byte aligned, not the stack pointer:
	SPARC V9 biases %sp by 2047, so the two cannot both be, and it is the frame
	the hardware and the ABI care about.

	A minimum frame is reserved below the stack top rather than handing userspace
	the top itself, because the first thing user code does is `save`, and a spill
	of that window writes sixteen registers to the frame this %sp names. Without
	the room, the spill writes above the stack.
*/
status_t
arch_thread_enter_userspace(Thread *thread, addr_t entry, void *arg1, void *arg2)
{
	addr_t stackTop = thread->user_stack_base + thread->user_stack_size;
	addr_t frame = ROUNDDOWN(stackTop - SPARC_MINIMUM_FRAME_SIZE, 16);

	sparc_enter_userspace(entry, frame - SPARC_STACK_BIAS, (addr_t)arg1,
		(addr_t)arg2, thread->user_local_storage);

	// sparc_enter_userspace() does not return: it leaves through `done`.
	panic("arch_thread_enter_userspace: returned from userspace entry");
	return B_ERROR;
}


bool
arch_on_signal_stack(Thread *thread)
{
	return false;
}


status_t
arch_setup_signal_frame(Thread *thread, struct sigaction *sa,
	struct signal_frame_data *signalFrameData)
{
	return B_ERROR;
}


int64
arch_restore_signal_frame(struct signal_frame_data* signalFrameData)
{
	return 0;
}


/**	Saves everything needed to restore the frame in the child fork in the
 *	arch_fork_arg structure to be passed to arch_restore_fork_frame().
 *	Also makes sure to return the right value.
 */

void
arch_store_fork_frame(struct arch_fork_arg *arg)
{
}


/** Restores the frame from a forked team as specified by the provided
 *	arch_fork_arg structure.
 *	Needs to be called from within the child team, ie. instead of
 *	arch_thread_enter_uspace() as thread "starter".
 *	This function does not return to the caller, but will enter userland
 *	in the child team at the same position where the parent team left of.
 */

void
arch_restore_fork_frame(struct arch_fork_arg *arg)
{
}



// #pragma mark - the context switch test


static int32 sPingPongCount;
static int32 sPingPongTurn;
static const int32 kPingPongRounds = 64;


/*!	One half of a pair of threads that take turns.

	Yields rather than blocking on a semaphore, because yielding is the thing
	being tested: each turn costs a voluntary trip through the scheduler and back,
	which is a full context switch out of this thread's stack and into the other's.

	The spin is bounded so that a switch which loses control -- returns to the
	wrong stack, or to a thread that never runs again -- fails the test instead of
	hanging the boot. That distinction matters here more than usual: there is no
	timer yet, so nothing else would ever take the CPU back.
*/
static status_t
sparc_ping_pong(void* data)
{
	int32 which = (int32)(addr_t)data;

	for (int32 round = 0; round < kPingPongRounds; round++) {
		int32 spins = 0;
		while (atomic_get(&sPingPongTurn) != which) {
			if (++spins > 1000000)
				return B_TIMED_OUT;
			thread_yield();
		}

		atomic_add(&sPingPongCount, 1);
		atomic_set(&sPingPongTurn, 1 - which);
	}

	return B_OK;
}


/*!	Proves two threads can hand control back and forth.

	The boot already demonstrates context switching by the time this runs -- there
	is a scheduler, and named threads have run on their own stacks. But "the
	machine got further" is not the same as knowing the switch is correct, and a
	switch that is subtly wrong corrupts a register somewhere and shows up as
	something unrelated much later.

	So: two threads, alternating a shared counter a fixed number of times, with
	the total checked against what it must be. A switch that returned to the
	wrong stack would take the count with it.
*/
void
sparc_test_context_switch()
{
	sPingPongCount = 0;
	sPingPongTurn = 0;

	thread_id first = spawn_kernel_thread(sparc_ping_pong, "sparc ping",
		B_NORMAL_PRIORITY, (void*)(addr_t)0);
	thread_id second = spawn_kernel_thread(sparc_ping_pong, "sparc pong",
		B_NORMAL_PRIORITY, (void*)(addr_t)1);

	if (first < 0 || second < 0) {
		dprintf("sparc_thread: could not spawn the context switch test\n");
		return;
	}

	resume_thread(first);
	resume_thread(second);

	// Waited for by yielding until the count arrives, rather than with
	// wait_for_thread(). What is being tested is whether control comes back, and
	// polling tests exactly that with nothing else involved -- no semaphores, no
	// thread-death bookkeeping, no timer, none of which this port has exercised
	// yet. An attempt with wait_for_thread() tripped an unrelated invariant in
	// the semaphore layer, which is worth chasing on its own and not from here.
	int32 expected = kPingPongRounds * 2;
	int32 spins = 0;
	while (atomic_get(&sPingPongCount) < expected) {
		if (++spins > 2000000)
			break;
		thread_yield();
	}

	int32 count = atomic_get(&sPingPongCount);
	bool ok = count == expected;

	dprintf("sparc_thread: two threads alternated, counter %" B_PRId32 " of %"
		B_PRId32 " after %" B_PRId32 " yields -- %s\n", count, expected, spins,
		ok ? "switched cleanly" : "WRONG");

	if (!ok) {
		panic("sparc context switch lost control: counter %" B_PRId32 " of %"
			B_PRId32, count, expected);
	}
}


/*!	A thread that exists only to finish and be waited for.

	Exits with exit_thread() rather than by returning the value.
	common_thread_entry() discards whatever a kernel thread's entry function
	returns and then calls thread_exit(), so a returned status never reaches the
	waiter -- the exit status is only what exit_thread() was given.
*/
static status_t
sparc_short_lived_thread(void *data)
{
	exit_thread((status_t)(addr_t)data);
	return B_OK;
}


/*!	Re-tests wait_for_thread(), which failed before setjmp() worked.

	The Phase 3 context switch test originally used wait_for_thread() and tripped
	"could acquire exit_sem for thread 5" -- acquire_sem_etc() returning B_OK on a
	semaphore created with a count of zero, which should have been impossible. The
	test was changed to poll instead, and the failure recorded as an open item
	rather than chased, since it was not a context switch problem.

	It is worth asking again rather than assumed fixed, because setjmp() and
	longjmp() were a bare `ret` at the time and that turned out to be behind two
	other things that had been written off -- the debug allocation pool's
	complaints and every kernel debugger command. A wild control transfer through
	the middle of semaphore bookkeeping would explain this one too.

	Checks the exit status as well as the return, because a wait that returned
	success without actually having waited would otherwise look identical -- and
	the status has to travel from the dying thread to the waiter, which is the
	bookkeeping the original failure was in.
*/
void
sparc_test_thread_wait()
{
	const status_t kExpected = 0x1234;

	thread_id thread = spawn_kernel_thread(sparc_short_lived_thread,
		"sparc short lived", B_NORMAL_PRIORITY, (void*)(addr_t)kExpected);
	if (thread < 0) {
		dprintf("sparc_thread: could not spawn the wait test\n");
		return;
	}

	resume_thread(thread);

	status_t exitStatus = 0;
	status_t waited = wait_for_thread(thread, &exitStatus);

	bool ok = waited == B_OK && exitStatus == kExpected;

	dprintf("sparc_thread: wait_for_thread returned %#x, thread exited %#x "
		"(wanted %#x) -- %s\n", waited, exitStatus, kExpected,
		ok ? "waited cleanly" : "WRONG");

	if (!ok) {
		panic("sparc: wait_for_thread returned %#x with exit status %#x, "
			"expected B_OK and %#x", waited, exitStatus, kExpected);
	}
}
