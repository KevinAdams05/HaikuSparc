/*
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk. All rights reserved.
** Distributed under the terms of the MIT License.
*/


#include <arch_mmu.h>

#include <stddef.h>

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
	sparc_verify_tlb_load(args);
	sparc_verify_tsb_lookup();
	sparc_verify_trap_table();
	sparc_verify_trap_globals();

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

#endif	// !_BOOT_MODE


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
		{ 0x0a0, "spill (other)",        0xffffffff, 0xe073a7ff },
		{ 0x0c0, "fill (normal)",        0xffffffff, 0xe05ba7ff },
		{ 0x0e0, "fill (other)",         0xffffffff, 0xe05ba7ff },
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
		offsetof(sparc_trap_data, tsbMask),
		offsetof(sparc_trap_data, missCount),
		offsetof(sparc_trap_data, missTagTarget),
		offsetof(sparc_trap_data, missTsbPointer),
		offsetof(sparc_trap_data, missTsbTag),
		offsetof(sparc_trap_data, missTsbData),
		offsetof(sparc_trap_data, missTrapType),
		offsetof(sparc_trap_data, missTpc),
		offsetof(sparc_trap_data, missTstate),
		offsetof(sparc_trap_data, missTl),
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

	sparc_set_trap_globals(&sTrapData);

	uint64 normalAfter;
	asm volatile("mov %%g7, %0" : "=r"(normalAfter));
	uint64 mmuGlobal = sparc_read_trap_globals(SPARC_GLOBALS_MMU);
	uint64 alternateGlobal = sparc_read_trap_globals(SPARC_GLOBALS_ALTERNATE);

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

	dprintf("sparc_mmu: trap globals verified in both banks\n");
	return B_OK;
}
