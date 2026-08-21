/*
 * Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */

#include <platform/openfirmware/openfirmware.h>
#if defined(__sparc__) && !defined(_BOOT_PLATFORM_OPENFIRMWARE)
#	include <arch_mmu.h>
#endif

#include <stdarg.h>

#ifndef _BOOT_PLATFORM_OPENFIRMWARE
#	include <debug.h>
#	include <interrupts.h>
#endif


// OpenFirmware entry function
static intptr_t (*gCallOpenFirmware)(void *) = 0;
intptr_t gChosen;


#if defined(__sparc__) && !defined(_BOOT_PLATFORM_OPENFIRMWARE)
// What the first client call did to the reserved globals, and whether any call
// has ever changed them. See call_open_firmware() below.
static bool sObservedReservedGlobals = false;
static bool sReservedGlobalsClobbered = false;
static uint64 sReservedGlobalsBefore[2];
static uint64 sReservedGlobalsAfter[2];
#endif


/*!	Calls the firmware, with interrupts off on SPARC.

	SPARC V9 reserves %g6 and %g7 for the operating system, and Haiku's SPARC
	kernel keeps the current thread pointer in %g7 -- every
	thread_get_current_thread() is a read of it. The firmware is not the
	operating system and uses those registers for its own state while a client
	call is running; OpenBIOS puts its Forth engine's working values there.

	It puts them back before returning, so the call looks harmless from the
	outside -- the check below confirms that, and says so through
	openfirmware_report_reserved_globals(). What is not harmless is an interrupt
	arriving in the middle. The handler is ordinary kernel code and reads %g7 to
	find out which thread and which CPU it is on, and during a client call %g7
	belongs to the firmware. The observed failure was a timer interrupt landing
	inside the of_write() behind a dprintf(): timer_interrupt() called
	smp_get_current_cpu(), which read %g7 as 0xffec73a1 -- an address inside
	OpenBIOS's own image -- and took a data alignment trap on thread->cpu. It
	then faulted again trying to report that, because panic() needs the thread
	too, and the cascade ran the trap level to MAXTL.

	So the window has to not exist. Nothing about a client call needs to be
	interruptible: they are short, they are already serialised by being the only
	way to reach the firmware, and the timer interrupt they delay is posted in
	SOFTINT and delivered as soon as interrupts come back.

	The save and restore stays as well, for a different reason: OpenBIOS putting
	the registers back is OpenBIOS's behaviour, and this port is aimed at real
	Sun OBP, which nobody has run it on. Two moves either side of a Forth call
	cost nothing measurable, and the alternative is finding out the hard way.

	Other architectures have no such reserved registers to lose, so this is a
	plain call for them.
*/
static intptr_t
call_open_firmware(void *args)
{
#ifdef __sparc__
#ifndef _BOOT_PLATFORM_OPENFIRMWARE
	// The loader has no interrupts to disable, and no threads to lose.
	cpu_status interruptState = disable_interrupts();
#endif

	uint64 savedG6;
	uint64 savedG7;
	asm volatile("mov %%g6, %0" : "=r"(savedG6));
	asm volatile("mov %%g7, %0" : "=r"(savedG7));

	intptr_t result = gCallOpenFirmware(args);

#ifndef _BOOT_PLATFORM_OPENFIRMWARE
	// Recorded rather than printed: the kernel's dprintf() reaches the serial
	// port through of_write(), which arrives here, so anything printed from
	// inside this function is dropped by dprintf()'s re-entrancy guard.
	uint64 firmwareG6;
	uint64 firmwareG7;
	asm volatile("mov %%g6, %0" : "=r"(firmwareG6));
	asm volatile("mov %%g7, %0" : "=r"(firmwareG7));

	if (!sObservedReservedGlobals) {
		sObservedReservedGlobals = true;
		sReservedGlobalsBefore[0] = savedG6;
		sReservedGlobalsBefore[1] = savedG7;
		sReservedGlobalsAfter[0] = firmwareG6;
		sReservedGlobalsAfter[1] = firmwareG7;
	}
	if (firmwareG6 != savedG6 || firmwareG7 != savedG7)
		sReservedGlobalsClobbered = true;
#endif

	asm volatile("mov %0, %%g6" : : "r"(savedG6));
	asm volatile("mov %0, %%g7" : : "r"(savedG7));

#ifndef _BOOT_PLATFORM_OPENFIRMWARE
	// And the trap banks, which the two moves above cannot reach: they run at
	// trap level zero on the normal bank, while the firmware's writes land in
	// whichever bank its own PSTATE selects. See sparc_restore_trap_globals().
	sparc_restore_trap_globals();
#endif

#ifndef _BOOT_PLATFORM_OPENFIRMWARE
	restore_interrupts(interruptState);
#endif

	return result;
#else
	return gCallOpenFirmware(args);
#endif
}


/*!	Says what the firmware does to %g6 and %g7, from somewhere that can print.

	Call once the kernel's debug output works and a few client calls have been
	made. Costs nothing and answers a question that is otherwise unanswerable
	from a log: whether the save and restore in call_open_firmware() is doing
	anything.
*/
void
openfirmware_report_reserved_globals()
{
#if defined(__sparc__) && !defined(_BOOT_PLATFORM_OPENFIRMWARE)
	if (!sObservedReservedGlobals) {
		dprintf("openfirmware: no client call has been made yet\n");
		return;
	}

	if (sReservedGlobalsClobbered) {
		dprintf("openfirmware: client calls return with the reserved globals "
			"changed; the first left %%g6 %#lx -> %#lx and %%g7 %#lx -> %#lx, "
			"and both are restored after every call\n",
			sReservedGlobalsBefore[0], sReservedGlobalsAfter[0],
			sReservedGlobalsBefore[1], sReservedGlobalsAfter[1]);
	} else {
		// Which says nothing about what they hold *during* a call -- see
		// call_open_firmware(). This reports the net effect only.
		dprintf("openfirmware: client calls return %%g6 and %%g7 unchanged\n");
	}
#endif
}


status_t
of_init(intptr_t (*openFirmwareEntry)(void *))
{
	gCallOpenFirmware = openFirmwareEntry;

	gChosen = of_finddevice("/chosen");
	if (gChosen == OF_FAILED)
		return B_ERROR;

	return B_OK;
}


intptr_t
of_call_client_function(const char *method, intptr_t numArgs,
	intptr_t numReturns, ...)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		void		*args[10];
	} args = {method, numArgs, numReturns};
	va_list list;
	int i;

	// iterate over all arguments and copy them into the
	// structure passed over to the OpenFirmware

	va_start(list, numReturns);
	for (i = 0; i < numArgs; i++) {
		// copy args
		args.args[i] = (void *)va_arg(list, void *);
	}
	for (i = numArgs; i < numArgs + numReturns; i++) {
		// clear return values
		args.args[i] = NULL;
	}

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	if (numReturns > 0) {
		// copy return values over to the provided location

		for (i = numArgs; i < numArgs + numReturns; i++) {
			void **store = va_arg(list, void **);
			if (store)
				*store = args.args[i];
		}
	}
	va_end(list);

	return 0;
}


intptr_t
of_interpret(const char *command, intptr_t numArgs, intptr_t numReturns, ...)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
			// "IN:	[string] cmd, stack_arg1, ..., stack_argP
			// OUT:	catch-result, stack_result1, ..., stack_resultQ
			// [...]
			// An implementation shall allow at least six stack_arg and six
			// stack_result items."
		const char	*command;
		void		*args[13];
	} args = {"interpret", numArgs + 1, numReturns + 1, command};
	va_list list;
	int i;

	// iterate over all arguments and copy them into the
	// structure passed over to the OpenFirmware

	va_start(list, numReturns);
	for (i = 0; i < numArgs; i++) {
		// copy args
		args.args[i] = (void *)va_arg(list, void *);
	}
	for (i = numArgs; i < numArgs + numReturns + 1; i++) {
		// clear return values
		args.args[i] = NULL;
	}

	// args.args[numArgs] is the "catch-result" return value
	if (call_open_firmware(&args) == OF_FAILED || args.args[numArgs])
		return OF_FAILED;

	if (numReturns > 0) {
		// copy return values over to the provided location

		for (i = numArgs + 1; i < numArgs + 1 + numReturns; i++) {
			void **store = va_arg(list, void **);
			if (store)
				*store = args.args[i];
		}
	}
	va_end(list);

	return 0;
}


intptr_t
of_call_method(uint32_t handle, const char *method, intptr_t numArgs,
	intptr_t numReturns, ...)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
			// "IN:	[string] method, ihandle, stack_arg1, ..., stack_argP
			// OUT:	catch-result, stack_result1, ..., stack_resultQ
			// [...]
			// An implementation shall allow at least six stack_arg and six
			// stack_result items."
		const char	*method;
		intptr_t	handle;
		void		*args[13];
	} args = {"call-method", numArgs + 2, numReturns + 1, method, handle};
	va_list list;
	int i;

	// iterate over all arguments and copy them into the
	// structure passed over to the OpenFirmware

	va_start(list, numReturns);
	for (i = 0; i < numArgs; i++) {
		// copy args
		args.args[i] = (void *)va_arg(list, void *);
	}
	for (i = numArgs; i < numArgs + numReturns + 1; i++) {
		// clear return values
		args.args[i] = NULL;
	}

	// args.args[numArgs] is the "catch-result" return value
	if (call_open_firmware(&args) == OF_FAILED || args.args[numArgs])
		return OF_FAILED;

	if (numReturns > 0) {
		// copy return values over to the provided location

		for (i = numArgs + 1; i < numArgs + 1 + numReturns; i++) {
			void **store = va_arg(list, void **);
			if (store)
				*store = args.args[i];
		}
	}
	va_end(list);

	return 0;
}


intptr_t
of_finddevice(const char *device)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		const char	*device;
		intptr_t	handle;
	} args = {"finddevice", 1, 1, device, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.handle;
}


/** Returns the first child of the given node
 */

intptr_t
of_child(intptr_t node)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	node;
		intptr_t	child;
	} args = {"child", 1, 1, node, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.child;
}


/** Returns the next sibling of the given node
 */

intptr_t
of_peer(intptr_t node)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	node;
		intptr_t	next_sibling;
	} args = {"peer", 1, 1, node, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.next_sibling;
}


/** Returns the parent of the given node
 */

intptr_t
of_parent(intptr_t node)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	node;
		intptr_t	parent;
	} args = {"parent", 1, 1, node, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.parent;
}


intptr_t
of_instance_to_path(uint32_t instance, char *pathBuffer, intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	instance;
		char		*path_buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"instance-to-path", 3, 1, instance, pathBuffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_instance_to_package(uint32_t instance)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	instance;
		intptr_t	package;
	} args = {"instance-to-package", 1, 1, instance, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.package;
}


intptr_t
of_getprop(intptr_t package, const char *property, void *buffer, intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	package;
		const char	*property;
		void		*buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"getprop", 4, 1, package, property, buffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_setprop(intptr_t package, const char *property, const void *buffer,
	intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	package;
		const char	*property;
		const void	*buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"setprop", 4, 1, package, property, buffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_getproplen(intptr_t package, const char *property)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	package;
		const char	*property;
		intptr_t	size;
	} args = {"getproplen", 2, 1, package, property, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_nextprop(intptr_t package, const char *previousProperty, char *nextProperty)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	package;
		const char	*previous_property;
		char		*next_property;
		intptr_t	flag;
	} args = {"nextprop", 3, 1, package, previousProperty, nextProperty, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.flag;
}


intptr_t
of_package_to_path(intptr_t package, char *pathBuffer, intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	package;
		char		*path_buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"package-to-path", 3, 1, package, pathBuffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


//	I/O functions


intptr_t
of_open(const char *nodeName)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		const char	*node_name;
		intptr_t	handle;
	} args = {"open", 1, 1, nodeName, 0};

	if (call_open_firmware(&args) == OF_FAILED || args.handle == 0)
		return OF_FAILED;

	return args.handle;
}


void
of_close(intptr_t handle)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	handle;
	} args = {"close", 1, 0, handle};

	call_open_firmware(&args);
}


intptr_t
of_read(intptr_t handle, void *buffer, intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	handle;
		void		*buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"read", 3, 1, handle, buffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_write(intptr_t handle, const void *buffer, intptr_t bufferSize)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	handle;
		const void	*buffer;
		intptr_t	buffer_size;
		intptr_t	size;
	} args = {"write", 3, 1, handle, buffer, bufferSize, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.size;
}


intptr_t
of_seek(intptr_t handle, off_t pos)
{
	intptr_t pos_hi = 0;
	if (sizeof(off_t) > sizeof(intptr_t))
		pos_hi = pos >> ((sizeof(off_t) - sizeof(intptr_t)) * CHAR_BIT);

	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	handle;
		intptr_t	pos_hi;
		intptr_t	pos;
		intptr_t	status;
	} args = {"seek", 3, 1, handle, pos_hi, pos, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.status;
}


intptr_t
of_blocks(intptr_t handle)
{
	struct {
		const char      *name;
		intptr_t        num_args;
		intptr_t        num_returns;
		intptr_t        handle;
		intptr_t        result;
		intptr_t        blocks;
	} args = {"#blocks", 2, 1, handle, 0, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;
	return args.blocks;
}


intptr_t
of_block_size(intptr_t handle)
{
	struct {
		const char      *name;
		intptr_t        num_args;
		intptr_t        num_returns;
		intptr_t        handle;
		intptr_t        result;
		intptr_t        size;
	} args = {"block-size", 2, 1, handle, 0, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;
	return args.size;
}


// memory functions


intptr_t
of_release(void *virtualAddress, intptr_t size)
{
	struct {
		const char *name;
		intptr_t	num_args;
		intptr_t	num_returns;
		void		*virtualAddress;
		intptr_t	size;
	} args = {"release", 2, 0, virtualAddress, size};

	return call_open_firmware(&args);
}


void *
of_claim(void *virtualAddress, intptr_t size, intptr_t align)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		void		*virtualAddress;
		intptr_t	size;
		intptr_t	align;
		void		*address;
	} args = {"claim", 3, 1, virtualAddress, size, align};

	if (call_open_firmware(&args) == OF_FAILED)
		return NULL;

	return args.address;
}


// misc functions


/** tests if the given service is missing
 */

intptr_t
of_test(const char *service)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		const char	*service;
		intptr_t	missing;
	} args = {"test", 1, 1, service, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.missing;
}


/** Returns the millisecond counter
 */

intptr_t
of_milliseconds(void)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
		intptr_t	milliseconds;
	} args = {"milliseconds", 0, 1, 0};

	if (call_open_firmware(&args) == OF_FAILED)
		return OF_FAILED;

	return args.milliseconds;
}


void
of_exit(void)
{
	struct {
		const char	*name;
		intptr_t	num_args;
		intptr_t	num_returns;
	} args = {"exit", 0, 0};

	call_open_firmware(&args);
}

