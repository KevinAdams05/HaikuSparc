/*
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Copyright 2003-2005, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */

#include <vm/vm.h>
#include <vm/VMAddressSpace.h>
#include <arch/vm.h>


//#define TRACE_ARCH_VM
#ifdef TRACE_ARCH_VM
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


status_t
arch_vm_init(kernel_args *args)
{
	return B_OK;
}


status_t
arch_vm_init_post_area(kernel_args *args)
{
	return B_OK;
}


status_t
arch_vm_init_post_modules(kernel_args *args)
{
	return B_OK;
}


status_t
arch_vm_init_end(kernel_args *args)
{
	TRACE(("arch_vm_init_end(): %lu virtual ranges to keep:\n",
		args->arch_args.num_virtual_ranges_to_keep));

	for (int i = 0; i < (int)args->arch_args.num_virtual_ranges_to_keep; i++) {
		addr_range &range = args->arch_args.virtual_ranges_to_keep[i];

		TRACE(("  start: %p, size: 0x%lx\n", (void*)range.start, range.size));

#if 0
		// skip ranges outside the kernel address space
		if (!IS_KERNEL_ADDRESS(range.start)) {
			TRACE(("    no kernel address, skipping...\n"));
			continue;
		}

		phys_addr_t physicalAddress;
		void *address = (void*)range.start;
		if (vm_get_page_mapping(VMAddressSpace::KernelID(), range.start,
				&physicalAddress) != B_OK)
			panic("arch_vm_init_end(): No page mapping for %p\n", address);
		area_id area = vm_map_physical_memory(VMAddressSpace::KernelID(),
			"boot loader reserved area", &address,
			B_EXACT_ADDRESS, range.size,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			physicalAddress, true);
		if (area < 0) {
			panic("arch_vm_init_end(): Failed to create area for boot loader "
				"reserved area: %p - %p\n", (void*)range.start,
				(void*)(range.start + range.size));
		}
#endif
	}

#if 0
	// Throw away any address space mappings we've inherited from the boot
	// loader and have not yet turned into an area.
	vm_free_unused_boot_loader_range(0, 0xffffffff - B_PAGE_SIZE + 1);
#endif

	return B_OK;
}


void
arch_vm_aspace_swap(struct VMAddressSpace *from, struct VMAddressSpace *to)
{
	// This functions is only invoked when a userland thread is in the process
	// of dying. It switches to the kernel team and does whatever cleanup is
	// necessary (in case it is the team's main thread, it will delete the
	// team).
	// It is however not necessary to change the page directory. Userland team's
	// page directories include all kernel mappings as well. Furthermore our
	// arch specific translation map data objects are ref-counted, so they won't
	// go away as long as they are still used on any CPU.
}


bool
arch_vm_supports_protection(team_id team, uint32 protection)
{
	return true;
}


void
arch_vm_unset_memory_type(VMArea *area)
{
}


/*!	Accepts a memory type for an area, and says what it will really get.

	There is nothing to program. sun4u carries the memory type in each page's
	TTE rather than in a separate register file the way x86 carries it in the
	MTRRs, so the decision is made where the entries are built --
	SPARCVMTranslationMap::Map() reads area->MemoryType() and turns it into the
	cacheability and side-effect bits. All this has to do is agree.

	Which means agreeing about two states, because that is all the hardware has:
	a page is cacheable, or it is uncached with side effects. Write-through and
	write-back both become cacheable, which weakens a write-through request to a
	guarantee about whether a store becomes visible rather than when.
	Write-combining becomes uncached -- combining writes is exactly what a device
	page must not do, and it is a performance hint rather than a semantic one, so
	honouring it as uncached is a slow answer and not a wrong one. Write-protected
	is accepted because protection is expressed by the TTE's W bit and enforced
	there, not by the memory type.

	Refusing them instead is what this used to do, and it made
	map_physical_memory() fail for every caller: vm_map_physical_memory() asks
	for B_UNCACHED_MEMORY when the caller expresses no preference, so the
	"type == 0" case never arrived. The PCI bus manager's sixteen megabytes of
	I/O ports failed to map, pci_read_io_8() added the port number to a null
	base, and the ATA driver read the loader's low identity-mapped memory --
	which answered every probe with the same plausible-looking rubbish rather
	than with an error.
*/
status_t
arch_vm_set_memory_type(VMArea *area, phys_addr_t physicalBase, uint32 type,
	uint32 *effectiveType)
{
	switch (type) {
		case 0:
			break;

		case B_UNCACHED_MEMORY:
		case B_WRITE_PROTECTED_MEMORY:
		case B_WRITE_THROUGH_MEMORY:
		case B_WRITE_BACK_MEMORY:
			break;

		case B_WRITE_COMBINING_MEMORY:
			// Reported as uncached rather than as asked for, so that a caller
			// that wanted to know what it got is not told something the
			// hardware cannot do.
			type = B_UNCACHED_MEMORY;
			break;

		default:
			return B_BAD_VALUE;
	}

	if (effectiveType != NULL)
		*effectiveType = type;

	return B_OK;
}
