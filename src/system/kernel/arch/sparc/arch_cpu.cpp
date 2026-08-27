/*
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Distributed under the terms of the MIT License.
 */


#include <KernelExport.h>

#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <commpage.h>
#include <elf.h>


status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	return B_OK;
}


status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
	//detect_cpu(curr_cpu);

	// we only support one anyway...
	return 0;
}


status_t
arch_cpu_init(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	return B_OK;
}


/*!	Makes stores to instruction space visible to instruction fetches.

	Needed because UltraSPARC does not keep its instruction cache coherent with
	stores, and this port writes code at run time in two places: the runtime
	loader and the kernel's own ELF loader both fill in `R_SPARC_JMP_SLOT`, and
	on SPARC a procedure linkage table entry is *instructions* rather than a
	pointer -- binding a symbol means assembling a branch to it.

	`flush` is the instruction for it, and the architecture is specific about the
	shape:

	  - "FLUSH operates on at least the doubleword containing the addressed
	    location", and bit 2 of the address is ignored for that reason, so the
	    loop steps by eight (SPARC V9 manual A.20, printed p.167).
	  - "If a program includes self-modifying code, it must issue a FLUSH
	    instruction for each modified doubleword of instructions" (H.1.6, printed
	    p.308). Each, not one for the range.
	  - `MEMBAR #StoreStore` first: "When a MEMBAR #StoreStore, FLUSH sequence is
	    performed, UltraSPARC-IIi guarantees that earlier code modifications will
	    be visible across the whole system" (UltraSPARC-IIi manual 14.4.4,
	    printed p.196).

	**The address is translated by the D-MMU**, which is the part that surprises:
	the same section notes that FLUSH can therefore take a `data_access_exception`
	or a `data_access_MMU_miss`. A miss is ordinary and the table handles it; an
	exception means the caller passed an address it could not have written to
	either, which is the caller's bug and is better as a fault than as silence.

	QEMU does not need any of this -- it invalidates its translations on any store
	-- so nothing here can be tested in emulation. It is written from the manual
	against the day this port meets a Blade 150.
*/
void
arch_cpu_sync_icache(void *address, size_t len)
{
	if (len == 0)
		return;

	asm volatile("membar #StoreStore" ::: "memory");

	addr_t end = (addr_t)address + len;
	for (addr_t doubleword = (addr_t)address & ~(addr_t)7; doubleword < end;
			doubleword += 8) {
		asm volatile("flush %0" :: "r"(doubleword) : "memory");
	}
}


/*!	Load-load and store-store ordering.

	Both are empty of work in the memory model this port runs in, and both are
	written out anyway.

	UltraSPARC-IIi implements three of SPARC V9's models, and which one is in
	force is PSTATE.MM. Nothing in this port writes that field -- every PSTATE
	write here is a read-modify-write of what the firmware left -- so the machine
	runs in TSO, where stores are ordered against stores and loads against loads
	by the hardware. The manual's own worked example labels the two barriers
	exactly: `MEMBAR #StoreStore` is "needed in PSO, RMO" and
	`MEMBAR #LoadLoad | #LoadStore` is "needed in RMO" (CODE EXAMPLE 8-1, printed
	p.71). In TSO neither is needed.

	So these could correctly be left empty, and they are not, because "correct
	because of a mode nothing establishes" is the shape of bug this port has paid
	for before. A `membar` the hardware has no work for retires immediately; the
	cost of being wrong later is a class of failure that appears as corruption.

	Device registers need neither: an access to a page with the E bit set is
	"strongly ordered with respect to other noncacheable accesses with the E-bit
	set" (8.3.1.2, printed p.71), so a driver's ordering comes from the mapping.

	**A `membar` must never be placed in a delay slot.** Erratum 51 (Appendix K.2,
	printed p.476) describes a deadlock whose easiest trigger is "the delay slot
	of the JMPL is a MEMBAR #Sync, or any instruction that synchronizes on the
	load or store buffers being empty" -- and it is only a performance loss
	"except when pstate.ie==0 ... (for instance, in trap handlers)", where it
	hangs. That does not constrain the C below, which the compiler will not use as
	a delay-slot filler, but it constrains every hand-written handler in this port.
*/
void
arch_cpu_memory_read_barrier(void)
{
	asm volatile("membar #LoadLoad" ::: "memory");
}


void
arch_cpu_memory_write_barrier(void)
{
	asm volatile("membar #StoreStore" ::: "memory");
}


void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
}


void
arch_cpu_global_tlb_invalidate()
{
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{
}


status_t
arch_cpu_shutdown(bool reboot)
{
	return B_ERROR;
}
