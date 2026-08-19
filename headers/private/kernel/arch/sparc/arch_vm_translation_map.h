/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_SPARC_VM_TRANSLATION_MAP_H
#define _KERNEL_ARCH_SPARC_VM_TRANSLATION_MAP_H


#include <arch/vm_translation_map.h>

#include <arch_mmu.h>


/*	The authoritative page table.

	The TSB is a cache, not a record: it is direct-mapped, so two live addresses
	can want the same line and one of them loses. Something has to be able to
	answer "what is this address mapped to" for every address, and that is this
	structure.

	Three levels, each one page of 1024 eight-byte entries, indexed by ten bits
	of the virtual address each. Together they cover VA<42:13> -- and 43 bits is
	not an arbitrary stopping point, it is the whole usable address space. sun4u
	implements a 44-bit virtual address with a hole in the middle: VA<63:43> must
	be all zeroes or all ones, and anything else is an out-of-range access rather
	than an unmapped one.

	The geometry is OpenBSD's, deliberately. Its miss handler faces the same
	constraint ours does and the layout is what makes the walk cheap; matching it
	means the walk is a known quantity rather than something to discover. See
	sparc-port/THIRD_PARTY.md.

	Two properties make the assembly walk possible at all:

	**Interior entries hold physical addresses.** A walk that had to dereference
	virtual pointers would need every level mapped, and a miss on one of those
	would be a miss taken inside the miss handler. Physical addressing is
	available from any privilege level through ASI_PHYS_USE_EC, so the walk
	touches no TLB entry and needs nothing locked.

	**Leaf entries are TTE data halves.** Not a private format that has to be
	converted: the value found at the leaf is exactly what gets stored to the
	TLB Data In register. That makes the tail of the slow path the same three
	instructions as the tail of the fast path, and it means a leaf can be
	examined with the same TTE_* accessors as anything else.

	Zero means absent at every level, which is why the tables are page-aligned
	allocations that get zeroed on the way in.
*/

// Bits 12:0 are the page offset; each level then takes ten bits.
#define SPARC_PAGE_TABLE_SHIFT		13
#define SPARC_PAGE_DIRECTORY_SHIFT	(SPARC_PAGE_TABLE_SHIFT + 10)
#define SPARC_SEGMENT_TABLE_SHIFT	(SPARC_PAGE_DIRECTORY_SHIFT + 10)

#define SPARC_PAGE_TABLE_ENTRIES	1024
	// B_PAGE_SIZE / sizeof(uint64). One page per level exactly.
#define SPARC_PAGE_TABLE_MASK		(SPARC_PAGE_TABLE_ENTRIES - 1)

// The virtual address hole. VA<63:43> must be all zeroes or all ones; the
// shift-and-increment test below is how the assembly walk checks it in two
// instructions, and C++ uses the same form so the two cannot disagree.
#define SPARC_VA_HOLE_SHIFT			43

#define SPARC_SEGMENT_INDEX(va) \
	(((va) >> SPARC_SEGMENT_TABLE_SHIFT) & SPARC_PAGE_TABLE_MASK)
#define SPARC_DIRECTORY_INDEX(va) \
	(((va) >> SPARC_PAGE_DIRECTORY_SHIFT) & SPARC_PAGE_TABLE_MASK)
#define SPARC_TABLE_INDEX(va) \
	(((va) >> SPARC_PAGE_TABLE_SHIFT) & SPARC_PAGE_TABLE_MASK)


/*!	True if the address is representable, rather than in the sun4u hole.

	Shifting an arithmetic 64-bit value right by 43 leaves 0 for a low address
	and -1 for a high one; incrementing turns those into 1 and 0, so a single
	unsigned comparison against 1 rejects everything else.
*/
static inline bool
sparc_is_valid_virtual_address(addr_t virtualAddress)
{
	return (uint64)(((int64)virtualAddress >> SPARC_VA_HOLE_SHIFT) + 1) <= 1;
}


/*!	Reads eight bytes of physical memory, bypassing the MMU.

	ASI_PHYS_USE_EC (0x14) is documented in the UltraSPARC-IIi manual's ASI
	table as "Physical address; external cacheable only", section 15.10. It
	needs no translation, so this works with no mapping in place and cannot
	fault on a missing one -- which is what lets the page table live in ordinary
	unlocked memory and still be walkable from a trap handler.
*/
static inline uint64
sparc_read_physical(phys_addr_t physicalAddress)
{
	uint64 value;
	asm volatile("ldxa [%[address]] 0x14, %[value]"
		: [value] "=r"(value)
		: [address] "r"(physicalAddress));
	return value;
}


/*!	Writes eight bytes of physical memory, bypassing the MMU. */
static inline void
sparc_write_physical(phys_addr_t physicalAddress, uint64 value)
{
	asm volatile("stxa %[value], [%[address]] 0x14"
		:
		: [value] "r"(value), [address] "r"(physicalAddress)
		: "memory");
}


struct kernel_args;

// In arch_vm_translation_map.cpp. Records a mapping in the kernel's page table
// during early boot, when no address space object exists yet.
extern status_t sparc_page_table_early_map(kernel_args* args,
	addr_t virtualAddress, phys_addr_t physicalAddress, uint64 flags);

// The kernel's page table root, physical. Zero before it is built.
extern phys_addr_t sparc_kernel_page_table();

struct SPARCPageTableAllocator;

// In SPARCVMTranslationMap.cpp. Declared here as well so code that only needs
// to read the table does not have to pull in the whole class.
extern phys_addr_t sparc_page_table_lookup(phys_addr_t root,
	addr_t virtualAddress, const SPARCPageTableAllocator* allocator);


#endif	/* _KERNEL_ARCH_SPARC_VM_TRANSLATION_MAP_H */
