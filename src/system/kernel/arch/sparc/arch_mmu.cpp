/*
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <arch_mmu.h>

#include <arch_cpu.h>
#include <debug.h>
#include <platform/openfirmware/openfirmware.h>
#include <vm/vm.h>


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

	// Kernel mappings are Global, so hit detection ignores the context field
	// and the entry serves every context. See PHASE2_MMU_DESIGN.md section 4
	// and the plan's section 4.3 for why the port stays in one address space.
	entry->fTag = TTE_TAG_GLOBAL | (virtualAddress >> 22);
	entry->fData = TTE_VALID | ((uint64)TTE_SIZE_8K << TTE_SIZE_SHIFT)
		| (physicalAddress & TTE_PA_MASK) | TTE_GLOBAL | flags;
}


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
	addr_t base = vm_allocate_early(args, size, size,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, size);
	if (base == 0) {
		dprintf("sparc_mmu: could not allocate a %" B_PRIuSIZE " byte TSB\n",
			size);
		return B_NO_MEMORY;
	}

	sKernelTsb = (TsbEntry*)base;
	memset(sKernelTsb, 0, size);

	dprintf("sparc_mmu: TSB at %#" B_PRIxADDR ", %" B_PRIu32 " entries per half"
		", %" B_PRIuSIZE " KB total\n", base, (uint32)KERNEL_TSB_ENTRIES,
		size / 1024);

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
			sparc_tsb_insert(va + offset, pa + offset,
				data & (TTE_WRITABLE | TTE_PRIVILEGED | TTE_LOCKED
					| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL
					| TTE_SIDE_EFFECT));
			inserted++;
		}
	}

	dprintf("sparc_mmu: warmed with %" B_PRIu32 " firmware pages, %" B_PRIu32
		" collisions (%" B_PRIu32 " distinct entries live)\n", inserted,
		sKernelTsbCollisions, inserted - sKernelTsbCollisions);

	sparc_verify_tsb_indexing();

	return B_OK;
}


/*!	Checks that our index arithmetic agrees with the hardware's.

	This is the one property the whole fast path rests on: the miss handler
	will take a pointer straight out of ASI_DMMU_TSB_8KB_PTR and expect to find
	there whatever software put in. If the two disagree by so much as a bit,
	every lookup misses, the slow path runs for everything, and the symptom is
	a mysteriously slow machine rather than an obvious fault.

	Rather than re-derive the formula and compare it with itself, use the
	firmware's TSB as an oracle. Open Firmware's miss handler uses the same
	hardware pointer, so its TSB is laid out the way the hardware expects.
	Index into it with our arithmetic and see whether the tags land where they
	should.
*/
void
sparc_verify_tsb_indexing()
{
	uint64_t rawInstruction;
	uint64_t rawData;
	asm("ldxa [%[reg]] 0x50, %[dest]"
		: [dest] "=r"(rawInstruction) : [reg] "r"(tsb));
	asm("ldxa [%[reg]] 0x58, %[dest]"
		: [dest] "=r"(rawData) : [reg] "r"(tsb));
	dprintf("sparc_mmu: firmware TSB registers: I %#018llx  D %#018llx\n",
		(unsigned long long)rawInstruction, (unsigned long long)rawData);

	TsbEntry* firmwareTsb;
	size_t size;
	sparc_get_data_tsb(&firmwareTsb, &size);

	if (rawData == 0 || firmwareTsb == NULL || size == 0) {
		// Not a failure, and worth knowing: it means the firmware services its
		// own TLB misses without the hardware TSB mechanism at all, walking
		// its translation list in software instead. The practical consequence
		// is good -- the TSB registers are ours to program without disturbing
		// anything the firmware depends on.
		dprintf("sparc_mmu: firmware does not use a hardware TSB; the TSB "
			"registers are free\n");
		return;
	}

	uint32 entries = size / sizeof(TsbEntry);
	dprintf("sparc_mmu: firmware data TSB at %p, %" B_PRIu32 " entries\n",
		firmwareTsb, entries);

	// Walk the firmware's TSB and confirm that every valid entry is stored at
	// the index our arithmetic computes from the virtual address its own tag
	// encodes. The tag holds VA<63:22>, so it only pins the address down to a
	// 4 MB region -- enough to catch a wrong shift or mask, which is what this
	// is guarding against.
	uint32 checked = 0;
	uint32 mismatched = 0;
	for (uint32 i = 0; i < entries; i++) {
		if (!firmwareTsb[i].IsValid())
			continue;

		addr_t va = (firmwareTsb[i].fTag & 0x3ffffffffffULL) << 22;
		uint32 expected = (va >> 13) & (entries - 1);

		checked++;
		// Only the bits above 21 survive in the tag, so compare the part of
		// the index those bits determine.
		if ((expected & ~0x1ffULL) != (i & ~0x1ffULL))
			mismatched++;
	}

	dprintf("sparc_mmu: index check: %" B_PRIu32 " valid entries, %" B_PRIu32
		" disagree with our arithmetic\n", checked, mismatched);
}
