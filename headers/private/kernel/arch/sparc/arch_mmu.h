/*
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
** Distributed under the terms of the MIT License.
*/
#ifndef _KERNEL_ARCH_SPARC_MMU_H
#define _KERNEL_ARCH_SPARC_MMU_H


#include <SupportDefs.h>
#include <string.h>

#include <arch_cpu.h>


// Translation Table Entry, the sun4u equivalent of a page table entry. Field
// positions are from the UltraSPARC-IIi User's Manual, FIGURE 15-1 (printed
// p.205): a TSB line is two 64-bit words, a tag and a data half.
//
//   tag:   G | -- | Context<60:48> | -- | VA_tag<63:22> at bits 41:0
//   data:  V | Size | NFO | IE | Soft2 | Diag | PA<40:13> | Soft
//                                       | L | CP | CV | E | P | W | G

// TTE tag
#define TTE_TAG_GLOBAL				(1ULL << 63)
#define TTE_TAG_CONTEXT_SHIFT		48
#define TTE_TAG_CONTEXT_MASK		0x1fffULL

// TTE data
#define TTE_VALID					(1ULL << 63)
#define TTE_SIZE_SHIFT				61
#define TTE_SIZE_MASK				0x3ULL
#define TTE_NFO						(1ULL << 60)
#define TTE_INVERT_ENDIANNESS		(1ULL << 59)
#define TTE_PA_MASK					0x000001ffffffe000ULL
	// PA<40:13>. Later CPUs define more bits here.
#define TTE_LOCKED					(1ULL << 6)
	// Exempt from the automatic replacement algorithm. At least one TLB entry
	// must be left unlocked or the last one is replaced regardless.
#define TTE_CACHEABLE_PHYSICAL		(1ULL << 5)
	// CP: cacheable in the physically indexed caches (E-cache, I-cache).
#define TTE_CACHEABLE_VIRTUAL		(1ULL << 4)
	// CV: cacheable in the virtually indexed D-cache. Read as zero in the
	// I-MMU. See TABLE 15-2 for the encoding of the two together.
#define TTE_SIDE_EFFECT				(1ULL << 3)
	// Set for pages mapping I/O with side effects: makes non-cacheable
	// accesses strongly ordered and traps speculative loads.
#define TTE_PRIVILEGED				(1ULL << 2)
#define TTE_WRITABLE				(1ULL << 1)
#define TTE_GLOBAL					(1ULL << 0)
	// Duplicated in tag and data to speed up the miss handler. When set, the
	// context field is ignored during hit detection, so the page is shared by
	// every context.

// TTE_SIZE_* values, from TABLE 15-1.
#define TTE_SIZE_8K					0
#define TTE_SIZE_64K				1
#define TTE_SIZE_512K				2
#define TTE_SIZE_4M					3

// TSB register, FIGURE 15-9 (printed p.227).
#define TSB_BASE_MASK				0xffffffffffffe000ULL
	// TSB_Base<63:13>. Must be aligned to the size of the TSB, or of both
	// TSBs together when split. Out-of-range values are NOT checked by the
	// hardware -- a bad base is accepted silently and fails later.
#define TSB_SPLIT					(1ULL << 12)
#define TSB_SIZE_MASK				0xfULL
	// Entries per TSB = 512 << TSB_Size, so 512 (8 KB) to 65536 (1 MB).

#define TSB_ENTRIES(size)			(512ULL << (size))


struct TsbEntry {
public:
	bool IsValid() const	{ return (fData & TTE_VALID) != 0; }

public:
	uint64_t fTag;
	uint64_t fData;
};


extern void sparc_get_instruction_tsb(TsbEntry **_pageTable, size_t *_size);
extern void sparc_get_data_tsb(TsbEntry **_pageTable, size_t *_size);

extern void sparc_dump_openfirmware_translations();
extern void sparc_verify_tsb_indexing();

// TSB_Size for the kernel's own TSB. 8192 entries of 16 bytes is 128 KB per
// TSB, and the split pair is 256 KB aligned to 256 KB. Sized from the measured
// working set -- see sparc-port/PHASE2_MMU_DESIGN.md section 4.1.
#define KERNEL_TSB_SIZE				4
#define KERNEL_TSB_ENTRIES			TSB_ENTRIES(KERNEL_TSB_SIZE)
#define KERNEL_TSB_BYTES			(KERNEL_TSB_ENTRIES * sizeof(TsbEntry))

struct kernel_args;

extern status_t sparc_mmu_init_tsb(struct kernel_args *args);
extern void sparc_tsb_insert(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint64 flags);


#endif	/* _KERNEL_ARCH_SPARC_MMU_H */
