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

// A tag holds VA<63:22> at bits 41:0, so virtual address bit n sits at tag bit
// n - TSB_TAG_VA_SHIFT.
#define TSB_TAG_VA_SHIFT			22

// Which bit of a tag decides whether the context field means anything.
//
// The kernel occupies everything from KERNEL_BASE upwards and KERNEL_BASE is a
// power of two, so one bit separates the halves: set means a kernel address,
// whose mappings are Global and whose tag is stored with context zero; clear
// means a user address, whose tag carries its team's context. That lets the TLB
// miss handler decide whether to compare the context without touching memory --
// see TLB_MISS_HANDLER in arch_traps.S.
//
// KERNEL_BASE is 0x80000000 on this port, so this is bit 31 of the address and
// bit 9 of the tag. Spelled as a literal because arch_kernel.h is not available
// here, and checked against KERNEL_BASE at init by sparc_verify_mmu_defines().
#define TSB_TAG_KERNEL_BIT			9

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

// The Soft field, TTE data bits 12:7, is ignored by the hardware and available
// to software. sun4u has no accessed or modified bit of its own -- the hardware
// records neither -- so the ones the VM needs live here and are maintained by
// the miss and protection handlers.
//
// The positions are OpenBSD's, from sys/arch/sparc64/include/pte.h. They are
// arbitrary in themselves, and matching a working implementation costs nothing
// and makes the handlers comparable against a reference. See
// sparc-port/THIRD_PARTY.md.
#define TTE_SOFT_MASK				0x0000000000001f80ULL
#define TTE_SOFT_EXECUTE			(1ULL << 8)
#define TTE_SOFT_ACCESSED			(1ULL << 9)
#define TTE_SOFT_REAL_WRITABLE		(1ULL << 10)
	// What the mapping's protection actually permits, as opposed to what the
	// hardware W bit currently says. The two differ while a writable page is
	// mapped read-only in order to catch the first write and set MODIFIED.
#define TTE_SOFT_MODIFIED			(1ULL << 11)

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
extern status_t sparc_verify_trap_table();
extern void sparc_dump_tlb();
extern status_t sparc_lock_trap_pages();
extern status_t sparc_install_trap_table(struct kernel_args *args);
extern bool sparc_mmu_is_installed();
extern void sparc_tsb_invalidate(addr_t virtualAddress);
extern void sparc_tlb_demap(addr_t virtualAddress);

// TLB geometry. Both TLBs on the UltraSPARC-IIi are 64-entry fully associative,
// and an entry is selected by putting its index in VA<8:3> of the address given
// to the Data Access or Tag Read ASI -- FIGURE 15-13, printed p.230.
#define SPARC_TLB_ENTRIES			64
#define SPARC_TLB_ENTRY_ADDRESS(entry)	((entry) << 3)

// TLB Tag Read register, FIGURE 15-14 (printed p.230). Note the VA field is
// sign-extended from VA<43>, and that page-offset bits for pages larger than
// 8 KB are stored and read back here even though translation ignores them.
#define TLB_TAG_VA_MASK				0xffffffffffffe000ULL
#define TLB_TAG_CONTEXT_MASK		0x1fffULL

// TSB_Size for the kernel's own TSB. 8192 entries of 16 bytes is 128 KB per
// TSB, and the split pair is 256 KB aligned to 256 KB. Sized from the measured
// working set -- see sparc-port/PHASE2_MMU_DESIGN.md section 4.1.
#define KERNEL_TSB_SIZE				4
#define KERNEL_TSB_ENTRIES			TSB_ENTRIES(KERNEL_TSB_SIZE)
#define KERNEL_TSB_BYTES			(KERNEL_TSB_ENTRIES * sizeof(TsbEntry))

// Per-CPU data the trap handlers reach through %g7.
//
// A TLB miss handler runs with no stack, and it cannot afford to build an
// address either: the kernel is linked as a shared object, so a sethi/or of a
// symbol would need a load-time relocation, and the GOT is only reachable
// through %l7 -- a register belonging to the interrupted window, which the
// handler must not touch. So the address is put in a register before any trap
// can happen, and the register is one the hardware hands the handler for free.
//
// Section 15.3.2 of the UltraSPARC-IIi manual gives the MMU miss traps their
// own bank of globals, and most other traps the alternate bank. Both banks get
// %g7 pointing here, so a handler in either can find this block with a single
// displaced load and nothing else.
//
// The offsets are duplicated in arch_traps.S, where the assembler cannot see
// this declaration. sparc_verify_trap_globals() checks the two agree.
struct sparc_trap_data {
	uint64	tsbBase;			// 0x00  physical base of the split TSB pair
	uint64	pageTableRoot;		// 0x08  physical root, as also held in %g3
	uint64	missCount;			// 0x10  unresolved misses taken since boot
	uint64	missTagTarget;		// 0x18  context and VA<63:22>, as compared
	uint64	missTsbPointer;		// 0x20  the TSB line the hardware chose
	uint64	missTagAccess;		// 0x28  VA<63:13> and context, from the MMU
	uint64	missEntry;			// 0x30  the leaf the walk reached, if any
	uint64	missTrapType;		// 0x38
	uint64	missTpc;			// 0x40
	uint64	missTstate;			// 0x48
	uint64	missTl;				// 0x50
	uint64	reportHandler;		// 0x58  where a failed trap returns to report
	uint64	trapKind;			// 0x60  which failure filled the fields above
	uint64	trapCallSite;		// 0x68  %o7 -- the trapped window's call site
	uint64	trapReturnAddress;	// 0x70  %i7 -- and where that frame returns to
	uint64	trapWindowState;	// 0x78  CWP, CANSAVE, CANRESTORE, CLEANWIN packed
	uint64	trapLocals[8];		// 0x80  %l0-%l7 of the window that trapped
	uint64	reserved[8];		// pad to 256 bytes
};

#define TRAP_DATA_TSB_BASE			0x00
#define TRAP_DATA_PAGE_TABLE_ROOT	0x08
#define TRAP_DATA_MISS_COUNT		0x10
#define TRAP_DATA_MISS_TAG_TARGET	0x18
#define TRAP_DATA_MISS_TSB_POINTER	0x20
#define TRAP_DATA_MISS_TAG_ACCESS	0x28
#define TRAP_DATA_MISS_ENTRY		0x30
#define TRAP_DATA_MISS_TRAP_TYPE	0x38
#define TRAP_DATA_MISS_TPC			0x40
#define TRAP_DATA_MISS_TSTATE		0x48
#define TRAP_DATA_MISS_TL			0x50
#define TRAP_DATA_REPORT_HANDLER	0x58
#define TRAP_DATA_TRAP_KIND			0x60
#define TRAP_DATA_TRAP_CALL_SITE	0x68
#define TRAP_DATA_TRAP_RETURN		0x70

// Values for sparc_trap_data::trapKind.
#define SPARC_TRAP_UNRESOLVED_MISS	0
#define SPARC_TRAP_UNHANDLED		1
#define TRAP_DATA_SIZE				0x100

// Which global bank sparc_read_trap_globals() should be asked about.
#define SPARC_GLOBALS_MMU			0
#define SPARC_GLOBALS_ALTERNATE		1

struct kernel_args;

extern status_t sparc_mmu_init_tsb(struct kernel_args *args);

/*	Claims the TSB's virtual range from the VM, once there is a VM to claim it
	from. Must be called from arch_vm_translation_map_init_post_area(), while the
	boot loader's ranges are still reserved -- vm_init() drops those immediately
	afterwards, and anything the architecture has not turned into an area of its
	own becomes free address space at that moment.
*/
extern status_t sparc_mmu_create_tsb_area();
extern void sparc_tsb_insert(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint64 flags);
extern status_t sparc_verify_trap_globals();

// Both in arch_traps.S.
extern "C" void sparc_set_trap_globals(struct sparc_trap_data *data,
	phys_addr_t pageTableRoot);
extern "C" uint64 sparc_read_trap_globals(int bank);
extern "C" uint64 sparc_read_trap_page_table(int bank);
extern "C" void sparc_trap_data_offsets(uint64 *out);

#define TRAP_DATA_OFFSET_COUNT		17


#endif	/* _KERNEL_ARCH_SPARC_MMU_H */
