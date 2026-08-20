/*
 * Copyright 2019, Haiku, Inc. All rights reserved.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Adrien Destugues <pulkomandy@pulkomandy.tk>
 */


#include <arch/debug.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <arch_thread_types.h>
#include <debug.h>
#include <debug_heap.h>
#include <elf.h>
#include <kernel.h>
#include <kimage.h>
#include <thread.h>


/*	A SPARC V9 stack frame's register save area.

	Every frame reserves room for the sixteen registers a window spill writes:
	the eight locals first, then the eight ins. It sits at %sp + 2047, because
	SPARC V9 biases the stack pointer -- %sp holds the frame address minus 2047,
	so the frame itself is above where %sp points.

	Two of those sixteen are the whole of a backtrace. %i6 is the caller's %sp,
	which is the next frame, and %i7 is the address of the `call` instruction
	that got here.

	Note what %i7 is not: it is not the return address. `call` records its own
	address and the return goes to %i7 + 8. That is better than the usual
	arrangement rather than worse -- the value already points *inside* the
	calling function, so it symbolises correctly even when the call is the last
	instruction in it, which is exactly the case where a return address lands in
	the next function and names the wrong one.
*/
struct sparc_frame {
	uint64	local[8];
	uint64	in[8];
};

#define SPARC_FRAME_CALLER_SP	6
#define SPARC_FRAME_CALL_SITE	7

#define SPARC_STACK_BIAS_LOCAL	2047


/*!	Forces every live register window out to the stack it belongs to.

	This is the one thing a SPARC stack walk cannot skip. A frame's save area
	holds nothing until that window has been spilled, and the most recent windows
	are usually still in the register file -- which is the whole point of register
	windows. Walking without flushing reads whatever those save areas held
	before, which is to say a plausible-looking backtrace of an older call chain.

	**It does not flush the current window**, and that is not a detail that can be
	glossed. `flushw` writes every window *except* the one executing it, so the
	innermost frame's save area is still stale afterwards and its %i6 and %i7
	have to be read from the registers instead. Getting this wrong is not subtle
	in its effect but is very subtle in its cause: the walk finds exactly one
	frame and stops, because the stale save area's "caller stack pointer" leads
	nowhere.

	A thread that is not running needs no flush at all: the context switch that
	took it off the CPU executed flushw for its own reasons.
*/
static inline void
flush_register_windows()
{
	asm volatile("flushw" : : : "memory");
}


/*!	The current window's stack pointer, frame pointer and call site.

	Read together in one block so they describe the same window. %fp is %i6, the
	caller's stack pointer, and %i7 is the call that reached here.
*/
static inline void
current_frame(addr_t* _sp, addr_t* _callerSp, addr_t* _callSite)
{
	asm volatile("mov %%sp, %0\n\t"
		"mov %%fp, %1\n\t"
		"mov %%i7, %2"
		: "=r"(*_sp), "=r"(*_callerSp), "=r"(*_callSite));
}


static bool
is_kernel_stack_address(Thread* thread, addr_t address)
{
	// Early in boot there is no thread pointer, and no stack but the kernel's.
	if (thread == NULL || thread->kernel_stack_top == 0)
		return IS_KERNEL_ADDRESS(address);

	return address >= thread->kernel_stack_base
		&& address < thread->kernel_stack_top;
}


/*!	Reads one frame and reports where the next one is.

	The alignment check is not decoration. A SPARC V9 frame address must be
	16-byte aligned, and %sp is that address less the bias, so a stack pointer
	worth following satisfies (sp + 2047) % 16 == 0. Garbage almost never does,
	which makes this a cheap way to stop a walk that has left the rails rather
	than following it into unmapped memory.
*/
static status_t
get_next_frame(addr_t sp, addr_t* _nextSp, addr_t* _callSite, Thread* thread)
{
	if (sp == 0)
		return B_BAD_ADDRESS;

	addr_t frameAddress = sp + SPARC_STACK_BIAS_LOCAL;
	if ((frameAddress % 16) != 0)
		return B_BAD_ADDRESS;

	if (!is_kernel_stack_address(thread, frameAddress)
		|| !is_kernel_stack_address(thread,
			frameAddress + sizeof(sparc_frame) - 1)) {
		return B_BAD_ADDRESS;
	}

	sparc_frame frame;
	memcpy(&frame, (void*)frameAddress, sizeof(frame));

	*_callSite = frame.in[SPARC_FRAME_CALL_SITE];
	*_nextSp = frame.in[SPARC_FRAME_CALLER_SP];

	return B_OK;
}


/*!	Names an address, when it is safe to ask.

	elf_debug_lookup_symbol_address() asserts that the image mutex is held, which
	is true inside the kernel debugger -- where locking is suspended and the
	assertion reads as "nobody else is running" -- and not true of ordinary
	kernel code. Calling it anyway from a normal thread trips that assertion
	rather than returning an error, so the check has to happen here.

	Outside the debugger a frame therefore prints as a bare address. That is less
	useful and still worth printing: the addresses alone are enough to resolve by
	hand with addr2line, which is how every backtrace in this port was read
	before there was a stack walker at all.
*/
static status_t
lookup_symbol(Thread* thread, addr_t address, addr_t* _baseAddress,
	const char** _symbolName, const char** _imageName, bool* _exactMatch)
{
	if (!debug_debugger_running())
		return B_NOT_ALLOWED;

	return elf_debug_lookup_symbol_address(address, _baseAddress, _symbolName,
		_imageName, _exactMatch);
}


/*!	Prints wherever the caller can actually read it.

	kprintf() is the debugger's output and goes nowhere when the debugger is not
	running, which makes it exactly wrong for a backtrace printed from ordinary
	kernel code -- the call succeeds and nothing appears.
*/
static void
trace_printf(const char* format, ...)
{
	char buffer[256];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (debug_debugger_running())
		kprintf("%s", buffer);
	else
		dprintf("%s", buffer);
}


static void
print_stack_frame(Thread* thread, addr_t callSite, addr_t sp, addr_t nextSp,
	int32 callIndex)
{
	const char* symbol;
	const char* image;
	addr_t baseAddress;
	bool exactMatch;

	addr_t frameSize = nextSp > sp ? nextSp - sp : 0;

	if (lookup_symbol(thread, callSite, &baseAddress, &symbol, &image,
			&exactMatch) == B_OK) {
		trace_printf("%2" B_PRId32 " %0*lx (+%5ld) %0*lx  <%s> %s%s + %#lx\n",
			callIndex, B_PRINTF_POINTER_WIDTH, sp, frameSize,
			B_PRINTF_POINTER_WIDTH, callSite, image, symbol,
			exactMatch ? "" : " (nearest)", callSite - baseAddress);
	} else {
		trace_printf("%2" B_PRId32 " %0*lx (+%5ld) %0*lx\n", callIndex,
			B_PRINTF_POINTER_WIDTH, sp, frameSize, B_PRINTF_POINTER_WIDTH,
			callSite);
	}
}


/*!	Walks a thread's stack, reporting each frame.

	The first frame is passed in rather than read from memory, because after
	`flushw` it is the one frame whose save area has not been written. Everything
	from the second onward comes from the stack.

	Returns the number of frames reported. \a callback is given the call site of
	each, deepest first.
*/
static int32
walk_stack(Thread* thread, addr_t sp, addr_t callerSp, addr_t callSite,
	int32 skipFrames, int32 maxFrames,
	void (*callback)(void* cookie, int32 index, addr_t sp, addr_t nextSp,
		addr_t callSite), void* cookie)
{
	int32 found = 0;
	int32 index = 0;

	// Bounded independently of the alignment and range checks, so that a stack
	// whose frame chain loops back on itself stops the walk rather than the
	// debugger.
	const int32 kMaxWalk = 512;

	for (int32 step = 0; step < kMaxWalk && found < maxFrames; step++) {
		if (callSite != 0) {
			if (index >= skipFrames) {
				if (callback != NULL)
					callback(cookie, found, sp, callerSp, callSite);
				found++;
			}
			index++;
		}

		if (callerSp == 0)
			break;

		// A frame chain only ever grows upward. Anything else is a loop or
		// garbage, and following it would report the same frames forever.
		if (callerSp <= sp)
			break;

		addr_t nextSp;
		addr_t nextCallSite;
		if (get_next_frame(callerSp, &nextSp, &nextCallSite, thread) != B_OK)
			break;

		sp = callerSp;
		callerSp = nextSp;
		callSite = nextCallSite;
	}

	return found;
}


struct print_cookie {
	Thread*	thread;
};


static void
print_frame_callback(void* cookie, int32 index, addr_t sp, addr_t nextSp,
	addr_t callSite)
{
	print_stack_frame(((print_cookie*)cookie)->thread, callSite, sp, nextSp,
		index);
}


static int
stack_trace_command(int argc, char** argv)
{
	Thread* thread = thread_get_current_thread();

	flush_register_windows();

	trace_printf("stack trace for thread %" B_PRId32 " \"%s\"\n",
		thread != NULL ? thread->id : -1,
		thread != NULL ? thread->name : "?");

	addr_t sp;
	addr_t callerSp;
	addr_t callSite;
	current_frame(&sp, &callerSp, &callSite);

	print_cookie cookie = { thread };
	int32 count = walk_stack(thread, sp, callerSp, callSite, 0, 64,
		print_frame_callback, &cookie);

	if (count == 0)
		trace_printf("no frames found\n");

	return 0;
}


void
arch_debug_stack_trace(void)
{
	stack_trace_command(0, NULL);
}


struct collect_cookie {
	addr_t*	addresses;
	int32	count;
};


static void
collect_frame_callback(void* cookie, int32 index, addr_t sp, addr_t nextSp,
	addr_t callSite)
{
	collect_cookie* collect = (collect_cookie*)cookie;
	collect->addresses[collect->count++] = callSite;
}


int32
arch_debug_get_stack_trace(addr_t* returnAddresses, int32 maxCount,
	int32 skipIframes, int32 skipFrames, uint32 flags)
{
	if (maxCount <= 0)
		return 0;

	// Interrupt frames are not distinguished yet: the walk follows the ordinary
	// frame chain, and an interrupt entry appears in it like any other call
	// because it builds a normal frame. Skipping "iframes" therefore has
	// nothing to select on, and asking for it skips ordinary frames instead,
	// which is the conservative reading.
	if (skipIframes > 0)
		skipFrames += skipIframes;

	// Only the current thread's windows can be flushed, and only the current
	// thread's are ever still in the register file.
	flush_register_windows();

	addr_t sp;
	addr_t callerSp;
	addr_t callSite;
	current_frame(&sp, &callerSp, &callSite);

	collect_cookie cookie = { returnAddresses, 0 };
	walk_stack(thread_get_current_thread(), sp, callerSp, callSite, skipFrames,
		maxCount, collect_frame_callback, &cookie);

	return cookie.count;
}


struct contains_cookie {
	Thread*		thread;
	addr_t		start;
	addr_t		end;
	const char*	symbol;
	bool		found;
};


static void
contains_frame_callback(void* cookie, int32 index, addr_t sp, addr_t nextSp,
	addr_t callSite)
{
	contains_cookie* contains = (contains_cookie*)cookie;
	if (contains->found)
		return;

	if (contains->start != 0 || contains->end != 0) {
		if (callSite >= contains->start && callSite < contains->end)
			contains->found = true;
		return;
	}

	addr_t baseAddress;
	const char* symbol;
	const char* image;
	bool exactMatch;

	if (lookup_symbol(contains->thread, callSite, &baseAddress, &symbol, &image,
			&exactMatch) != B_OK) {
		return;
	}

	if (symbol != NULL && strcmp(symbol, contains->symbol) == 0)
		contains->found = true;
}


bool
arch_debug_contains_call(Thread *thread, const char *symbol,
	addr_t start, addr_t end)
{
	flush_register_windows();

	addr_t sp;
	addr_t callerSp;
	addr_t callSite;
	current_frame(&sp, &callerSp, &callSite);

	contains_cookie cookie = { thread, start, end, symbol, false };
	walk_stack(thread, sp, callerSp, callSite, 0, 512,
		contains_frame_callback, &cookie);

	return cookie.found;
}


void
arch_debug_save_registers(struct arch_debug_registers* registers)
{
	addr_t sp;
	addr_t callerSp;
	addr_t callSite;

	flush_register_windows();
	current_frame(&sp, &callerSp, &callSite);
	registers->sp = sp;
}


/*!	Runs a function with a fault handler installed.

	The handler fields are filled in so that the fault path can find them, but
	that path does not consult them yet: a data access exception still reaches
	the unhandled-trap handler and stops the machine rather than being caught
	here. Setting them anyway is not pretence -- it means the day the fault path
	learns to honour them, this works without being revisited, and until then
	the value is that the stack walk above validates every address itself rather
	than relying on being rescued.
*/
void
arch_debug_call_with_fault_handler(cpu_ent* cpu, jmp_buf jumpBuffer,
	void (*function)(void*), void* parameter)
{
	addr_t sp;
	addr_t callerSp;
	addr_t callSite;
	current_frame(&sp, &callerSp, &callSite);

	cpu->fault_handler_stack_pointer = sp;
	cpu->fault_handler = 0;

	function(parameter);
}


void
arch_debug_unset_current_thread(void)
{
	asm volatile("mov %g0, %g7");
}


bool
arch_is_debug_variable_defined(const char* variableName)
{
	// TODO: no architecture-specific debug variables yet. Registers would be
	// the obvious set, and they want an iframe to read them from.
	return false;
}


status_t
arch_set_debug_variable(const char* variableName, uint64 value)
{
	return B_ENTRY_NOT_FOUND;
}


status_t
arch_get_debug_variable(const char* variableName, uint64* value)
{
	return B_ENTRY_NOT_FOUND;
}


ssize_t
arch_debug_gdb_get_registers(char* buffer, size_t bufferSize)
{
	// TODO: Implement!
	return B_NOT_SUPPORTED;
}


void*
arch_debug_get_interrupt_pc(bool* _isSyscall)
{
	// TODO: wants the innermost iframe, which needs the interrupt entry to
	// record where it put one.
	if (_isSyscall != NULL)
		*_isSyscall = false;
	return NULL;
}


void
arch_debug_snooze(bigtime_t duration)
{
	spin(duration);
}


// #pragma mark - the backtrace test


#define BACKTRACE_PROBE_DEPTH	24

static addr_t sBacktraceAddresses[64];
static int32 sBacktraceCount;


/*!	Recurses deeper than the register file has windows, then takes a backtrace.

	Twenty-four frames against eight windows, so most of this call chain is on the
	stack rather than in registers by the time the trace is taken. That is the
	point: a walk that forgot to flush would find whatever those save areas held
	before, and a walk that could only see live windows would stop after eight.

	Compiled at -O0, which is not laziness. At -O2 the compiler applied the
	accumulator transformation to `f(depth - 1) + 1` and turned the whole
	recursion into a loop -- one frame instead of twenty-four, and a test that
	reported five frames and proved nothing. `noinline` does not prevent that;
	only turning the optimisation off does.
*/
static addr_t __attribute__((noinline, optimize("O0")))
sparc_backtrace_probe(int32 depth)
{
	if (depth > 0)
		return sparc_backtrace_probe(depth - 1) + 1;

	sBacktraceCount = arch_debug_get_stack_trace(sBacktraceAddresses,
		(int32)(sizeof(sBacktraceAddresses) / sizeof(sBacktraceAddresses[0])),
		0, 0, 0);

	return 0;
}


/*!	Checks that the walk produced the call chain that actually exists.

	Counting frames would not do: a walk that followed garbage produces frames
	too. What makes this checkable without a symbol table is that the probe calls
	itself from exactly one place, so **every recursive frame records the same
	call site**. Twenty-four identical addresses in a row is not something a
	wrong walk produces by accident, and it is precisely the evidence that the
	spilled windows were read correctly -- there are only eight windows, so most
	of those frames came from memory.

	Symbolising them would have been the obvious check and is not available here:
	elf_debug_lookup_symbol_address() asserts that the image mutex is held, which
	is true in KDL and not true of ordinary kernel code. Comparing raw addresses
	needs no such thing.
*/
void
sparc_test_backtrace()
{
	sBacktraceCount = 0;
	sparc_backtrace_probe(BACKTRACE_PROBE_DEPTH);

	// Frame 0 is the call into arch_debug_get_stack_trace() from the deepest
	// probe; the recursive call sites start after it.
	int32 identical = 0;
	addr_t callSite = 0;

	if (sBacktraceCount > 1) {
		callSite = sBacktraceAddresses[1];
		for (int32 i = 1; i < sBacktraceCount; i++) {
			if (sBacktraceAddresses[i] != callSite)
				break;
			identical++;
		}
	}

	// And that address has to be inside the probe rather than merely repeated.
	addr_t probe = (addr_t)&sparc_backtrace_probe;
	addr_t distance = callSite > probe ? callSite - probe : probe - callSite;

	bool ok = identical >= BACKTRACE_PROBE_DEPTH && distance < 0x1000;

	dprintf("arch_debug: backtrace found %" B_PRId32 " frames, %" B_PRId32
		" of them the same call site %#lx, %#lx past the probe -- %s\n",
		sBacktraceCount, identical, callSite, distance,
		ok ? "walked the spilled windows" : "WRONG");

	if (!ok) {
		panic("sparc backtrace: %" B_PRId32 " repeated frames at %#lx (%#lx "
			"from the probe), expected at least %d", identical, callSite,
			distance, BACKTRACE_PROBE_DEPTH);
	}

	// And print one, so the boot log carries an actual backtrace rather than
	// only a claim that the walk works. Symbols are unavailable outside the
	// debugger -- see lookup_symbol() -- so these are bare addresses.
	arch_debug_stack_trace();
}


status_t
arch_debug_init(kernel_args *args)
{
	add_debugger_command("where", &stack_trace_command, "Same as \"sc\"");
	add_debugger_command("bt", &stack_trace_command,
		"Same as \"sc\" (as in gdb)");
	add_debugger_command("sc", &stack_trace_command,
		"Stack crawl for the current thread");

	return B_NO_ERROR;
}
