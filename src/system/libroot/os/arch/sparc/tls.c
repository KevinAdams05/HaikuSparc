/*
 * Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*!	Thread local storage, as userspace reaches it.

	Two separate things share the name, and this file is where they meet.

	The first is Haiku's own tls_allocate()/tls_get()/tls_set() -- a small array
	of pointers, one per thread, indexed by a slot number the caller is handed at
	runtime. errno lives in it, and so does the pointer to the thread's own
	structure. Nothing about it is architecture specific except one question:
	given a thread, where is its array? Every architecture answers that with a
	register the kernel loads before the thread starts.

	The second is the ELF thread local storage the compiler generates for
	`__thread` variables, which is a different mechanism with different rules --
	and on the general dynamic model the compiler emits a call to
	__tls_get_addr() for every access, because the offset is not known until the
	shared object holding the variable has been loaded. That one is the runtime
	loader's problem, and this delegates to it.

	The register is %g7.

	Which is the platform's answer rather than this port's choice: the SPARC ABI
	names %g7 the thread pointer, so this is where a compiler emitting an
	initial-exec or local-exec access looks, and no other value would work. The
	kernel gave up needing it -- the trap entry reads the current thread out of
	the trap data block instead -- specifically so that userspace could have it.
	See sparc_enter_userspace().

	This file replaced a stub whose comment said it was "just broken now (okay
	for single threaded apps, though)": a static array shared by every thread in
	the team, which is exactly what thread local storage exists not to be. It was
	the last architecture still carrying that stub.
*/

#include <runtime_loader/runtime_loader.h>

#include <support/TLS.h>
#include <tls.h>


/*!	The argument the compiler passes to __tls_get_addr().

	Two words: which loaded object the variable belongs to, and where in that
	object's block it sits. Declared here rather than included from anywhere
	because it is part of the ELF TLS ABI, not of Haiku -- every architecture in
	this tree declares its own copy for the same reason.

	On SPARC the relocation pair that fills it in is R_SPARC_TLS_DTPMOD64 and
	R_SPARC_TLS_DTPOFF64, and the linker places the two words adjacently so that
	one pointer reaches both.
*/
struct tls_index {
	unsigned long	ti_module;
	unsigned long	ti_offset;
};


static int32 gNextSlot = TLS_FIRST_FREE_SLOT;


void* __tls_get_addr(struct tls_index* ti);


static inline void**
get_tls(void)
{
	void** tls;
	__asm__ __volatile__("mov %%g7, %0" : "=r"(tls));
	return tls;
}


int32
tls_allocate(void)
{
	int32 next = atomic_add(&gNextSlot, 1);
	if (next >= TLS_MAX_KEYS)
		return B_NO_MEMORY;

	return next;
}


void *
tls_get(int32 index)
{
	return get_tls()[index];
}


void **
tls_address(int32 index)
{
	return get_tls() + index;
}


void
tls_set(int32 index, void *value)
{
	get_tls()[index] = value;
}


/*!	Where a general-dynamic __thread variable lives, for this thread.

	Called by compiler-generated code, once per access, with the pair of words
	the linker filled in. The answer depends on which objects this thread has
	blocks for, which is bookkeeping the runtime loader keeps -- so this is a
	forwarding function and deliberately nothing more.
*/
void*
__tls_get_addr(struct tls_index* ti)
{
	return __gRuntimeLoader->get_tls_address(ti->ti_module, ti->ti_offset);
}
