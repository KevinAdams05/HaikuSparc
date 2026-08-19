/*
 * Copyright 2007-2010, François Revol, revol@free.fr.
 * Copyright 2008-2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2007, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <KernelExport.h>
#include <arch_mmu.h>
#include <kernel.h>
#include <platform/openfirmware/openfirmware.h>
#include <vm/vm.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>


#define TRACE_VM_TMAP
#ifdef TRACE_VM_TMAP
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// Mode passed to the Open Firmware MMU node's "map" method. -1 asks the
// firmware for its own default: cacheable, privileged and writable. See the
// longer explanation in the boot loader's arch/sparc/mmu.cpp -- spelling out
// TTE bits by hand would also have to get CP, CV and P right, for mappings the
// kernel is going to replace with its own once it owns the MMU.
#define PAGE_DEFAULT_MODE	((int64)-1)

// Open Firmware's MMU instance, from /chosen. Zero until first needed.
static int sMmuInstance;


status_t
arch_vm_translation_map_init(kernel_args *args,
	VMPhysicalPageMapper** _physicalPageMapper)
{
	TRACE("vm_translation_map_init: entry\n");

#ifdef TRACE_VM_TMAP
	TRACE("physical memory ranges:\n");
	for (uint32 i = 0; i < args->num_physical_memory_ranges; i++) {
		phys_addr_t start = args->physical_memory_range[i].start;
		phys_addr_t end = start + args->physical_memory_range[i].size;
		TRACE("  %#10" B_PRIxPHYSADDR " - %#10" B_PRIxPHYSADDR "\n", start,
			end);
	}

	TRACE("allocated physical ranges:\n");
	for (uint32 i = 0; i < args->num_physical_allocated_ranges; i++) {
		phys_addr_t start = args->physical_allocated_range[i].start;
		phys_addr_t end = start + args->physical_allocated_range[i].size;
		TRACE("  %#10" B_PRIxPHYSADDR " - %#10" B_PRIxPHYSADDR "\n", start,
			end);
	}

	TRACE("allocated virtual ranges:\n");
	for (uint32 i = 0; i < args->num_virtual_allocated_ranges; i++) {
		addr_t start = args->virtual_allocated_range[i].start;
		addr_t end = start + args->virtual_allocated_range[i].size;
		TRACE("  %#10" B_PRIxADDR " - %#10" B_PRIxADDR "\n", start, end);
	}
#endif

	// What Open Firmware currently has mapped, with physical addresses and
	// modes. This is the set that has to be carried into the kernel's own TSB
	// before %tba is repointed -- see sparc-port/PHASE2_MMU_DESIGN.md.
	sparc_dump_openfirmware_translations();

	// Build the kernel's TSB and warm it with those translations. Nothing
	// points at it yet: the hardware still uses the firmware's TSB and the
	// firmware's trap handlers. This only gets the structure in place so it
	// can be inspected before the cutover depends on it.
	sparc_mmu_init_tsb(args);

	return B_OK;
}


status_t
arch_vm_translation_map_init_post_sem(kernel_args *args)
{
	return B_OK;
}


status_t
arch_vm_translation_map_init_post_area(kernel_args *args)
{
	TRACE("vm_translation_map_init_post_area: entry\n");
	return B_OK;
}


/*!	Establishes a mapping before the kernel owns the MMU.

	These run while Open Firmware's trap table is still installed -- %tba is
	untouched at this point -- so the firmware, not the kernel, is servicing
	every TLB miss the machine takes. Writing a TTE straight into the TLB would
	therefore work only until that entry was evicted, at which point the miss
	would be handed to a firmware handler that has never heard of it.

	So ask the firmware to make the mapping, exactly as the boot loader does.
	It records the translation in its own tables, its miss handler services it,
	and the mapping survives eviction. That is also what makes these mappings
	"early": they belong to the period before the kernel installs a trap table
	and a TSB of its own, after which it must import what the firmware holds --
	the loader already reads the "translations" property for that purpose.

	Note the size argument is a single page. The caller maps one page per call
	and Haiku's page size on sparc is 8 KB, matching the smallest TTE size.

	\note This does not scale, and cannot be the permanent answer. OpenBIOS
	rebuilds its "translations" property on every call, so the cost is
	quadratic in the number of mappings, and its heap is exhausted after
	roughly 1300 of them -- around half of what a boot needs. Coalescing
	adjacent pages into one larger call is not safe either: vm_allocate_early()
	maps a whole allocation before returning it, so a deferred flush would land
	after the memory had already been used, and mapping ahead would create
	translations for virtual addresses the VM has not handed out yet.

	The durable fix is for the kernel to own a TSB and a trap table, at which
	point this becomes a few stores rather than a firmware call. That is
	Phase 2 proper; this gets the kernel far enough to be worth debugging in
	the meantime, and real Open Firmware may well have the headroom that
	OpenBIOS lacks.
*/
status_t
arch_vm_translation_map_early_map(kernel_args *args, addr_t va, phys_addr_t pa,
	uint8 attributes)
{
	TRACE("early_tmap: entry pa 0x%lx va 0x%lx\n", pa, va);

	if (sMmuInstance == 0) {
		if (of_getprop(gChosen, "mmu", &sMmuInstance, sizeof(int))
				== OF_FAILED) {
			panic("arch_vm_translation_map_early_map: no Open Firmware mmu");
			return B_ERROR;
		}
	}

	// The kernel's TSB always gets the mapping. Before the cutover that is
	// bookkeeping for later; afterwards it is the mapping.
	sparc_tsb_insert(va, pa, TTE_WRITABLE | TTE_PRIVILEGED
		| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL);

	// Once the kernel services its own traps, the firmware has no further part
	// in this. Asking it to map anyway is not merely redundant: every call
	// extends its "translations" property, and OpenBIOS's heap does not survive
	// the thousands of pages vm_page_init() maps -- it fails with "out of malloc
	// memory" partway through, which is where this port stopped before the
	// cutover made the call unnecessary.
	if (sparc_mmu_is_installed())
		return B_OK;

	// "map" takes ( mode size virt phys.hi phys.lo -- ) and returns nothing, so
	// the value here reports only whether the client-interface call itself got
	// through -- a firmware that declines the mapping does so silently. Under
	// OpenBIOS that shows up as "Unable to allocate memory for translations
	// property!" on the console and nothing else; see the note above about its
	// heap limit.
	if (of_call_method(sMmuInstance, "map", 5, 0, (uint64)PAGE_DEFAULT_MODE,
			(uint64)B_PAGE_SIZE, (uint64)va, 0, (uint64)pa) != 0) {
		panic("arch_vm_translation_map_early_map: map call failed for va %#"
			B_PRIxADDR " pa %#" B_PRIxPHYSADDR, va, pa);
		return B_ERROR;
	}

	return B_OK;
}


status_t
arch_vm_translation_map_create_map(bool kernel, VMTranslationMap** _map)
{
	return B_OK;
}


bool
arch_vm_translation_map_is_kernel_page_accessible(addr_t virtualAddress,
	uint32 protection)
{
	return false;
}

