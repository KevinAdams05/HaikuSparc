/*
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <arch_mmu.h>

#include <stddef.h>

#include <arch_cpu.h>
#include <arch_thread_types.h>
#include <arch_vm_translation_map.h>
#include <debug.h>
#include <platform/openfirmware/openfirmware.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <vm/VMAddressSpace.h>


// Address space identifiers for the MMUs
// Ultrasparc User Manual, Table 6-10
enum {
	instruction_control_asi = 0x50,
	data_control_asi = 0x58,
	instruction_8k_tsb_asi = 0x51,
	data_8k_tsb_asi = 0x59,
	instruction_64k_tsb_asi = 0x52,
	data_64k_tsb_asi = 0x5A,
	data_direct_tsb_asi = 0x5B,
	instruction_tlb_in_asi = 0x54,
	data_tlb_in_asi = 0x5C,
	instruction_tlb_access_asi = 0x55,
	data_tlb_access_asi = 0x5D,
	instruction_tlb_read_asi = 0x56,
	data_tlb_read_asi = 0x5E,
	instruction_tlb_demap_asi = 0x57,
	data_tlb_demap_asi = 0x5F,
};


// MMU register addresses
// Ultrasparc User Manual, Table 6-10
enum {
	tsb_tag_target = 0x00,            // I/D, RO
	primary_context = 0x08,           //   D, RW
	secondary_context = 0x10,         //   D, RW
	synchronous_fault_status = 0x18,  // I/D, RW
	synchronous_fault_address = 0x20, //   D, RO
	tsb = 0x28,                       // I/D, RW
	tlb_tag_access = 0x30,            // I/D, RW
	virtual_watchpoint = 0x38,        //   D, RW
	physical_watchpoint = 0x40        //   D, RW
};


extern void sparc_get_instruction_tsb(TsbEntry **_pageTable, size_t *_size)
{
	uint64_t tsbEntry;
	asm("ldxa [%[mmuRegister]] 0x50, %[destination]"
		: [destination] "=r"(tsbEntry)
		: [mmuRegister] "r"(tsb));

	// TSB_Size is a four-bit field, not two: it ranges 0..7, giving 512 to
	// 65536 entries. Masking with 3 silently reported the wrong size for any
	// TSB larger than 4096 entries. See FIGURE 15-9, printed p.227.
	*_pageTable = (TsbEntry*)(tsbEntry & TSB_BASE_MASK);
	*_size = TSB_ENTRIES(tsbEntry & TSB_SIZE_MASK) * sizeof(TsbEntry);
	if ((tsbEntry & TSB_SPLIT) != 0) {
		// When split, TSB_Size gives the size of *each* of the two abutting
		// TSBs, so the region spans twice that.
		*_size *= 2;
	}
}


extern void sparc_get_data_tsb(TsbEntry **_pageTable, size_t *_size)
{
	uint64_t tsbEntry;
	asm("ldxa [%[mmuRegister]] 0x58, %[destination]"
		: [destination] "=r"(tsbEntry)
		: [mmuRegister] "r"(tsb));

	// TSB_Size is a four-bit field, not two: it ranges 0..7, giving 512 to
	// 65536 entries. Masking with 3 silently reported the wrong size for any
	// TSB larger than 4096 entries. See FIGURE 15-9, printed p.227.
	*_pageTable = (TsbEntry*)(tsbEntry & TSB_BASE_MASK);
	*_size = TSB_ENTRIES(tsbEntry & TSB_SIZE_MASK) * sizeof(TsbEntry);
	if ((tsbEntry & TSB_SPLIT) != 0) {
		// When split, TSB_Size gives the size of *each* of the two abutting
		// TSBs, so the region spans twice that.
		*_size *= 2;
	}
}




#ifndef _BOOT_MODE

/*	Everything below is kernel-only.
 *
 *	The Open Firmware boot loader compiles this file too, for the TSB register
 *	readers above -- see the SEARCH rule in
 *	src/system/boot/platform/openfirmware/arch/sparc/Jamfile. It has no use for
 *	the rest, and it is built with -Wstack-usage=1023 because it runs on the
 *	stack the firmware provides, so the 1.5 KB translation buffers these
 *	functions need would fail the build outright.
 */

/*!	Dumps the translations Open Firmware currently has installed.

	These are the mappings the kernel inherits and, crucially, the ones that
	must be carried into its own TSB before %tba is repointed: Open Firmware's
	trap handlers are servicing every TLB miss the kernel takes right now, and
	they stop being reachable the moment the kernel takes over. Knowing exactly
	what is in that set -- with real physical addresses and modes rather than
	the virtual ranges kernel_args carries -- is the input to that cutover.

	Read from the firmware rather than from kernel_args because
	arch_kernel_args only records virtual_ranges_to_keep, which has no physical
	address and no mode.
*/
void
sparc_dump_openfirmware_translations()
{
	int mmuInstance;
	if (of_getprop(gChosen, "mmu", &mmuInstance, sizeof(int)) == OF_FAILED) {
		dprintf("sparc_mmu: no Open Firmware mmu instance\n");
		return;
	}

	intptr_t mmu = of_instance_to_package(mmuInstance);
	if (mmu == OF_FAILED) {
		dprintf("sparc_mmu: cannot resolve the mmu package\n");
		return;
	}

	// Same layout the boot loader parses: virtual address, length, and the
	// TTE data half.
	struct translation {
		void*		virtual_address;
		intptr_t	length;
		intptr_t	data;
	} translations[64];

	int length = of_getprop(mmu, "translations", &translations,
		sizeof(translations));
	if (length == OF_FAILED) {
		dprintf("sparc_mmu: no \"translations\" property\n");
		return;
	}

	length /= sizeof(struct translation);
	dprintf("sparc_mmu: %d Open Firmware translations to preserve:\n", length);

	for (int i = 0; i < length; i++) {
		struct translation* map = &translations[i];
		uint64 data = (uint64)map->data;

		dprintf("  va %#18lx len %#10lx -> pa %#12llx  %c%c%c%c%c size %d\n",
			(addr_t)map->virtual_address, (long)map->length,
			(unsigned long long)(data & TTE_PA_MASK),
			(data & TTE_VALID) != 0 ? 'v' : '-',
			(data & TTE_WRITABLE) != 0 ? 'w' : '-',
			(data & TTE_PRIVILEGED) != 0 ? 'p' : '-',
			(data & TTE_LOCKED) != 0 ? 'l' : '-',
			(data & TTE_GLOBAL) != 0 ? 'g' : '-',
			(int)((data >> TTE_SIZE_SHIFT) & TTE_SIZE_MASK));
	}
}


// The kernel's own TSB. Two abutting halves: 8 KB TTEs first, then 64 KB, as
// the split configuration requires. Only the 8 KB half is populated for now --
// nothing maps with larger pages yet.
static TsbEntry* sKernelTsb;
static phys_addr_t sKernelTsbPhysical;
static bool sMmuInstalled;

// Ranges whose TLB entries are locked and whose mappings never change: the trap
// handlers, the trap table, the trap data block and the TSB.
//
// They have to be remembered, because "locked" does not mean what it sounds
// like. The Lock bit exempts an entry from the *replacement* algorithm; it does
// nothing about an explicit demap, which removes whatever matches the address.
// So an ordinary VM operation on a kernel address -- and the trap table lives
// inside the kernel image, so protecting or remapping kernel text covers it --
// silently unlocks the machine's ability to service a TLB miss, and the next
// instruction fetch of the miss handler misses, nests, and ends in a watchdog
// reset at MAXTL.
struct locked_range {
	addr_t	base;
	size_t	size;
};

static locked_range sLockedRanges[8];
static uint32 sLockedRangeCount;

// Defined further down, next to the TLB-reading code they are built on, but
// needed by the TSB allocation above them.
static status_t sparc_allocate_aligned_physical(kernel_args *args, size_t size,
	size_t alignment, phys_addr_t *_physicalBase);
static int sparc_tlb_lock_range(addr_t virtualAddress,
	phys_addr_t physicalAddress, size_t size, uint64 pageSize,
	bool instruction, uint64 flags);
static void sparc_add_locked_range(addr_t base, size_t size);
static uint32 sKernelTsbCollisions;


/*!	Inserts a translation into the kernel TSB.

	The index is the same one the hardware computes for the 8 KB pointer:
	VA<21+N:13>, which is the low N+9 bits of the page number. Keeping software
	and hardware agreed on this is the whole point -- the miss handler will
	take the pointer straight from ASI_DMMU_TSB_8KB_PTR and expect to find this
	entry there.

	The TSB is a cache, not an authoritative page table. A direct-mapped
	structure means two virtual addresses far enough apart collide, and the
	later writer simply wins; recovering from that is the TSB-miss path's job,
	not this function's.
*/
void
sparc_tsb_insert(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint64 flags)
{
	if (sKernelTsb == NULL)
		return;

	uint32 index = (virtualAddress >> 13) & (KERNEL_TSB_ENTRIES - 1);
	TsbEntry* entry = &sKernelTsb[index];

	if (entry->IsValid())
		sKernelTsbCollisions++;

	// The tag is exactly what the hardware's tag target will be:
	// (context << 48) | VA<63:22>, and the context is zero throughout the
	// kernel. That lets the miss handler compare with a single xor.
	//
	// The Global bit is deliberately not set here even though the data half
	// carries it. Its purpose in the tag is to tell a handler that supports
	// several contexts to ignore the context field, and setting it now would
	// only force the handler to mask it back off. When user contexts arrive --
	// see the plan's section 4.3 -- the comparison will need to become
	// G-aware, and that is the point to set it.
	entry->fTag = (virtualAddress >> 22);
	entry->fData = TTE_VALID | ((uint64)TTE_SIZE_8K << TTE_SIZE_SHIFT)
		| (physicalAddress & TTE_PA_MASK) | TTE_GLOBAL | flags;
}


static void sparc_verify_tlb_load(struct kernel_args *args);
static void sparc_verify_tsb_lookup();
static void sparc_verify_trap_handlers(struct kernel_args *args);

// See sparc_restore_trap_globals().
static bool sObservedMmuGlobal = false;
static uint64 sMmuGlobalAfterCall = 0;
static void sparc_verify_mmu_defines();

extern "C" void sparc_mmu_defines(uint64 *out);


/*!	Allocates the kernel TSB and warms it with Open Firmware's translations.

	This does not point the hardware at it. The TSB register still refers to
	the firmware's own TSB, and the firmware's trap handlers are still
	servicing every miss -- repointing %tba and the TSB register together is
	the cutover, and it comes later. Building the structure first means it can
	be inspected before anything depends on it.
*/
status_t
sparc_mmu_init_tsb(struct kernel_args *args)
{
	// Both halves, aligned to the size of the pair, as FIGURE 15-9 requires.
	// A misaligned base is not diagnosed by the hardware.
	size_t size = KERNEL_TSB_BYTES * 2;

	// The TSB has to be locked in the data TLB, because the miss handler reads
	// it with an atomic quad load and this CPU has no physical-address variant
	// of that instruction -- see section 4.4 of the design note. Locking 256 KB
	// as 8 KB pages would take 32 of the 64 entries, so it is locked as 64 KB
	// pages instead, which takes four.
	//
	// That needs the physical half 64 KB aligned and the whole span physically
	// contiguous, which vm_allocate_early() cannot promise: it takes physical
	// pages one at a time and its alignment argument constrains only the virtual
	// base. So the virtual range is taken from it with no physical backing at
	// all -- a physicalSize of zero maps nothing -- and the physical side is
	// gathered separately and mapped by the locked entries themselves.
	addr_t base = vm_allocate_early(args, size, 0,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, size);
	if (base == 0) {
		dprintf("sparc_mmu: could not allocate a %" B_PRIuSIZE " byte TSB\n",
			size);
		return B_NO_MEMORY;
	}

	const size_t kLockPageSize = 64 * 1024;
	phys_addr_t physicalBase;
	status_t status = sparc_allocate_aligned_physical(args, size, kLockPageSize,
		&physicalBase);
	if (status != B_OK)
		return status;

	int locked = sparc_tlb_lock_range(base, physicalBase, size, TTE_SIZE_64K,
		false, TTE_WRITABLE | TTE_PRIVILEGED | TTE_CACHEABLE_PHYSICAL
			| TTE_CACHEABLE_VIRTUAL);
	if (locked < 0)
		return B_ERROR;

	sKernelTsb = (TsbEntry*)base;
	sKernelTsbPhysical = physicalBase;
	sparc_add_locked_range(base, size);

	// The first write through the new mapping. If the locked entries were built
	// wrong this faults here, at a point where the firmware is still handling
	// traps and can still say so -- which is the whole reason for doing it
	// before the trap table is installed rather than after.
	memset(sKernelTsb, 0, size);

	dprintf("sparc_mmu: TSB at %#" B_PRIxADDR " (pa %#" B_PRIxPHYSADDR "), %"
		B_PRIu32 " entries per half, %" B_PRIuSIZE " KB total, locked in %d "
		"64 KB D-TLB entries\n", base, physicalBase,
		(uint32)KERNEL_TSB_ENTRIES, size / 1024, locked);

	// Warm it with what the firmware has mapped. These are the translations
	// the kernel inherits, and the ones it must be able to service itself once
	// it stops being able to fall back on the firmware.
	int mmuInstance;
	if (of_getprop(gChosen, "mmu", &mmuInstance, sizeof(int)) == OF_FAILED)
		return B_ERROR;
	intptr_t mmu = of_instance_to_package(mmuInstance);
	if (mmu == OF_FAILED)
		return B_ERROR;

	struct translation {
		void*		virtual_address;
		intptr_t	length;
		intptr_t	data;
	} translations[64];

	int length = of_getprop(mmu, "translations", &translations,
		sizeof(translations));
	if (length == OF_FAILED)
		return B_ERROR;
	length /= sizeof(struct translation);

	uint32 inserted = 0;
	for (int i = 0; i < length; i++) {
		struct translation* map = &translations[i];
		addr_t va = (addr_t)map->virtual_address;
		uint64 data = (uint64)map->data;
		phys_addr_t pa = data & TTE_PA_MASK;

		// The firmware describes regions, always with 8 KB pages, so expand
		// each into individual entries.
		for (addr_t offset = 0; offset < (addr_t)map->length;
				offset += B_PAGE_SIZE) {
			uint64 flags = data & (TTE_WRITABLE | TTE_PRIVILEGED | TTE_LOCKED
				| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL
				| TTE_SIDE_EFFECT);

			// The page table as well as the TSB, and this is not optional. The
			// TSB is direct-mapped on VA<25:13>, so two regions more than 64 MB
			// apart index to the same lines and the later one silently wins --
			// the loader's own allocations alias the kernel image exactly. Only
			// the page table can answer for whichever of them lost.
			sparc_page_table_early_map(args, va + offset, pa + offset,
				flags | TTE_SOFT_ACCESSED | TTE_SOFT_MODIFIED
					| TTE_SOFT_REAL_WRITABLE | TTE_GLOBAL);
			sparc_tsb_insert(va + offset, pa + offset, flags);
			inserted++;
		}
	}

	dprintf("sparc_mmu: warmed with %" B_PRIu32 " firmware pages, %" B_PRIu32
		" collisions (%" B_PRIu32 " distinct entries live)\n", inserted,
		sKernelTsbCollisions, inserted - sKernelTsbCollisions);

	sparc_verify_mmu_defines();
	sparc_verify_tsb_indexing();
	sparc_verify_tlb_load(args);
	sparc_verify_tsb_lookup();
	sparc_verify_trap_table();
	sparc_verify_trap_globals();
	sparc_lock_trap_pages();
	sparc_dump_tlb();
	sparc_install_trap_table(args);

	return B_OK;
}


/*!	Turns the TSB's virtual range into an area, so the VM stops handing it out.

	The range was taken with vm_allocate_early(), which records it in
	kernel_args -- and vm_init() reserves every one of those ranges, calls this
	architecture's init_post_area hook, and then *unreserves* them all. Anything
	the architecture still needs has to have become an area of its own by then.
	This one had not, and the consequence was as bad as it sounds: create_area()
	handed thread stacks out of the middle of the TSB.

	Which is a very quiet kind of wrong. A TSB entry is sixteen bytes, a tag and
	a TTE, and the miss handler writes one whenever it resolves a miss -- so an
	ordinary memory access by any thread would drop sixteen bytes on top of
	another thread's stack, at whichever offset the faulting address happened to
	index to. What that looked like was a register window coming back from a fill
	with %i6 and %i7 replaced: the tag of a virtual address in one, its
	translation in the other, and a return to an address that was never code.
	Everything else in the window was intact, because a spill writes 128 bytes
	and only two of its slots had been landed on.

	A null area rather than create_area(): as far as the VM is concerned nothing
	is mapped here. The TSB is reachable only through four locked entries in the
	data TLB and has no page table entries and no vm_page structures, because the
	miss handler reads it with an atomic quad load that exists in no
	physical-address form on this processor -- see section 4.4 of the design note.
	All that is wanted from the VM is that it never offer these addresses to
	anybody else.
*/
status_t
sparc_mmu_create_tsb_area()
{
	if (sKernelTsb == NULL)
		return B_NO_INIT;

	void* address = sKernelTsb;
	area_id area = vm_create_null_area(VMAddressSpace::KernelID(), "kernel TSB",
		&address, B_EXACT_ADDRESS, KERNEL_TSB_BYTES * 2, 0);
	if (area < B_OK) {
		panic("sparc_mmu: could not claim the TSB at %p: %s", sKernelTsb,
			strerror(area));
		return area;
	}

	dprintf("sparc_mmu: TSB range %p-%p claimed as area %" B_PRId32 "\n",
		sKernelTsb, (char*)sKernelTsb + KERNEL_TSB_BYTES * 2, area);

	return B_OK;
}


/*!	Checks that our index arithmetic agrees with the hardware's.

	This is the one property the whole fast path rests on: the miss handler
	takes a pointer straight out of ASI_DMMU_TSB_8KB_PTR and expects to find
	there whatever software put in. If the two disagree by so much as a bit,
	every lookup misses, the slow path runs for everything, and the symptom is
	a mysteriously slow machine rather than an obvious fault.

	Asking the hardware directly is the only real test. Program the TSB
	register, set the Tag Access register to a chosen virtual address, and read
	back the pointer the hardware forms; then compare it with the entry address
	sparc_tsb_insert() would use for the same address.

	This is safe to do here. Open Firmware keeps both TSB registers at zero --
	it walks its translation list in software and never uses the hardware TSB
	mechanism -- so programming them disturbs nothing. Tag Access is rewritten
	by the hardware on every MMU trap anyway. Both are restored regardless.
*/
/*	Checks that the assembly and this file agree about the MMU constants.

	Every one of them is duplicated into arch_traps.S, which cannot include a C++
	header, and the numbers are the kind that fail quietly. A page table shift
	that disagrees walks to a plausible wrong entry and returns a translation for
	some other page. A context shift that disagrees masks the wrong bits out of a
	tag target, so the comparison never matches, every access takes the slow path,
	and nothing says why -- the system would simply be slow forever.

	The same arrangement sparc_verify_context_layout() uses for arch_context, and
	for the same reason: the assembler's own view of each number, reported back
	and compared, rather than a comment asking the next reader to check.
*/
static void
sparc_verify_mmu_defines()
{
	uint64 assembler[11];
	sparc_mmu_defines(assembler);

	const uint64 declared[11] = {
		SPARC_SEGMENT_TABLE_SHIFT,
		SPARC_PAGE_DIRECTORY_SHIFT,
		SPARC_PAGE_TABLE_SHIFT,
		SPARC_PAGE_TABLE_MASK,
		SPARC_VA_HOLE_SHIFT,
		TTE_TAG_CONTEXT_SHIFT,
		TSB_TAG_KERNEL_BIT,
		TSB_TAG_VA_SHIFT,
		__builtin_ctzll(SPARC_WINDOW_SAVE_SLOT_SIZE),
		SPARC_WINDOW_SAVE_SLOTS,
		SPARC_WINDOW_SAVE_STACK_POINTER,
	};

	static const char* const kNames[11] = {
		"PT_SEGMENT_SHIFT", "PT_DIRECTORY_SHIFT", "PT_TABLE_SHIFT",
		"PT_INDEX_MASK", "PT_VA_HOLE_SHIFT", "TTE_TAG_CONTEXT_SHIFT",
		"TSB_TAG_KERNEL_BIT", "TSB_TAG_VA_SHIFT",
		"WINDOW_SAVE_SLOT_SHIFT", "WINDOW_SAVE_SLOTS",
		"WINDOW_SAVE_STACK_POINTER",
	};

	for (int i = 0; i < 11; i++) {
		if (assembler[i] != declared[i]) {
			panic("sparc mmu define %s: arch_traps.S says %" B_PRIu64
				", the headers say %" B_PRIu64, kNames[i], assembler[i],
				declared[i]);
		}
	}

	// And the one number that is derived rather than chosen. Both copies above
	// could agree with each other and still be wrong about the address space,
	// which is the failure this catches: the miss handler would mask the context
	// off the wrong half and every access in it would miss forever.
	//
	// The single-bit test is only valid if the kernel is exactly the top half of
	// the addresses the tag can represent -- a power-of-two base, and nothing
	// mapped for the kernel above it that the bit would not cover.
	if ((KERNEL_BASE & (KERNEL_BASE - 1)) != 0) {
		panic("sparc mmu: KERNEL_BASE %#lx is not a power of two, so one bit "
			"cannot separate kernel addresses from user ones", (addr_t)KERNEL_BASE);
	}

	// The save area's header has to be exactly one slot, or slot N is not where
	// the handler's shift says it is.
	if (offsetof(sparc_window_save, slots) != SPARC_WINDOW_SAVE_SLOT_SIZE) {
		panic("sparc window save: slots start at %d, not one slot in at %d",
			(int)offsetof(sparc_window_save, slots),
			(int)SPARC_WINDOW_SAVE_SLOT_SIZE);
	}

	uint32 kernelBit = __builtin_ctzll(KERNEL_BASE) - TSB_TAG_VA_SHIFT;
	if (kernelBit != TSB_TAG_KERNEL_BIT) {
		panic("sparc mmu: TSB_TAG_KERNEL_BIT is %d but KERNEL_BASE %#lx makes "
			"it %" B_PRIu32, TSB_TAG_KERNEL_BIT, (addr_t)KERNEL_BASE, kernelBit);
	}
}


void
sparc_verify_tsb_indexing()
{
	uint64_t savedTsb;
	asm volatile("ldxa [%[reg]] 0x58, %[dest]"
		: [dest] "=r"(savedTsb) : [reg] "r"(tsb));

	dprintf("sparc_mmu: firmware TSB registers: D %#018llx%s\n",
		(unsigned long long)savedTsb,
		savedTsb == 0 ? " (unused, so free to program)" : "");

	if (sKernelTsb == NULL)
		return;

	uint64_t tsbRegister = (uint64_t)(addr_t)sKernelTsb | TSB_SPLIT
		| KERNEL_TSB_SIZE;

	static const addr_t kProbes[] = {
		0x80000000,		// the kernel image
		0x81000000,		// where early_map starts handing out addresses
		0xfd000000,		// the frame buffer
		0xffd00000,		// Open Firmware's own code
	};

	uint32 mismatches = 0;
	for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); i++) {
		addr_t va = kProbes[i];
		uint64_t pointer;

		// Kept as one block so nothing can take a TLB miss between setting Tag
		// Access and reading the pointer back -- the hardware would overwrite
		// Tag Access on its way into the miss handler.
		asm volatile(
			"stxa %[tsbValue], [%[tsbReg]] 0x58\n\t"
			"membar #Sync\n\t"
			"stxa %[tagValue], [%[tagReg]] 0x58\n\t"
			"membar #Sync\n\t"
			"ldxa [%[zero]] 0x59, %[result]\n\t"
			"stxa %[savedValue], [%[tsbReg]] 0x58\n\t"
			"membar #Sync"
			: [result] "=&r"(pointer)
			: [tsbValue] "r"(tsbRegister), [tagValue] "r"((uint64_t)va),
			  [savedValue] "r"(savedTsb), [tsbReg] "r"(tsb),
			  [tagReg] "r"(tlb_tag_access), [zero] "r"(0UL)
			: "memory");

		addr_t expected = (addr_t)&sKernelTsb[(va >> 13)
			& (KERNEL_TSB_ENTRIES - 1)];

		bool ok = (addr_t)pointer == expected;
		if (!ok)
			mismatches++;

		dprintf("  va %#12lx -> hardware %#12llx  software %#12lx  %s\n", va,
			(unsigned long long)pointer, expected, ok ? "match" : "MISMATCH");
	}

	if (mismatches == 0) {
		dprintf("sparc_mmu: index arithmetic agrees with the hardware\n");
	} else {
		panic("sparc_mmu: TSB index arithmetic disagrees with the hardware on "
			"%" B_PRIu32 " of %" B_PRIuSIZE " probes", mismatches,
			sizeof(kProbes) / sizeof(kProbes[0]));
	}
}


/*!	Loads a translation directly into the data TLB.

	The tag comes from the Tag Access register rather than from the store, so
	it has to be set first; the store to ASI_DTLB_DATA_IN then triggers the
	atomic write of whichever entry the replacement algorithm picks. See the
	refill sequence in the IIi manual section 15.3.1, step 3.
*/
static void
sparc_tlb_load(addr_t virtualAddress, phys_addr_t physicalAddress, uint64 flags)
{
	uint64_t data = TTE_VALID | ((uint64_t)TTE_SIZE_8K << TTE_SIZE_SHIFT)
		| (physicalAddress & TTE_PA_MASK) | TTE_GLOBAL | flags;

	asm volatile(
		"stxa %[tag], [%[tagReg]] 0x58\n\t"
		"membar #Sync\n\t"
		"stxa %[data], [%[zero]] 0x5c\n\t"
		"membar #Sync"
		:
		: [tag] "r"((uint64_t)virtualAddress), [data] "r"(data),
		  [tagReg] "r"(tlb_tag_access), [zero] "r"(0UL)
		: "memory");
}


/*!	Proves that a TTE this code builds actually maps memory.

	Everything about the fast path so far has been read-only: the index
	arithmetic was checked against the hardware, but nothing has yet built a
	translation and used it. That code runs inside the miss handler, in
	assembly, where a mistake is far harder to see -- so exercise it here
	first, in C, where it can simply be printed.

	Maps a fresh physical page at an address nothing else uses, writes a
	pattern through it, and reads it back.
*/
static void
sparc_verify_tlb_load(struct kernel_args *args)
{
	// Well clear of everything: the kernel image ends by 0x80222000, early_map
	// works upward from 0x81000000, and the frame buffer starts at 0xfd000000.
	const addr_t kTestAddress = 0xa0000000;
	const uint64_t kPattern = 0x0123456789abcdefULL;

	page_num_t page = vm_allocate_early_physical_page(args);
	if (page == 0) {
		dprintf("sparc_mmu: no physical page for the TLB test\n");
		return;
	}
	phys_addr_t physicalAddress = (phys_addr_t)page * B_PAGE_SIZE;

	sparc_tlb_load(kTestAddress, physicalAddress, TTE_WRITABLE | TTE_PRIVILEGED
		| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL);

	volatile uint64_t* probe = (volatile uint64_t*)kTestAddress;
	*probe = kPattern;
	uint64_t read = *probe;

	dprintf("sparc_mmu: TLB load test: va %#lx -> pa %#llx, wrote %#llx read "
		"%#llx -- %s\n", kTestAddress,
		(unsigned long long)physicalAddress, (unsigned long long)kPattern,
		(unsigned long long)read, read == kPattern ? "OK" : "MISMATCH");

	// Leave nothing behind: this address is not part of anyone's address space
	// and the entry would only be a trap waiting for a later allocation.
	asm volatile(
		"stxa %%g0, [%[va]] 0x5f\n\t"
		"membar #Sync"
		:
		: [va] "r"((uint64_t)kTestAddress | 0x0)
		: "memory");
}


/*!	The TLB miss lookup, written in C so the algorithm can be checked apart
	from the assembly that will eventually run it.

	This is steps 2 and 3 of the refill sequence: take the pointer the hardware
	formed, read the TSB line it names, compare the tag, and on a hit the data
	half is what gets stored to the TLB. Getting this wrong in assembly, inside
	a handler with no stack, is expensive to debug; getting it wrong here costs
	a printf.

	The tag comparison deserves a note. The hardware's tag target is
	((tag_access & 0x1fff) << 48) | (tag_access >> 22): context in bits 60:48
	and VA<63:22> below. Our stored tags additionally carry the Global bit,
	which the manual describes as duplicated into the tag "to optimize the
	software miss handler" -- when set, the context is ignored during hit
	detection. Since every kernel entry here is Global and runs in context
	zero, masking the Global bit off the stored tag and comparing for equality
	is both correct and the cheapest form the handler can take.
*/
static bool
sparc_tsb_lookup(addr_t virtualAddress, uint64_t* _data)
{
	if (sKernelTsb == NULL)
		return false;

	uint32 index = (virtualAddress >> 13) & (KERNEL_TSB_ENTRIES - 1);
	TsbEntry* entry = &sKernelTsb[index];

	if (!entry->IsValid())
		return false;

	uint64_t target = ((uint64_t)virtualAddress >> 22);
	if (entry->fTag != target)
		return false;

	*_data = entry->fData;
	return true;
}


/*!	Checks the lookup against translations we know are present. */
static void
sparc_verify_tsb_lookup()
{
	// 0x80000000 is the kernel image, mapped by the firmware and warmed into
	// the TSB. 0xffe00000 is one of the firmware's own locked regions.
	static const addr_t kProbes[] = { 0x80000000, 0xffe00000 };

	for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); i++) {
		uint64_t data = 0;
		bool found = sparc_tsb_lookup(kProbes[i], &data);
		dprintf("sparc_mmu: lookup %#12lx -> %s pa %#12llx\n", kProbes[i],
			found ? "hit " : "MISS", (unsigned long long)(data & TTE_PA_MASK));
	}
}


/*!	Checks the trap table's geometry before anything relies on it.

	The table is laid out by the assembler, and the assembler's own checks --
	the .org directives in arch_traps.S -- catch a handler that outgrows its
	group. What they cannot catch is a handler landing at the wrong trap type,
	because that is a question about which offset means what rather than about
	sizes.

	Getting that wrong is close to undiagnosable after the fact: every trap
	would vector into some other handler's code, and the machine would misbehave
	in a way that looks nothing like a table problem. So verify it here, from a
	place where a failure can still be printed, and keep verifying it on every
	boot rather than trusting a check that was run once.

	The first instruction of each handler is distinctive enough to identify it.
*/
status_t
sparc_verify_trap_table()
{
	extern uint32 sparc_trap_table[];
	extern uint32 sparc_trap_table_end[];

	addr_t base = (addr_t)sparc_trap_table;
	size_t size = (addr_t)sparc_trap_table_end - base;

	dprintf("sparc_mmu: trap table at %#" B_PRIxADDR ", %" B_PRIuSIZE " bytes\n",
		base, size);

	if (size != 32768) {
		panic("sparc trap table is %" B_PRIuSIZE " bytes, expected 32768",
			size);
		return B_ERROR;
	}
	if ((base & 0x7fff) != 0) {
		panic("sparc trap table at %#" B_PRIxADDR " is not 32 KB aligned",
			base);
		return B_ERROR;
	}

	// The system call vector is checked differently, because a shared header
	// cannot cover it. The other four callers of SPARC_SYSCALL_TRAP now use one
	// definition, but the trap table encodes the number as a *position* -- the
	// handler is placed after that many unhandled entries -- and a position
	// cannot include a header. So decode the branch and see where it goes.
	extern char sparc_syscall_entry[];

	uint32 instruction = ((uint32*)base)[TRAP_SYSCALL * 8];
	if ((instruction & 0xffc00000) != 0x30800000) {
		panic("sparc trap table: trap type %#x is not the branch to the system "
			"call entry, it is %#" B_PRIx32 " -- SPARC_SYSCALL_TRAP and the "
			"table disagree", TRAP_SYSCALL, instruction);
		return B_ERROR;
	}

	// ba,a is a 22-bit word displacement, signed, from the branch itself.
	int32 displacement = (int32)(instruction << 10) >> 10;
	addr_t target = base + TRAP_SYSCALL * 32 + (addr_t)(displacement * 4);
	if (target != (addr_t)sparc_syscall_entry) {
		panic("sparc trap table: trap type %#x branches to %#" B_PRIxADDR
			", but sparc_syscall_entry is at %p", TRAP_SYSCALL, target,
			sparc_syscall_entry);
		return B_ERROR;
	}

	dprintf("sparc_mmu: system call vector %#x branches to sparc_syscall_entry "
		"at %p\n", TRAP_SYSCALL, sparc_syscall_entry);

	// Each entry is 32 bytes, so the first instruction of trap type t sits at
	// t * 8 words. Masks ignore the register and immediate fields and keep only
	// enough opcode to tell the handlers apart.
	struct {
		uint32		trapType;
		const char*	name;
		uint32		mask;
		uint32		expected;
	} checks[] = {
		// ldxa [%g0] ASI, %g2 -- the ASI is in bits 12:5.
		{ 0x064, "instruction MMU miss", 0xffffffff, 0xc4d80a20 },
		{ 0x068, "data MMU miss",        0xffffffff, 0xc4d80b20 },
		{ 0x268, "data MMU miss (TL>0)", 0xffffffff, 0xc4d80b20 },
		// stx %l0, [%sp + 0x7ff] and ldx [%sp + 0x7ff], %l0.
		{ 0x080, "spill (normal)",       0xffffffff, 0xe073a7ff },
		{ 0x0c0, "fill (normal)",        0xffffffff, 0xe05ba7ff },
		// The _other pair go somewhere else entirely -- a window that belongs to
		// userspace is saved into this thread's save area, not to the address
		// userspace chose -- so they start by loading that area's pointer out of
		// the trap data block: ldx [%g7 + TRAP_DATA_WINDOW_SAVE], %g1.
		{ 0x0a0, "spill (other)",        0xffffffff, 0xc259e0d0 },
		{ 0x0e0, "fill (other)",         0xffffffff, 0xc259e0d0 },
		// clr %l0. gas emits the register form, or %g0, %g0, %l0, rather than
		// the immediate one; the mask keeps the opcode, rd and rs1 and ignores
		// which form was chosen.
		{ 0x024, "clean window",         0xffffc000, 0xa0100000 },
		// ba,a -- only the opcode and the annul and condition fields matter,
		// since the displacement depends on where the handler landed.
		{ 0x000, "unhandled (reset)",    0xffc00000, 0x30800000 },
		{ 0x100, "unhandled (soft trap)",0xffc00000, 0x30800000 },
	};

	uint32 failures = 0;
	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		uint32 found = sparc_trap_table[checks[i].trapType * 8];
		if ((found & checks[i].mask) != checks[i].expected) {
			dprintf("sparc_mmu: trap %#" B_PRIx32 " (%s): found %#" B_PRIx32
				", expected %#" B_PRIx32 "\n", checks[i].trapType,
				checks[i].name, found, checks[i].expected);
			failures++;
		}
	}

	if (failures != 0) {
		panic("sparc trap table: %" B_PRIu32 " handlers are in the wrong place",
			failures);
		return B_ERROR;
	}

	dprintf("sparc_mmu: trap table geometry verified, %" B_PRIuSIZE
		" handlers in place\n", sizeof(checks) / sizeof(checks[0]));
	return B_OK;
}


/*!	The trap handlers' per-CPU data block.

	Statically allocated rather than taken from the early allocator, for the same
	reason the handlers themselves are in the kernel image: the page it lands on
	is one Open Firmware already mapped and locked in order to load the kernel,
	so a handler can reach it without that reach being the thing that faults.

	Aligned to its own size, which is one cache line, so a store from a trap
	handler cannot straddle two lines and cannot share a line with anything a
	handler does not own.
*/
static sparc_trap_data sTrapData __attribute__((aligned(TRAP_DATA_SIZE)));

static_assert(sizeof(sparc_trap_data) == TRAP_DATA_SIZE,
	"the trap data block must be exactly as large as it is aligned, or it can "
	"straddle a page boundary and leave part of itself outside the locked TLB "
	"entry -- see TRAP_DATA_SIZE");


/*!	Reports a trap the kernel could not handle, from TL=0.

	Reached by the slow path overwriting %tnpc and executing done, so this runs
	on the interrupted thread's stack with the normal register bank restored and
	the trap level back to zero. That is what makes it possible to call panic()
	at all: everything about trap context -- no stack, alternate globals, a trap
	level that turns the next fault into a watchdog reset -- rules out reporting
	from where the failure was detected.

	The faulting address is reassembled rather than read directly, because the
	Tag Access register holds VA<63:13> in the top bits with the context in the
	low thirteen. Shifting the context off and back leaves the page address.

	This does not return.
*/
extern "C" void
sparc_report_unresolved_miss()
{
	if (sTrapData.trapKind == SPARC_TRAP_UNRESOLVED_MISS) {
		// Tag Access holds VA<63:13> with the context in the low thirteen bits,
		// so shifting the context off leaves the page address.
		uint64 tagAccess = sTrapData.missTagAccess;
		panic("sparc: no page table entry for %#" B_PRIxADDR " (context %"
			B_PRIu64 ", trap %#" B_PRIx64 " at pc %#" B_PRIx64 ", tl %" B_PRIu64
			", leaf %#" B_PRIx64 ", %" B_PRIu64 " unresolved so far)",
			(addr_t)(tagAccess & ~(uint64)0x1fff), tagAccess & 0x1fff,
			sTrapData.missTrapType, sTrapData.missTpc, sTrapData.missTl,
			sTrapData.missEntry, sTrapData.missCount);
	} else {
		// Everything that matters, in one line, before anything else. Not for
		// brevity: a fault inside this function's own reporting re-enters it and
		// overwrites sTrapData, so a report split across several calls can lose
		// the part that identifies the failure and print eight window dumps of
		// the recursion instead. That is not hypothetical -- panic() needs the
		// current thread, and the fault this was written for was a lost thread
		// pointer, so the panic below faulted in turn and the cascade ran to
		// MAXTL. Which is why %g7 is here as well: when it is not a kernel
		// address, nothing further in this function can work, and this line is
		// the whole report.
		uint64 threadPointer;
		asm volatile("mov %%g7, %0" : "=r"(threadPointer));

		dprintf("sparc: trap %#" B_PRIx64 " at pc %#" B_PRIx64 ", address %#"
			B_PRIx64 ", tl %" B_PRIu64 ", tstate %#" B_PRIx64 ", called from %#"
			B_PRIx64 ", returns to %#" B_PRIx64 ", %%g7 %#" B_PRIx64 "\n",
			sTrapData.missTrapType, sTrapData.missTpc, sTrapData.missTagAccess,
			sTrapData.missTl, sTrapData.missTstate, sTrapData.trapCallSite,
			sTrapData.trapReturnAddress, threadPointer);

		// The thread's stack, because a register window that came back wrong is
		// most often a stack that was written over or run off the end of, and
		// the trapped locals below are frequently stack addresses -- which only
		// mean something next to the range they are supposed to be inside.
		// Guarded on %g7 looking like a kernel address, since reading a Thread
		// through a bad thread pointer is how this function last failed.
		if (threadPointer >= KERNEL_BASE
			&& threadPointer < KERNEL_BASE + KERNEL_SIZE) {
			Thread* thread = (Thread*)threadPointer;
			dprintf("sparc: thread %" B_PRId32 " \"%s\", kernel stack %#"
				B_PRIxADDR " to %#" B_PRIxADDR "\n", thread->id, thread->name,
				thread->kernel_stack_base, thread->kernel_stack_top);
		}

		uint64 windows = sTrapData.trapWindowState;
		dprintf("sparc: trap %#" B_PRIx64 " window cwp %" B_PRIu64 " cansave %"
			B_PRIu64 " canrestore %" B_PRIu64 " cleanwin %" B_PRIu64
			" otherwin %" B_PRIu64 "\n", sTrapData.missTrapType,
			windows & 0xff, (windows >> 8) & 0xff, (windows >> 16) & 0xff,
			(windows >> 24) & 0xff, (windows >> 32) & 0xff);
		dprintf("sparc: trapped locals %#" B_PRIx64 " %#" B_PRIx64 " %#"
			B_PRIx64 " %#" B_PRIx64 "\n", sTrapData.trapLocals[0],
			sTrapData.trapLocals[1], sTrapData.trapLocals[2],
			sTrapData.trapLocals[3]);
		dprintf("sparc:                %#" B_PRIx64 " %#" B_PRIx64 " %#"
			B_PRIx64 " %#" B_PRIx64 "\n", sTrapData.trapLocals[4],
			sTrapData.trapLocals[5], sTrapData.trapLocals[6],
			sTrapData.trapLocals[7]);

		panic("sparc: unhandled trap %#" B_PRIx64 " at pc %#" B_PRIx64
			" (tl %" B_PRIu64 ", tstate %#" B_PRIx64 ", fault address %#"
			B_PRIx64 ", called from %#" B_PRIx64 ", frame returns to %#"
			B_PRIx64 ")", sTrapData.missTrapType, sTrapData.missTpc,
			sTrapData.missTl, sTrapData.missTstate, sTrapData.missTagAccess,
			sTrapData.trapCallSite, sTrapData.trapReturnAddress);
	}

	// panic() returns if the user continues from KDL, and there is nothing
	// sensible to continue into: the access that faulted was skipped rather than
	// retried, so the caller's state is already inconsistent.
	for (;;)
		;
}


/*!	Installs the trap data pointer and proves it went where it should.

	Three separate things can be wrong here and each fails silently:

	The two copies of the field offsets -- this file's structure and
	arch_traps.S's defines -- can drift apart, in which case handlers write over
	each other's fields and every recorded value is attributed to the wrong name.

	The bank switch in sparc_set_trap_globals() can select the wrong bank, or
	fail to select one at all, in which case %g7 is set in the normal bank; the
	kernel's own %g7 is then corrupt and the handlers still have nothing.

	And the hardware may simply not implement the alternate banks the way the
	manual says, which is the assumption this whole handler design rests on.

	So check all three, before anything depends on any of them.
*/
status_t
sparc_verify_trap_globals()
{
	// The structure's own opinion of its layout, against the assembler's.
	uint64 assemblerOffsets[TRAP_DATA_OFFSET_COUNT];
	sparc_trap_data_offsets(assemblerOffsets);

	const uint64 declaredOffsets[TRAP_DATA_OFFSET_COUNT] = {
		offsetof(sparc_trap_data, tsbBase),
		offsetof(sparc_trap_data, pageTableRoot),
		offsetof(sparc_trap_data, missCount),
		offsetof(sparc_trap_data, missTagTarget),
		offsetof(sparc_trap_data, missTsbPointer),
		offsetof(sparc_trap_data, missTagAccess),
		offsetof(sparc_trap_data, missEntry),
		offsetof(sparc_trap_data, missTrapType),
		offsetof(sparc_trap_data, missTpc),
		offsetof(sparc_trap_data, missTstate),
		offsetof(sparc_trap_data, missTl),
		offsetof(sparc_trap_data, reportHandler),
		offsetof(sparc_trap_data, trapKind),
		offsetof(sparc_trap_data, trapCallSite),
		offsetof(sparc_trap_data, trapReturnAddress),
		offsetof(sparc_trap_data, trapWindowState),
		offsetof(sparc_trap_data, trapLocals),
		offsetof(sparc_trap_data, userPageTableRoot),
		offsetof(sparc_trap_data, kernelStackTop),
		offsetof(sparc_trap_data, windowSave),
		offsetof(sparc_trap_data, currentThread),
		offsetof(sparc_trap_data, userSpillCount),
		offsetof(sparc_trap_data, otherSpillCount),
		offsetof(sparc_trap_data, otherFillCount),
		offsetof(sparc_trap_data, winfixupAddress),
		offsetof(sparc_trap_data, winfixupTrapType),
		offsetof(sparc_trap_data, winfixupCount),
		offsetof(sparc_trap_data, faultTagAccess),
	};

	for (int i = 0; i < TRAP_DATA_OFFSET_COUNT; i++) {
		if (assemblerOffsets[i] != declaredOffsets[i]) {
			panic("sparc trap data offset %d: arch_traps.S says %#" B_PRIx64
				", arch_mmu.h says %#" B_PRIx64, i, assemblerOffsets[i],
				declaredOffsets[i]);
			return B_ERROR;
		}
	}

	if (sizeof(sparc_trap_data) != TRAP_DATA_SIZE) {
		panic("struct sparc_trap_data is %" B_PRIuSIZE " bytes, expected %d",
			sizeof(sparc_trap_data), TRAP_DATA_SIZE);
		return B_ERROR;
	}

	// Remember what the kernel's own %g7 holds, so the check below can tell
	// whether the bank switch actually happened. Writing the normal bank by
	// mistake is the failure that would be least obvious and most destructive.
	uint64 normalBefore;
	asm volatile("mov %%g7, %0" : "=r"(normalBefore));

	sparc_set_trap_globals(&sTrapData, sparc_kernel_page_table());

	uint64 normalAfter;
	asm volatile("mov %%g7, %0" : "=r"(normalAfter));
	uint64 mmuGlobal = sparc_read_trap_globals(SPARC_GLOBALS_MMU);
	uint64 alternateGlobal = sparc_read_trap_globals(SPARC_GLOBALS_ALTERNATE);
	uint64 mmuRoot = sparc_read_trap_page_table(SPARC_GLOBALS_MMU);

	dprintf("sparc_mmu: trap data at %p, %%g7 mmu %#" B_PRIx64 " alternate %#"
		B_PRIx64 ", normal %#" B_PRIx64 " -> %#" B_PRIx64 "\n", &sTrapData,
		mmuGlobal, alternateGlobal, normalBefore, normalAfter);

	if (normalAfter != normalBefore) {
		panic("sparc_set_trap_globals wrote the normal global bank: %%g7 went "
			"from %#" B_PRIx64 " to %#" B_PRIx64, normalBefore, normalAfter);
		return B_ERROR;
	}
	if (mmuGlobal != (uint64)(addr_t)&sTrapData) {
		panic("sparc mmu-global %%g7 is %#" B_PRIx64 ", expected %p",
			mmuGlobal, &sTrapData);
		return B_ERROR;
	}
	if (alternateGlobal != (uint64)(addr_t)&sTrapData) {
		panic("sparc alternate-global %%g7 is %#" B_PRIx64 ", expected %p",
			alternateGlobal, &sTrapData);
		return B_ERROR;
	}

	// The banks must be distinct from the normal one, or the "private set" the
	// stack-free handlers rely on does not exist and they would be scribbling
	// on the interrupted code's registers. Equal values here would mean the
	// writes above landed in one shared bank and merely looked correct.
	if (mmuGlobal == normalAfter && normalAfter != 0) {
		dprintf("sparc_mmu: WARNING: mmu and normal %%g7 read alike; the "
			"alternate banks may not be implemented\n");
	}

	if (mmuRoot != (uint64)sparc_kernel_page_table()) {
		panic("sparc mmu-global %%g3 is %#" B_PRIx64 ", expected the page table "
			"root %#" B_PRIxPHYSADDR, mmuRoot, sparc_kernel_page_table());
		return B_ERROR;
	}

	dprintf("sparc_mmu: trap globals verified in both banks, page table root %#"
		B_PRIxPHYSADDR "\n", sparc_kernel_page_table());
	return B_OK;
}


/*!	Reads one TLB entry's tag and data.

	The entry index goes in VA<8:3> of the address handed to the ASI, with the
	low three bits zero (FIGURE 15-13, printed p.230). Each read is an internal
	MMU operation rather than a memory access, hence the membar after it: without
	one the result can be observed out of order with respect to the next.
*/
static void
read_tlb_entry(int entry, bool instruction, uint64 *_tag, uint64 *_data)
{
	uint64 address = SPARC_TLB_ENTRY_ADDRESS(entry);
	uint64 tag;
	uint64 data;

	if (instruction) {
		// ASI_IMMU_TLB_TAG 0x56, ASI_IMMU_TLB_DATA 0x55.
		asm volatile("ldxa [%[address]] 0x56, %[tag]\n\t"
			"membar #Sync\n\t"
			"ldxa [%[address]] 0x55, %[data]\n\t"
			"membar #Sync"
			: [tag] "=&r"(tag), [data] "=r"(data)
			: [address] "r"(address));
	} else {
		// ASI_DMMU_TLB_TAG 0x5e, ASI_DMMU_TLB_DATA 0x5d.
		asm volatile("ldxa [%[address]] 0x5e, %[tag]\n\t"
			"membar #Sync\n\t"
			"ldxa [%[address]] 0x5d, %[data]\n\t"
			"membar #Sync"
			: [tag] "=&r"(tag), [data] "=r"(data)
			: [address] "r"(address));
	}

	*_tag = tag;
	*_data = data;
}


/*!	Writes one TLB entry directly, with the Lock bit set.

	Data Access is the ASI that names an entry explicitly, as opposed to Data In
	which lets the replacement algorithm choose. The tag does not come from the
	store: section 15.9.9 says the entry tag is taken from the current contents
	of the Tag Access register, so that has to be written first and the pair is
	only atomic from the store onwards.

	The demap first is not belt-and-braces. The manual warns that a store to Data
	In "is not guaranteed to replace the previous TLB entry causing a fault", and
	that to change an entry's attribute bits software must explicitly demap the
	old entry first, "otherwise, a multiple match error condition can result" --
	and a multiple match is documented as having undefined results, not as being
	detected. Since these pages are already mapped by ordinary unlocked entries
	when this runs, that is exactly the situation being walked into.
*/
static void
sparc_tlb_lock_entry(int entry, addr_t virtualAddress,
	phys_addr_t physicalAddress, uint64 pageSize, bool instruction,
	uint64 flags)
{
	uint64 tag = (uint64)virtualAddress & TLB_TAG_VA_MASK;
	uint64 data = TTE_VALID | (pageSize << TTE_SIZE_SHIFT)
		| ((uint64)physicalAddress & TTE_PA_MASK) | TTE_LOCKED | TTE_GLOBAL
		| flags;
	uint64 address = SPARC_TLB_ENTRY_ADDRESS(entry);

	// Demap address format is FIGURE 15-15 (printed p.231): VA<63:13>, then
	// type at bit 6 and context at bits 5:4. Type 0 is demap-page and context
	// 00 is primary, which is what the existing TLB test uses; for the Global
	// entries written here the context is ignored during matching anyway.
	uint64 demap = tag;

	if (instruction) {
		// ASI_IMMU 0x50, ASI_IMMU_DEMAP 0x57, ASI_IMMU_TLB_DATA 0x55.
		asm volatile(
			"stxa %[demap], [%[demap]] 0x57\n\t"
			"membar #Sync\n\t"
			"stxa %[tag], [%[tagReg]] 0x50\n\t"
			"membar #Sync\n\t"
			"stxa %[data], [%[address]] 0x55\n\t"
			"membar #Sync"
			:
			: [demap] "r"(demap), [tag] "r"(tag), [data] "r"(data),
			  [tagReg] "r"(tlb_tag_access), [address] "r"(address)
			: "memory");
	} else {
		// ASI_DMMU 0x58, ASI_DMMU_DEMAP 0x5f, ASI_DMMU_TLB_DATA 0x5d.
		asm volatile(
			"stxa %[demap], [%[demap]] 0x5f\n\t"
			"membar #Sync\n\t"
			"stxa %[tag], [%[tagReg]] 0x58\n\t"
			"membar #Sync\n\t"
			"stxa %[data], [%[address]] 0x5d\n\t"
			"membar #Sync"
			:
			: [demap] "r"(demap), [tag] "r"(tag), [data] "r"(data),
			  [tagReg] "r"(tlb_tag_access), [address] "r"(address)
			: "memory");
	}
}


/*!	Finds a TLB entry that may be overwritten, searching from the top down.

	Invalid entries first, then valid but unlocked ones -- those are backed by
	the TSB, or will be once it is authoritative, so losing one costs a refill
	and nothing else. A locked entry is never a candidate: the five the firmware
	holds are its I/O and OBP mappings, and the console we are printing on is
	among them.

	Top down because the firmware put its locked entries at the bottom, so
	descending finds room without stepping over them.
*/
static int
sparc_find_tlb_entry(bool instruction)
{
	int candidate = -1;

	for (int entry = SPARC_TLB_ENTRIES - 1; entry >= 0; entry--) {
		uint64 tag;
		uint64 data;
		read_tlb_entry(entry, instruction, &tag, &data);

		if ((data & TTE_LOCKED) != 0)
			continue;
		if ((data & TTE_VALID) == 0)
			return entry;
		if (candidate < 0)
			candidate = entry;
	}

	return candidate;
}


/*!	Locks a range of memory into one or both TLBs.

	Returns the number of entries used, or -1 if the TLB ran out of room.
*/
static int
sparc_tlb_lock_range(addr_t virtualAddress, phys_addr_t physicalAddress,
	size_t size, uint64 pageSize, bool instruction, uint64 flags)
{
	size_t pageBytes = (size_t)B_PAGE_SIZE << (3 * pageSize);
		// 8K, 64K, 512K, 4M -- each step is 8 times the last (TABLE 15-1).
	int used = 0;

	for (size_t offset = 0; offset < size; offset += pageBytes) {
		int entry = sparc_find_tlb_entry(instruction);
		if (entry < 0) {
			dprintf("sparc_mmu: no %s TLB entry available to lock %#" B_PRIxADDR
				"\n", instruction ? "I" : "D", virtualAddress + offset);
			return -1;
		}

		sparc_tlb_lock_entry(entry, virtualAddress + offset,
			physicalAddress + offset, pageSize, instruction, flags);
		used++;
	}

	return used;
}


/*!	Allocates physical memory that is both contiguous and aligned.

	Needed because vm_allocate_early() cannot do it: it takes physical pages one
	at a time and its alignment argument constrains only the virtual base. A
	large TTE needs its physical half aligned to the page size and the whole span
	physically contiguous, so the pages have to be gathered here and checked.

	The early physical allocator normally grows one range upwards a page at a
	time, which does produce contiguous runs -- but it can also expand downwards
	or start a new range, so contiguity is something to verify rather than
	assume. Pages taken before an aligned run begins, and pages of a run that
	turns out to be broken, are simply left allocated: a few tens of kilobytes
	lost once at boot is not worth the bookkeeping to reclaim.
*/
static status_t
sparc_allocate_aligned_physical(kernel_args *args, size_t size,
	size_t alignment, phys_addr_t *_physicalBase)
{
	size_t neededPages = size / B_PAGE_SIZE;
	// Enough slack to skip into alignment once and then survive one broken run.
	size_t budget = alignment / B_PAGE_SIZE + 2 * neededPages + 2;

	phys_addr_t runStart = 0;
	size_t runPages = 0;
	phys_addr_t previous = 0;

	for (size_t taken = 0; taken < budget; taken++) {
		page_num_t page = vm_allocate_early_physical_page(args);
		if (page == 0)
			break;
		phys_addr_t address = (phys_addr_t)page * B_PAGE_SIZE;

		if (runPages > 0 && address == previous + B_PAGE_SIZE) {
			runPages++;
		} else if ((address & (alignment - 1)) == 0) {
			runStart = address;
			runPages = 1;
		} else {
			runPages = 0;
		}
		previous = address;

		if (runPages == neededPages) {
			*_physicalBase = runStart;
			return B_OK;
		}
	}

	dprintf("sparc_mmu: could not find %" B_PRIuSIZE " contiguous bytes of "
		"physical memory aligned to %" B_PRIuSIZE "\n", size, alignment);
	return B_NO_MEMORY;
}


/*!	Dumps both TLBs, which is the only way to find out what the firmware locked.

	This matters before installing our own trap table. The TLB miss handler is
	only safe if the pages it touches -- its own instructions, the TSB it reads,
	and the trap data block it writes -- can never themselves miss, because a
	miss taken inside the miss handler nests, and a handful of nested traps ends
	in a watchdog reset with nothing reported.

	The usual way to guarantee that is to lock those pages in the TLB. But Open
	Firmware had to map the kernel in order to load and enter it, and if it
	locked what it mapped then some of that guarantee already exists and the
	question is only what is missing. Guessing either way is expensive: assume
	too little and we lock entries the firmware is still relying on, assume too
	much and the first cutover resets the machine.
*/
void
sparc_dump_tlb()
{
	static const char *kSizeNames[] = { "8K", "64K", "512K", "4M" };

	for (int instruction = 0; instruction < 2; instruction++) {
		int valid = 0;
		int locked = 0;

		dprintf("sparc_mmu: %s TLB:\n", instruction ? "I" : "D");

		for (int entry = 0; entry < SPARC_TLB_ENTRIES; entry++) {
			uint64 tag;
			uint64 data;
			read_tlb_entry(entry, instruction != 0, &tag, &data);

			if ((data & TTE_VALID) == 0)
				continue;
			valid++;
			if ((data & TTE_LOCKED) != 0)
				locked++;

			dprintf("  %2d va %#016" B_PRIx64 " ctx %4" B_PRIu64 " -> pa %#011"
				B_PRIx64 " %-5s %s%s%s%s%s\n", entry,
				(uint64)(tag & TLB_TAG_VA_MASK),
				(uint64)(tag & TLB_TAG_CONTEXT_MASK),
				(uint64)(data & TTE_PA_MASK),
				kSizeNames[(data >> TTE_SIZE_SHIFT) & TTE_SIZE_MASK],
				(data & TTE_LOCKED) != 0 ? "locked " : "",
				(data & TTE_PRIVILEGED) != 0 ? "priv " : "",
				(data & TTE_WRITABLE) != 0 ? "write " : "",
				(data & TTE_GLOBAL) != 0 ? "global " : "",
				(data & TTE_SIDE_EFFECT) != 0 ? "side-effect " : "");
		}

		dprintf("sparc_mmu: %s TLB has %d valid entries, %d locked, %d free\n",
			instruction ? "I" : "D", valid, locked,
			SPARC_TLB_ENTRIES - valid);
	}
}


/*!	Locks every page a trap handler touches while a trap is in progress.

	Section 2.6 of the design note lists what this has to cover, and section 4.4
	explains why the firmware's own locked entries do not help: it locked only
	its I/O and OBP mappings, leaving every kernel page an ordinary replaceable
	8 KB entry -- the trap table's included.

	Three ranges, each for a different reason:

	The trap handler code and the table are instruction fetches, so they go in
	the I-TLB. A fetch that missed here would be a miss taken inside the miss
	handler, and the code needed to service it is the code that could not be
	fetched.

	The trap data block is a store target, so it goes in the D-TLB. Only the slow
	path writes it today, but the slow path is where a fault would be least
	welcome.

	The TSB is locked where it is allocated, in sparc_mmu_init_tsb(), because it
	is the only one of the three whose physical address this code chooses.

	Physical addresses come from the TSB rather than from the firmware, since the
	TSB has by now been warmed from the firmware's own translations and looking
	them up there exercises the lookup path as a side effect. A miss means the
	page was never mapped by the firmware, which would be worth knowing about
	regardless of what it is wanted for.
*/
status_t
sparc_lock_trap_pages()
{
	extern uint32 sparc_trap_handlers_start[];
	extern uint32 sparc_trap_handlers_end[];
	extern uint32 sparc_trap_table[];
	extern uint32 sparc_trap_table_end[];

	struct {
		addr_t		start;
		addr_t		end;
		bool		instruction;
		const char*	name;
	} ranges[] = {
		{ (addr_t)sparc_trap_handlers_start, (addr_t)sparc_trap_handlers_end,
			true, "trap handlers" },
		{ (addr_t)sparc_trap_table, (addr_t)sparc_trap_table_end, true,
			"trap table" },
		{ (addr_t)&sTrapData, (addr_t)&sTrapData + sizeof(sTrapData), false,
			"trap data" },
	};

	int total = 0;

	for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		addr_t first = ranges[i].start & ~(addr_t)(B_PAGE_SIZE - 1);
		addr_t last = (ranges[i].end + B_PAGE_SIZE - 1)
			& ~(addr_t)(B_PAGE_SIZE - 1);
		int locked = 0;

		for (addr_t page = first; page < last; page += B_PAGE_SIZE) {
			// From the page table, not the TSB. The TSB is a direct-mapped
			// cache and any given line may currently belong to a colliding
			// address, so a miss there means nothing about whether the page is
			// mapped. Asking the cache what the record says is how the first
			// version of this got the wrong answer.
			phys_addr_t entry = sparc_page_table_lookup(
				sparc_kernel_page_table(), page, NULL);
			uint64 data = entry != 0 ? sparc_read_physical(entry) : 0;
			if ((data & TTE_VALID) == 0) {
				panic("sparc_mmu: %s page %#" B_PRIxADDR " is not in the page "
					"table, so it cannot be locked", ranges[i].name, page);
				return B_ERROR;
			}
			phys_addr_t physical = data & TTE_PA_MASK;

			int used = sparc_tlb_lock_range(page, physical, B_PAGE_SIZE,
				TTE_SIZE_8K, ranges[i].instruction,
				TTE_PRIVILEGED | TTE_CACHEABLE_PHYSICAL
					| TTE_CACHEABLE_VIRTUAL
					| (ranges[i].instruction ? 0 : TTE_WRITABLE));
			if (used < 0)
				return B_ERROR;
			locked += used;
		}

		sparc_add_locked_range(first, last - first);

		dprintf("sparc_mmu: locked %s, %#" B_PRIxADDR "-%#" B_PRIxADDR ", in %d "
			"%s-TLB entries\n", ranges[i].name, first, last, locked,
			ranges[i].instruction ? "I" : "D");
		total += locked;
	}

	dprintf("sparc_mmu: %d pages locked for the trap path\n", total);
	return B_OK;
}


/*!	Proves the hardware builds the tag the miss handler will be compared against.

	sparc_tsb_insert() stores VA<63:22> and nothing else, on the grounds that the
	hardware's tag target is (context << 48) | VA<63:22> and the context is zero
	throughout the kernel. The single xor in the fast path depends entirely on
	that being true, and if it is not, the comparison never matches: every access
	misses, the slow path stops the machine on the first one, and nothing says
	why.

	The context part is the assumption worth testing, because it is about state
	the firmware set up rather than about arithmetic. At TL>0 the MMU uses the
	nucleus context, which is zero by definition, but the kernel spends nearly
	all its time at TL=0 where the primary context register decides -- and that
	register belongs to whatever the firmware left behind.

	Rather than read the context registers and reason about them, this writes an
	address into the Tag Access register and reads back the tag target the
	hardware forms from it, which is the exact value the handler will see.
*/
static status_t
sparc_verify_tag_target()
{
	uint64 primaryContext;
	uint64 secondaryContext;
	asm volatile("ldxa [%[primary]] 0x58, %[primaryValue]\n\t"
		"ldxa [%[secondary]] 0x58, %[secondaryValue]"
		: [primaryValue] "=&r"(primaryContext),
		  [secondaryValue] "=r"(secondaryContext)
		: [primary] "r"(primary_context), [secondary] "r"(secondary_context));

	dprintf("sparc_mmu: contexts: primary %" B_PRIu64 ", secondary %" B_PRIu64
		"\n", primaryContext, secondaryContext);

	static const addr_t kProbes[] = {
		0x80000000,		// the kernel image
		0x801b8000,		// the trap table itself
		0x8023e000,		// the trap data block
		0xffd00000,		// Open Firmware's own code
	};

	uint32 mismatches = 0;
	for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); i++) {
		uint64 hardware;
		asm volatile(
			"stxa %[tag], [%[tagReg]] 0x58\n\t"
			"membar #Sync\n\t"
			"ldxa [%[target]] 0x58, %[result]"
			: [result] "=r"(hardware)
			: [tag] "r"((uint64)kProbes[i]), [tagReg] "r"(tlb_tag_access),
			  [target] "r"(tsb_tag_target)
			: "memory");

		uint64 software = kProbes[i] >> 22;
		bool ok = hardware == software;
		if (!ok)
			mismatches++;

		dprintf("sparc_mmu: tag target %#010" B_PRIxADDR " -> hardware %#014"
			B_PRIx64 ", stored %#014" B_PRIx64 " -- %s\n", kProbes[i], hardware,
			software, ok ? "match" : "MISMATCH");
	}

	if (mismatches != 0) {
		panic("sparc_mmu: the hardware's tag target disagrees with the tags in "
			"the TSB on %" B_PRIu32 " probes; the fast path would never hit",
			mismatches);
		return B_ERROR;
	}

	return B_OK;
}


/*!	Hands the MMU and the trap table to the kernel.

	This is the point the whole phase has been working towards, and it is a step
	that cannot be taken halfway: from the store to %tba onwards, every trap the
	machine takes goes to our handlers instead of the firmware's, including the
	window spills and fills that ordinary C code generates by the thousand.

	Order matters more than anything else here. The TSB registers go first,
	because a trap taken after %tba was set but before the TSB base was
	programmed would send the fast path to read a TSB at address zero -- it would
	find whatever is there, treat it as a translation, and load it into the TLB.
	Programming the TSB first is harmless by comparison: nothing reads those
	registers until a handler does.

	Interrupts come off for the window between the two, and stay off, because
	interrupt handling has its own requirements on the trap table that nothing
	has yet built.
*/
status_t
sparc_install_trap_table(struct kernel_args *args)
{
	if (sKernelTsb == NULL)
		return B_NO_INIT;

	if (sparc_verify_tag_target() != B_OK)
		return B_ERROR;

	extern uint32 sparc_trap_table[];

	// FIGURE 15-9: base<63:13>, Split at bit 12, Size in bits 3:0.
	uint64 tsbRegister = ((uint64)(addr_t)sKernelTsb & TSB_BASE_MASK)
		| TSB_SPLIT | KERNEL_TSB_SIZE;
	uint64 trapTable = (uint64)(addr_t)sparc_trap_table;

	sTrapData.tsbBase = sKernelTsbPhysical;
	sTrapData.pageTableRoot = sparc_kernel_page_table();

	// The kernel's root, not zero, so that a user address faulting with no team
	// current walks a real table and finds nothing rather than reading the
	// bottom of physical memory as a page table. The context switch replaces it.
	sTrapData.userPageTableRoot = sparc_kernel_page_table();

	// Whatever is current now, so a trap taken before the first context switch
	// loads a real thread pointer rather than zero. The boot thread is a real
	// Thread; it simply has an id of 0.
	sTrapData.currentThread = (uint64)(addr_t)thread_get_current_thread();

	// Where an unresolved miss goes to be reported. Set before %tba, so the very
	// first miss after the cutover already has somewhere to complain to.
	sTrapData.reportHandler = (uint64)(addr_t)&sparc_report_unresolved_miss;

	dprintf("sparc_mmu: installing TSB register %#018" B_PRIx64 " and %%tba %#"
		B_PRIx64 "\n", tsbRegister, trapTable);

	// Both MMUs get the same TSB. They have separate registers because they can
	// have separate TSBs, but the kernel's mappings are one set and splitting
	// them would only mean maintaining the same data twice.
	uint64 savedPstate;
	uint64 scratch;
	asm volatile(
		"rdpr %%pstate, %[saved]\n\t"
		"andn %[saved], 2, %[scratch]\n\t"	// PSTATE.IE
		"wrpr %[scratch], 0, %%pstate\n\t"

		"stxa %[tsbValue], [%[tsbReg]] 0x58\n\t"
		"membar #Sync\n\t"
		"stxa %[tsbValue], [%[tsbReg]] 0x50\n\t"
		"membar #Sync\n\t"

		"wrpr %%g0, %[table], %%tba\n\t"
		"membar #Sync"
		: [saved] "=&r"(savedPstate), [scratch] "=&r"(scratch)
		: [tsbValue] "r"(tsbRegister), [tsbReg] "r"(tsb),
		  [table] "r"(trapTable)
		: "memory");

	sMmuInstalled = true;

	dprintf("sparc_mmu: the kernel now services its own traps\n");

	sparc_verify_trap_handlers(args);
	return B_OK;
}


/*!	Whether the kernel has taken the MMU and the trap table over.

	Used by the early mapping path to decide whether the firmware still needs to
	be told about a mapping, or whether writing it into the TSB is enough.
*/
bool
sparc_mmu_is_installed()
{
	return sMmuInstalled;
}


/*!	Recurses deeper than the register file has windows.

	NWINDOWS is 8, so a call chain longer than that cannot keep every frame's
	registers in the file and the hardware has to spill the oldest to its stack
	frame -- then fill them back as the chain unwinds. Both directions are our
	handlers now.

	The marker is live across the recursive call, which is what forces it into a
	register this window owns rather than a scratch one, and therefore what makes
	it something a spill has to save and a fill has to restore correctly. A
	handler that saved the wrong register, or used the wrong stack bias, returns
	the wrong sum rather than crashing -- which is exactly the failure that would
	otherwise go unnoticed until something far away misbehaved.
*/
static uint64 __attribute__((noinline))
sparc_probe_window_depth(int depth)
{
	if (depth == 0)
		return 0;

	uint64 marker = 0x5741524bULL * (uint64)depth;
	uint64 deeper = sparc_probe_window_depth(depth - 1);

	return marker + deeper;
}


/*!	Exercises the two things the firmware used to do for us, on purpose.

	Everything up to here has been the handlers working incidentally: the boot
	takes thousands of TLB misses and thousands of window spills, and getting
	this far means most of that worked. But "most of that worked" is not the same
	as knowing either path is correct, and a handler that is subtly wrong -- one
	register short, one displacement off -- shows up as corruption somewhere else
	entirely, long after the evidence has gone.

	So provoke both deliberately, where the answer is known in advance.
*/
static void
sparc_verify_trap_handlers(struct kernel_args *args)
{
	// A fresh page, mapped by us and nobody else: after the cutover early_map
	// writes the TSB and does not call the firmware, so this mapping exists
	// only because our own code put it there.
	const uint64 kPattern = 0xfeedfacecafebeefULL;
	addr_t page = vm_allocate_early(args, B_PAGE_SIZE, B_PAGE_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0);
	if (page == 0) {
		dprintf("sparc_mmu: no page for the provoked-miss test\n");
		return;
	}

	volatile uint64* probe = (volatile uint64*)page;
	*probe = kPattern;

	// Now take the translation out of the TLB but leave it in the TSB. The next
	// read has to miss, trap, and be refilled by the fast path -- there is no
	// other way for it to succeed.
	asm volatile("stxa %%g0, [%[va]] 0x5f\n\t"
		"membar #Sync"
		:
		: [va] "r"((uint64)page & TLB_TAG_VA_MASK)
		: "memory");

	uint64 read = *probe;
	dprintf("sparc_mmu: provoked TLB miss at %#" B_PRIxADDR ": read %#" B_PRIx64
		" -- %s\n", page, read, read == kPattern ? "refilled" : "WRONG");
	if (read != kPattern) {
		panic("sparc_mmu: a provoked TLB miss was not refilled correctly");
		return;
	}

	// 24 frames against 8 windows: several rounds of spills on the way down and
	// fills on the way back.
	const int kDepth = 24;
	uint64 expected = 0;
	for (int i = 1; i <= kDepth; i++)
		expected += 0x5741524bULL * (uint64)i;

	uint64 sum = sparc_probe_window_depth(kDepth);
	dprintf("sparc_mmu: forced window overflow %d deep: got %#" B_PRIx64
		", expected %#" B_PRIx64 " -- %s\n", kDepth, sum, expected,
		sum == expected ? "spilled and filled" : "WRONG");
	if (sum != expected)
		panic("sparc_mmu: window spill/fill lost or corrupted a register");
}


/*!	Records a range as permanently mapped, so nothing invalidates it later. */
static void
sparc_add_locked_range(addr_t base, size_t size)
{
	if (sLockedRangeCount >= sizeof(sLockedRanges) / sizeof(sLockedRanges[0])) {
		panic("sparc_mmu: too many locked ranges");
		return;
	}

	sLockedRanges[sLockedRangeCount].base = base;
	sLockedRanges[sLockedRangeCount].size = size;
	sLockedRangeCount++;
}


/*!	Whether an address belongs to a mapping that must never be taken away. */
static bool
sparc_is_locked_address(addr_t virtualAddress)
{
	for (uint32 i = 0; i < sLockedRangeCount; i++) {
		if (virtualAddress >= sLockedRanges[i].base
			&& virtualAddress < sLockedRanges[i].base + sLockedRanges[i].size) {
			return true;
		}
	}

	return false;
}


/*!	Drops a TSB line if it is the one describing this address in this context.

	Conditional on the tag, because the TSB is direct-mapped: the line this
	address indexes to may currently belong to a different address that collided
	with it, and clearing that would evict a live translation for no reason. Not
	a correctness problem -- it would be refilled from the page table -- but the
	whole point of the TSB is to make that unnecessary.

	The context is part of that comparison and not decoration. One TSB serves
	every address space, so the same user address in two teams indexes to the
	same line, and unmapping it in one of them must not throw away the other's.
*/
void
sparc_tsb_invalidate(addr_t virtualAddress, uint32 context)
{
	if (sKernelTsb == NULL)
		return;

	// See sparc_add_locked_range(): these mappings are permanent, and dropping
	// one costs the machine its ability to handle the next trap.
	if (sparc_is_locked_address(virtualAddress))
		return;

	uint32 index = (virtualAddress >> 13) & (KERNEL_TSB_ENTRIES - 1);
	TsbEntry* entry = &sKernelTsb[index];

	if (entry->fTag != (((uint64)context << TTE_TAG_CONTEXT_SHIFT)
			| (virtualAddress >> TSB_TAG_VA_SHIFT))) {
		return;
	}

	// Data first. A reader that sees a valid tag with cleared data treats the
	// line as invalid and goes to the page table, which is correct; a reader
	// that saw a cleared tag with stale data would compare against a tag of
	// zero and could match a low address.
	entry->fData = 0;
	entry->fTag = 0;
}


/*!	Removes an address's translation from both TLBs.

	Demap-page rather than demap-context: it removes the entry matching this
	virtual page and leaves everything else alone (FIGURE 15-15, printed p.231,
	with type 0 at bit 6 and the primary context selected at bits 5:4).

	Both TLBs unconditionally. Asking which one holds it would cost more than
	demapping the one that does not, and a demap of an address with no entry is
	defined to remove nothing.
*/
void
sparc_tlb_demap(addr_t virtualAddress, uint32 context)
{
	// A demap removes whatever matches the address, locked or not -- the Lock
	// bit only exempts an entry from replacement. Refusing here is what keeps
	// the trap path mapped; the mappings it protects never change, so there is
	// nothing to invalidate anyway.
	if (sparc_is_locked_address(virtualAddress))
		return;

	// A kernel mapping is demapped through the primary context selector, with
	// no register touched at all. Its TTE is Global, so it is matched whatever
	// the context is, and this is the path every demap in the system takes today
	// -- worth leaving exactly as it was rather than routing it through the
	// register borrowing below for the sake of one code path.
	if (context == SPARC_KERNEL_CONTEXT) {
		uint64 address = ((uint64)virtualAddress & TLB_TAG_VA_MASK)
			| TLB_DEMAP_TYPE_PAGE | TLB_DEMAP_CONTEXT_PRIMARY;

		asm volatile(
			"stxa	%%g0, [%[address]] 0x5f\n\t"	// ASI_DMMU_DEMAP
			"membar	#Sync\n\t"
			"stxa	%%g0, [%[address]] 0x57\n\t"	// ASI_IMMU_DEMAP
			"membar	#Sync"
			:
			: [address] "r"(address)
			: "memory");
		return;
	}

	uint64 address = ((uint64)virtualAddress & TLB_TAG_VA_MASK)
		| TLB_DEMAP_TYPE_PAGE | TLB_DEMAP_CONTEXT_SECONDARY;

	// The secondary context register is borrowed rather than the primary one, so
	// that the accesses the interrupted code is in the middle of keep matching
	// what they matched before. It is still per-CPU state though, and an
	// interrupt handler that demapped something in between would restore the
	// wrong value over the top of this one.
	InterruptsLocker locker;

	uint64 wasSecondary;
	asm volatile(
		"ldxa	[%[contextRegister]] 0x58, %[saved]\n\t"
		"stxa	%[context], [%[contextRegister]] 0x58\n\t"
		"membar	#Sync\n\t"
		"stxa	%%g0, [%[address]] 0x5f\n\t"	// ASI_DMMU_DEMAP
		"stxa	%%g0, [%[address]] 0x57\n\t"	// ASI_IMMU_DEMAP
		"membar	#Sync\n\t"
		"stxa	%[saved], [%[contextRegister]] 0x58\n\t"
		"membar	#Sync"
		: [saved] "=&r"(wasSecondary)
		: [contextRegister] "r"(secondary_context), [context] "r"((uint64)context),
		  [address] "r"(address)
		: "memory");
}


/*!	Puts %g3 and %g7 back in the trap global banks after a firmware call.

	Open Firmware uses the operating system's reserved registers for its own
	state, which call_open_firmware() already knows: it saves and restores %g6
	and %g7 around every client call. But that save runs as ordinary C at trap
	level zero, so it saves the *normal* bank -- and the firmware's writes land
	in whichever bank its own PSTATE selects. The MMU and alternate banks are
	separate register files and were left to rot.

	Nothing noticed for four phases because nothing read them. The TLB miss
	handlers keep the trap data pointer in %g7 and the page table root in %g3
	and use neither: the fast path reads MMU registers, the slow path walks
	physical addresses out of %g3, and the paths that do use %g7 -- the fault
	entry and the unhandled-trap reporter -- run on the *alternate* bank. The
	first code to read [%g7 + offset] from the MMU bank found 0xffe80018, an
	address inside the firmware's own image, and treated the word beside it as a
	page table root.

	So re-establish both banks from the values the kernel knows are right, rather
	than saving and restoring three banks' worth around every call. Idempotent,
	two stores per bank, and it cannot drift from what the cutover installed
	because it calls the same function the cutover did.
*/
void
sparc_restore_trap_globals()
{
	if (!sparc_mmu_is_installed())
		return;

	// One-shot: whether the firmware really does clobber the MMU bank, said out
	// loud rather than left to be inferred from a fault three layers away.
	if (!sObservedMmuGlobal) {
		sObservedMmuGlobal = true;
		sMmuGlobalAfterCall = sparc_read_trap_globals(SPARC_GLOBALS_MMU);
	}

	sparc_set_trap_globals(&sTrapData, sparc_kernel_page_table());
}


/*!	Says whether the firmware was found to have clobbered the MMU bank's %g7. */
void
sparc_report_mmu_global()
{
	if (!sObservedMmuGlobal) {
		dprintf("sparc_mmu: no firmware call has been made since the cutover\n");
		return;
	}

	bool clobbered = sMmuGlobalAfterCall != (uint64)(addr_t)&sTrapData;
	dprintf("sparc_mmu: %%g7 in the MMU bank after a firmware call was %#"
		B_PRIx64 ", trap data is at %p -- %s\n", sMmuGlobalAfterCall,
		&sTrapData, clobbered ? "CLOBBERED, and restored each call"
			: "unchanged");
}


/*!	Records this thread's kernel stack, for a trap out of userspace to land on.

	Biased, as SPARC V9 stack pointers are, so that the trap entry can treat it
	the same way it treats the %sp it would otherwise have used.
*/
void
sparc_set_kernel_stack(addr_t stackTop)
{
	sTrapData.kernelStackTop = stackTop - SPARC_STACK_BIAS;
}


/*!	How many user windows have gone to the user's own stack. See the header. */
uint64
sparc_user_spill_count()
{
	return sTrapData.userSpillCount;
}


/*!	Arrivals in the save-area fill handler. See the header. */
uint64
sparc_other_fill_count()
{
	return sTrapData.otherFillCount;
}


/*!	The address a faulted spill or fill wanted. See the header. */
uint64
sparc_winfixup_address()
{
	return sTrapData.winfixupAddress;
}


/*!	And which window trap was abandoned to get it. See the header. */
uint64
sparc_winfixup_trap_type()
{
	return sTrapData.winfixupTrapType;
}


/*!	How many have been abandoned and re-run. See the header. */
uint64
sparc_winfixup_count()
{
	return sTrapData.winfixupCount;
}


/*!	How many user windows the kernel has parked. See the header. */
uint64
sparc_other_spill_count()
{
	return sTrapData.otherSpillCount;
}


/*!	Records what %g7 must be for kernel C code. See the header. */
void
sparc_set_current_thread(void* thread)
{
	sTrapData.currentThread = (uint64)(addr_t)thread;
}


/*!	Points the user window handlers at this thread's save area. */
void
sparc_set_window_save(void* area)
{
	sTrapData.windowSave = (uint64)(addr_t)area;
}


/*!	Points the MMU at a different team's mappings. */
void
sparc_switch_address_space(uint32 context, phys_addr_t pageTableRoot)
{
	// The root first, because it is the one the miss handler reads out of
	// memory. Between the two stores the register still names the outgoing team
	// while the root names the incoming one, and a miss taken in that window
	// would walk the new table and tag the result with the old id. Interrupts
	// are already off here -- the scheduler holds a spinlock across the switch --
	// but the ordering is written down rather than relied on, because the reason
	// it is safe is not local to this function.
	sTrapData.userPageTableRoot = pageTableRoot;

	asm volatile(
		"stxa	%[context], [%[contextRegister]] 0x58\n\t"
		"membar	#Sync"
		:
		: [contextRegister] "r"(primary_context), [context] "r"((uint64)context)
		: "memory");
}


/*!	Removes every trace of a context, so its id can be given to another team. */
void
sparc_context_invalidate(uint32 context)
{
	// The kernel's id is never reassigned, and demapping it wholesale would take
	// the locked trap mappings out from under the next trap.
	if (context == SPARC_KERNEL_CONTEXT)
		return;

	uint64 address = TLB_DEMAP_TYPE_CONTEXT | TLB_DEMAP_CONTEXT_SECONDARY;

	InterruptsLocker locker;

	uint64 wasSecondary;
	asm volatile(
		"ldxa	[%[contextRegister]] 0x58, %[saved]\n\t"
		"stxa	%[context], [%[contextRegister]] 0x58\n\t"
		"membar	#Sync\n\t"
		"stxa	%%g0, [%[address]] 0x5f\n\t"	// ASI_DMMU_DEMAP
		"stxa	%%g0, [%[address]] 0x57\n\t"	// ASI_IMMU_DEMAP
		"membar	#Sync\n\t"
		"stxa	%[saved], [%[contextRegister]] 0x58\n\t"
		"membar	#Sync"
		: [saved] "=&r"(wasSecondary)
		: [contextRegister] "r"(secondary_context), [context] "r"((uint64)context),
		  [address] "r"(address)
		: "memory");

	if (sKernelTsb == NULL)
		return;

	// And the TSB, which has no demap of its own. A linear scan of every line,
	// which is the price of one shared TSB rather than one per address space --
	// 8192 lines, on a path taken once per team rather than once per mapping.
	//
	// Leaving a line behind would not be a slow path. The next team to be given
	// this id forms tag targets with it, matches the line, and reads the page
	// that used to belong to somebody else.
	uint64 wanted = (uint64)context << TTE_TAG_CONTEXT_SHIFT;
	for (uint32 i = 0; i < KERNEL_TSB_ENTRIES; i++) {
		if ((sKernelTsb[i].fTag & ((uint64)TTE_TAG_CONTEXT_MASK
				<< TTE_TAG_CONTEXT_SHIFT)) != wanted) {
			continue;
		}

		// Data first, for the reason sparc_tsb_invalidate() gives.
		sKernelTsb[i].fData = 0;
		sKernelTsb[i].fTag = 0;
	}
}

#endif	// !_BOOT_MODE
