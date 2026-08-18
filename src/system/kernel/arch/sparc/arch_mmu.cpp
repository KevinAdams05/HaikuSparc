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
