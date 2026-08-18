/*
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <arch_mmu.h>

#include <arch_cpu.h>
#include <debug.h>
#include <platform/openfirmware/openfirmware.h>


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
