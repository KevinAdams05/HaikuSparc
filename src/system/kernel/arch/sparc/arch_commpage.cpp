/*
 * Copyright 2007, Travis Geiselbrecht. All rights reserved.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

#include <commpage.h>

#include <string.h>

#include <KernelExport.h>

#include <cpu.h>
#include <elf.h>
#include <ksignal.h>
#include <smp.h>

#include "syscall_numbers.h"


/*!\tThe code a signal handler returns through, running in userspace.

	Written in C and copied into the commpage rather than hand-assembled, which is
	what every other architecture does here and is worth saying why: this runs
	unprivileged, at an address the kernel chose, with no relocations applied to
	it. So it may not reference a kernel symbol, may not call anything, and may not
	touch memory it was not handed -- and a C function that only calls through a
	pointer it was given and then traps satisfies all three, whereas assembly that
	looks like it satisfies them has to be read carefully to be sure.

	The system call at the end is the same convention libroot's stubs use: the
	index in %g1, the argument in %o0, `ta` with the number
	headers/private/kernel/arch/sparc/arch_int.h calls SPARC_SYSCALL_TRAP. It does
	not return, because _kern_restore_signal_frame() puts the interrupted context
	back and returns to that instead.
*/
extern "C" void __attribute__((noreturn))
sparc_user_signal_handler(signal_frame_data* data)
{
	if (data->siginfo_handler) {
		auto handler = (void (*)(int, siginfo_t*, void*, void*))data->handler;
		handler(data->info.si_signo, &data->info, &data->context,
			data->user_data);
	} else {
		auto handler = (void (*)(int, void*, vregs*))data->handler;
		handler(data->info.si_signo, data->user_data,
			&data->context.uc_mcontext);
	}

	// _kern_restore_signal_frame(data)
	asm volatile(
		"mov	%[data], %%o0\n\t"
		"mov	%[index], %%g1\n\t"
		"ta	0x40"
		:
		: [data] "r"(data),
		  [index] "r"((unsigned long)SYSCALL_RESTORE_SIGNAL_FRAME)
		: "g1", "o0", "memory");

	__builtin_unreachable();
}


/*!\tCopies a kernel function into the commpage for userspace to call.

	The same shape every architecture uses. The expected address is passed in as
	well as looked up so that a mismatch between the symbol table and the linker's
	idea of where the function went is caught here rather than by userspace
	jumping into the middle of something.
*/
static void
register_commpage_function(const char* functionName, int32 commpageIndex,
	const char* commpageSymbolName, addr_t expectedAddress)
{
	elf_symbol_info symbolInfo;
	if (elf_lookup_kernel_symbol(functionName, &symbolInfo) != B_OK) {
		panic("register_commpage_function(): cannot find \"%s\"", functionName);
		return;
	}

	ASSERT(expectedAddress == symbolInfo.address);

	addr_t position = fill_commpage_entry(commpageIndex,
		(void*)symbolInfo.address, symbolInfo.size);

	image_id image = get_commpage_image();
	elf_add_memory_image_symbol(image, commpageSymbolName, position,
		symbolInfo.size, B_SYMBOL_TYPE_TEXT);
}


status_t
arch_commpage_init(void)
{
	return B_OK;
}


status_t
arch_commpage_init_post_cpus(void)
{
	register_commpage_function("sparc_user_signal_handler",
		COMMPAGE_ENTRY_SPARC_SIGNAL_HANDLER, "commpage_signal_handler",
		(addr_t)&sparc_user_signal_handler);

	return B_OK;
}
