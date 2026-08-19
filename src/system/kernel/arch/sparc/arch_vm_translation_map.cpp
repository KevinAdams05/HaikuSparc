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


#include <new>

#include <KernelExport.h>
#include <arch_mmu.h>
#include <arch_vm_translation_map.h>
#include <kernel.h>
#include <platform/openfirmware/openfirmware.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>

#include "SPARCVMTranslationMap.h"


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

// The kernel's page table root, physical. Every kernel translation map wraps
// this same table: there is one kernel address space and it outlives everything
// that describes it.
static phys_addr_t sKernelPageTable;

// A buffer plus placement new, not a static object.
//
// The kernel does not run global constructors. It has a .ctors section with
// relocations against it and nothing that walks it, which for a plain struct
// costs nothing -- but this class has virtual methods, so without its
// constructor the vtable pointer stays zero and the first virtual call jumps to
// address zero.
//
// That is not hypothetical. A static object here got as far as
// vm_page_allocate_page() asking for a cleared page, which reaches
// vm_memset_physical(), which loaded the vtable pointer, loaded the method at
// offset 0x40 from it, and called zero -- reported as an illegal instruction at
// pc 0x4. This is why x86's paging code constructs its page mapper the same way.
//
// A buffer rather than the heap because this has to exist before there is one.
static char sPhysicalPageMapperBuffer[sizeof(SPARCVMPhysicalPageMapper)]
	__attribute__((aligned(16)));
static SPARCVMPhysicalPageMapper* sPhysicalPageMapper;


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

	// The kernel's page table root. This has to exist before the TSB is warmed,
	// because warming records the firmware's translations and they belong in the
	// authoritative structure as much as in the cache -- a TSB line that gets
	// evicted has to be recoverable, and the page table is the only thing that
	// can recover it.
	SPARCPageTableAllocator allocator = { args, NULL };
	sKernelPageTable = allocator.Allocate();
	if (sKernelPageTable == 0) {
		panic("arch_vm_translation_map_init: no page for the kernel page table");
		return B_NO_MEMORY;
	}
	TRACE("kernel page table root at %#" B_PRIxPHYSADDR "\n", sKernelPageTable);

	// Build the kernel's TSB, warm it and the page table with the firmware's
	// translations, lock what the trap handlers need, and hand the MMU over.
	status_t status = sparc_mmu_init_tsb(args);
	if (status != B_OK)
		return status;

	sPhysicalPageMapper
		= new(sPhysicalPageMapperBuffer) SPARCVMPhysicalPageMapper;
	*_physicalPageMapper = sPhysicalPageMapper;

	return B_OK;
}


phys_addr_t
sparc_kernel_page_table()
{
	return sKernelPageTable;
}


/*!	Records an early mapping in the kernel's page table.

	Separate from the translation map class because it runs before any address
	space exists, and shares the class's walk rather than repeating it.
*/
status_t
sparc_page_table_early_map(kernel_args* args, addr_t virtualAddress,
	phys_addr_t physicalAddress, uint64 flags)
{
	if (sKernelPageTable == 0)
		return B_NO_INIT;

	SPARCPageTableAllocator allocator = { args, NULL };
	phys_addr_t entry = sparc_page_table_lookup(sKernelPageTable,
		virtualAddress, &allocator);
	if (entry == 0)
		return B_NO_MEMORY;

	sparc_write_physical(entry, TTE_VALID
		| ((uint64)TTE_SIZE_8K << TTE_SIZE_SHIFT)
		| (physicalAddress & TTE_PA_MASK) | flags);

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

	// Both structures, always, and the page table first. It is the record; the
	// TSB is a cache of it. A TSB line can be evicted by a collision at any
	// moment, and when it is, the only thing that can answer the resulting miss
	// is the page table.
	//
	// Early mappings are writable outright rather than mapped read-only to catch
	// the first write. Nothing is tracking modified pages this early, and a
	// protection trap taken before the handler for it exists would be fatal
	// rather than informative.
	const uint64 kEarlyFlags = TTE_WRITABLE | TTE_SOFT_REAL_WRITABLE
		| TTE_SOFT_ACCESSED | TTE_SOFT_MODIFIED | TTE_PRIVILEGED | TTE_GLOBAL
		| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL;

	status_t status = sparc_page_table_early_map(args, va, pa, kEarlyFlags);
	if (status != B_OK) {
		panic("arch_vm_translation_map_early_map: no page table entry for va %#"
			B_PRIxADDR, va);
		return status;
	}

	sparc_tsb_insert(va, pa, kEarlyFlags & ~TTE_GLOBAL);

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
	// A kernel map wraps the table that already exists and is already populated;
	// a user map starts with nothing and grows its levels on demand. Passing zero
	// for the latter is what makes the first Map() allocate a root.
	SPARCVMTranslationMap* map = new(std::nothrow) SPARCVMTranslationMap(kernel,
		kernel ? sKernelPageTable : 0);
	if (map == NULL)
		return B_NO_MEMORY;

	*_map = map;
	return B_OK;
}


/*!	Whether the debugger may touch this address without risking a fault.

	Answered from the page table rather than by guessing at address ranges,
	because the walk uses physical accesses: it cannot fault, cannot block and
	needs no lock, which is exactly what makes it safe to run from KDL with the
	rest of the machine stopped.
*/
bool
arch_vm_translation_map_is_kernel_page_accessible(addr_t virtualAddress,
	uint32 protection)
{
	if (sKernelPageTable == 0)
		return false;

	phys_addr_t entry = sparc_page_table_lookup(sKernelPageTable,
		virtualAddress, NULL);
	if (entry == 0)
		return false;

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0)
		return false;

	// A write needs the mapping to actually permit one. REAL_WRITABLE rather
	// than W, since a writable page may be sitting read-only waiting to have its
	// first write noticed, and the debugger's write would simply set the bit.
	if ((protection & B_KERNEL_WRITE_AREA) != 0
			&& (tte & TTE_SOFT_REAL_WRITABLE) == 0) {
		return false;
	}

	return true;
}

