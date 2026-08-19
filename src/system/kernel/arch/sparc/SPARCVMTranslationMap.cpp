/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


#include "SPARCVMTranslationMap.h"

#include <string.h>

#include <util/AutoLock.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>

#include <arch/vm.h>
#include <boot/elf.h>

#include <stddef.h>


// PAGE_SHIFT and B_PAGE_SIZE describe the same number and the kernel uses them
// interchangeably -- vm_page.cpp shifts a page number by one, this file
// multiplies by the other. They disagreed on sparc for as long as the port has
// existed, and nothing said so; see the note in arch_vm.h for what that cost.
static_assert((1UL << PAGE_SHIFT) == B_PAGE_SIZE,
	"PAGE_SHIFT and B_PAGE_SIZE disagree");

// The kernel image structures the loader hands over contain sub-structures that
// are not themselves packed -- Elf64_Ehdr above all -- so the compiler assumes
// their members are naturally aligned and emits full-width loads for them. That
// only holds if the enclosing structure ends on an eight-byte boundary, which is
// not something the header can be relied upon to keep true by accident.
// Asserted on the base's size rather than with offsetof(), which the compiler
// refuses on a type with a base class.
static_assert(sizeof(preloaded_image) % 8 == 0,
	"preloaded_image must be a multiple of 8 bytes, or the ELF header in every "
	"image derived from it is misaligned");


//#define TRACE_SPARC_VM_TRANSLATION_MAP
#ifdef TRACE_SPARC_VM_TRANSLATION_MAP
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


/*!	Turns Haiku's area protection flags into the TTE bits that express them.

	Two things here are not a direct translation.

	**Modified tracking is deferred, and writable means writable.** The hardware
	records nothing about whether a page has been written. The way to find out is
	to be asked: map the page without the W bit, let the first write take a
	fast_data_access_protection trap, then set MODIFIED and grant W. That needs a
	protection handler, and there is not one yet -- a write to a read-only page
	would reach the unhandled-trap handler and stop the machine.

	So for now W is set whenever the protection allows writing, and MODIFIED is
	set with it. That is conservative in the safe direction: the VM sees every
	writable page as dirty, which costs writebacks of pages that were never
	written, and never the other way round. REAL_WRITABLE is still recorded, so
	when the protection handler arrives the only change here is to stop setting
	the two bits up front.

	**Execute permission is recorded but not enforced.** sun4u has no per-page
	execute bit; it has two separate TLBs, and a page is executable exactly when
	it has an entry in the instruction TLB. Enforcing it therefore means deciding
	which TLB to refill from the miss handler, which is a property of the trap
	type rather than of the TTE. The bit is kept so that decision has something
	to consult.
*/
static uint64
tte_flags_for_attributes(uint32 attributes, bool kernel)
{
	uint64 flags = TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL;

	bool writable;
	bool executable;

	if ((attributes & B_USER_PROTECTION) != 0) {
		writable = (attributes & B_WRITE_AREA) != 0;
		executable = (attributes & B_EXECUTE_AREA) != 0;
	} else {
		flags |= TTE_PRIVILEGED;
		writable = (attributes & B_KERNEL_WRITE_AREA) != 0;
		executable = (attributes & B_KERNEL_EXECUTE_AREA) != 0;
	}

	if (writable) {
		flags |= TTE_SOFT_REAL_WRITABLE | TTE_WRITABLE | TTE_SOFT_MODIFIED;
			// See above: TTE_WRITABLE and TTE_SOFT_MODIFIED go in now only
			// because there is no protection handler to set them later.
	}
	if (executable)
		flags |= TTE_SOFT_EXECUTE;

	// Kernel mappings are Global, so the context field is ignored when the
	// hardware matches them and they need no invalidation on a context switch.
	// This is what buys a shared address space without touching IS_USER_ADDRESS
	// -- see section 4.3 of the porting plan.
	if (kernel)
		flags |= TTE_GLOBAL;

	return flags;
}


/*!	The reverse, for Query(). */
static uint32
attributes_for_tte(uint64 tte)
{
	uint32 attributes = 0;

	if ((tte & TTE_VALID) != 0)
		attributes |= PAGE_PRESENT;
	if ((tte & TTE_SOFT_ACCESSED) != 0)
		attributes |= PAGE_ACCESSED;
	if ((tte & TTE_SOFT_MODIFIED) != 0)
		attributes |= PAGE_MODIFIED;

	// REAL_WRITABLE, not W: a writable page sitting read-only so its first
	// write can be caught is still a writable page as far as the caller is
	// concerned, and reporting otherwise would make Protect() look like it had
	// failed.
	if ((tte & TTE_PRIVILEGED) != 0) {
		attributes |= B_KERNEL_READ_AREA;
		if ((tte & TTE_SOFT_REAL_WRITABLE) != 0)
			attributes |= B_KERNEL_WRITE_AREA;
		if ((tte & TTE_SOFT_EXECUTE) != 0)
			attributes |= B_KERNEL_EXECUTE_AREA;
	} else {
		attributes |= B_READ_AREA;
		if ((tte & TTE_SOFT_REAL_WRITABLE) != 0)
			attributes |= B_WRITE_AREA;
		if ((tte & TTE_SOFT_EXECUTE) != 0)
			attributes |= B_EXECUTE_AREA;
	}

	return attributes;
}


/*!	Obtains a zeroed page to be a page table level.

	Zeroed matters more than usual: zero is what "absent" means at every level of
	this table, so an uncleared page would present itself as a full set of
	garbage translations, and the first thing to walk into it would be handed a
	physical address chosen at random. The early allocator does not clear, so
	that is done here through the physical accessors -- which also means the page
	never has to be mapped in order to be prepared.
*/
phys_addr_t
SPARCPageTableAllocator::Allocate() const
{
	phys_addr_t address;

	if (earlyArgs != NULL) {
		page_num_t page = vm_allocate_early_physical_page(earlyArgs);
		if (page == 0)
			return 0;
		address = (phys_addr_t)page * B_PAGE_SIZE;
	} else if (reservation != NULL) {
		vm_page* page = vm_page_allocate_page(reservation, PAGE_STATE_WIRED);
		if (page == NULL)
			return 0;
		DEBUG_PAGE_ACCESS_END(page);
		address = (phys_addr_t)page->physical_page_number * B_PAGE_SIZE;
	} else {
		return 0;
	}

	for (size_t offset = 0; offset < B_PAGE_SIZE; offset += sizeof(uint64))
		sparc_write_physical(address + offset, 0);

	return address;
}


// #pragma mark - SPARCVMTranslationMap


SPARCVMTranslationMap::SPARCVMTranslationMap(bool kernel, phys_addr_t pageTable)
	:
	fIsKernel(kernel),
	fPageTable(pageTable)
{
	recursive_lock_init(&fLock, kernel ? "sparc kernel translation map"
		: "sparc translation map");
}


SPARCVMTranslationMap::~SPARCVMTranslationMap()
{
	recursive_lock_destroy(&fLock);

	// Freeing a user page table's pages belongs here, and is not written yet
	// because nothing creates one: userland does not run. Leaving it unwritten
	// is a leak the moment it does, so it is called out rather than left to be
	// discovered.
	if (!fIsKernel && fPageTable != 0)
		panic("SPARCVMTranslationMap: user page table teardown not implemented");
}


bool
SPARCVMTranslationMap::Lock()
{
	recursive_lock_lock(&fLock);
	return true;
}


void
SPARCVMTranslationMap::Unlock()
{
	if (recursive_lock_get_recursion(&fLock) == 1) {
		// About to be unlocked for real, so anything queued has to take effect
		// before another thread can observe the map as consistent.
		Flush();
	}

	recursive_lock_unlock(&fLock);
}


addr_t
SPARCVMTranslationMap::MappedSize() const
{
	return fMapCount;
}


size_t
SPARCVMTranslationMap::MaxPagesNeededToMap(addr_t start, addr_t end) const
{
	// One page per level of table that the range can straddle, for the two
	// interior levels and the leaf. A range starting at zero is a size rather
	// than an address -- the caller does not yet know where it will land -- so
	// it is charged for the worst placement.
	const size_t kLeafRange = (size_t)B_PAGE_SIZE * SPARC_PAGE_TABLE_ENTRIES;
	const size_t kDirectoryRange = kLeafRange * SPARC_PAGE_TABLE_ENTRIES;
	const size_t kSegmentRange = kDirectoryRange * SPARC_PAGE_TABLE_ENTRIES;

	if (start == 0) {
		start = kSegmentRange - B_PAGE_SIZE;
		end += start;
	}

	size_t segments = end / kSegmentRange + 1 - start / kSegmentRange;
	size_t directories = end / kDirectoryRange + 1 - start / kDirectoryRange;
	size_t leaves = end / kLeafRange + 1 - start / kLeafRange;

	return segments + directories + leaves;
}


/*!	Walks a page table to the leaf entry for an address.

	Physical addresses throughout, not pointers, because that is the form the
	assembly walk in the TLB miss handler uses and the two must agree about the
	layout. Working in the same terms here means a change to the geometry cannot
	be made in one place and forgotten in the other.
*/
phys_addr_t
sparc_page_table_lookup(phys_addr_t root, addr_t virtualAddress,
	const SPARCPageTableAllocator* allocator)
{
	if (root == 0)
		return 0;

	// An address in the sun4u hole has no page table entry to find: it is out of
	// range rather than unmapped, and the hardware faults differently for it.
	if (!sparc_is_valid_virtual_address(virtualAddress))
		return 0;

	const uint32 kInteriorShifts[] = {
		SPARC_SEGMENT_TABLE_SHIFT,
		SPARC_PAGE_DIRECTORY_SHIFT
	};

	phys_addr_t table = root;

	for (size_t level = 0;
			level < sizeof(kInteriorShifts) / sizeof(kInteriorShifts[0]);
			level++) {
		uint32 index = (virtualAddress >> kInteriorShifts[level])
			& SPARC_PAGE_TABLE_MASK;
		phys_addr_t slot = table + (phys_addr_t)index * sizeof(uint64);

		phys_addr_t next = sparc_read_physical(slot);
		if (next == 0) {
			if (allocator == NULL)
				return 0;

			next = allocator->Allocate();
			if (next == 0)
				return 0;

			sparc_write_physical(slot, next);
		}

		table = next;
	}

	return table + (phys_addr_t)SPARC_TABLE_INDEX(virtualAddress)
		* sizeof(uint64);
}


phys_addr_t
SPARCVMTranslationMap::LookupEntry(addr_t virtualAddress,
	const SPARCPageTableAllocator* allocator)
{
	return sparc_page_table_lookup(fPageTable, virtualAddress, allocator);
}


/*!	Drops an address from the two caches in front of the page table.

	Order is deliberate. The TSB goes first: while it still holds the old line, a
	TLB miss would refill from it and undo the demap that followed. Doing it the
	other way round leaves a window in which the TLB is clean, the TSB is stale,
	and any access reinstates the entry we were trying to remove.
*/
void
SPARCVMTranslationMap::InvalidateCaches(addr_t virtualAddress)
{
	sparc_tsb_invalidate(virtualAddress);
	sparc_tlb_demap(virtualAddress);
}


status_t
SPARCVMTranslationMap::Map(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint32 attributes, uint32 memoryType, vm_page_reservation* reservation)
{
	TRACE("SPARCVMTranslationMap::Map(%#" B_PRIxADDR ", %#" B_PRIxPHYSADDR
		")\n", virtualAddress, physicalAddress);

	RecursiveLocker locker(fLock);

	SPARCPageTableAllocator allocator = { NULL, reservation };
	phys_addr_t entry = LookupEntry(virtualAddress, &allocator);
	if (entry == 0)
		return B_NO_MEMORY;

	uint64 tte = TTE_VALID | ((uint64)TTE_SIZE_8K << TTE_SIZE_SHIFT)
		| (physicalAddress & TTE_PA_MASK)
		| tte_flags_for_attributes(attributes, fIsKernel);

	sparc_write_physical(entry, tte);

	// The caches may hold something for this address already -- Map() is also
	// how a mapping gets replaced -- and a stale line here outlives the entry it
	// described.
	InvalidateCaches(virtualAddress);

	fMapCount++;
	return B_OK;
}


status_t
SPARCVMTranslationMap::Unmap(addr_t start, addr_t end)
{
	TRACE("SPARCVMTranslationMap::Unmap(%#" B_PRIxADDR ", %#" B_PRIxADDR ")\n",
		start, end);

	start = ROUNDDOWN(start, B_PAGE_SIZE);
	if (start > end)
		return B_OK;

	RecursiveLocker locker(fLock);

	for (addr_t address = start; address < end; address += B_PAGE_SIZE) {
		phys_addr_t entry = LookupEntry(address, NULL);
		if (entry == 0)
			continue;

		if (sparc_read_physical(entry) == 0)
			continue;

		sparc_write_physical(entry, 0);
		InvalidateCaches(address);
		fMapCount--;
	}

	return B_OK;
}


status_t
SPARCVMTranslationMap::UnmapPage(VMArea* area, addr_t address,
	bool updatePageQueue, bool deletingAddressSpace, uint32* _flags)
{
	ASSERT(address % B_PAGE_SIZE == 0);
	ASSERT(_flags == NULL || !updatePageQueue);

	RecursiveLocker locker(fLock);

	phys_addr_t entry = LookupEntry(address, NULL);
	if (entry == 0)
		return B_ENTRY_NOT_FOUND;

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0)
		return B_ENTRY_NOT_FOUND;

	sparc_write_physical(entry, 0);
	InvalidateCaches(address);
	fMapCount--;

	if (_flags != NULL) {
		*_flags = attributes_for_tte(tte);
		return B_OK;
	}

	// Hand the page itself back to the VM, along with what the mapping recorded
	// about it. Everything below wants the map unlocked, since it takes the
	// page's cache and the page queues.
	locker.Unlock();

	if (area->cache_type == CACHE_TYPE_DEVICE)
		return B_OK;

	vm_page* page = vm_lookup_page((tte & TTE_PA_MASK) / B_PAGE_SIZE);
	ASSERT(page != NULL);

	DEBUG_PAGE_ACCESS_START(page);
	PageUnmapped(area, (tte & TTE_PA_MASK) / B_PAGE_SIZE,
		(tte & TTE_SOFT_ACCESSED) != 0, (tte & TTE_SOFT_MODIFIED) != 0,
		updatePageQueue);
	DEBUG_PAGE_ACCESS_END(page);

	return B_OK;
}


status_t
SPARCVMTranslationMap::Query(addr_t virtualAddress,
	phys_addr_t* _physicalAddress, uint32* _flags)
{
	*_physicalAddress = 0;
	*_flags = 0;

	RecursiveLocker locker(fLock);

	phys_addr_t entry = LookupEntry(virtualAddress, NULL);
	if (entry == 0)
		return B_OK;

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0)
		return B_OK;

	*_physicalAddress = tte & TTE_PA_MASK;
	*_flags = attributes_for_tte(tte);

	return B_OK;
}


status_t
SPARCVMTranslationMap::QueryInterrupt(addr_t virtualAddress,
	phys_addr_t* _physicalAddress, uint32* _flags)
{
	// The walk uses physical accesses, which cannot fault and cannot block, so
	// there is nothing about it that needs the lock or a mapped page. That is
	// exactly what makes it usable from interrupt context, and it is the same
	// property the assembly miss handler relies on.
	*_physicalAddress = 0;
	*_flags = 0;

	phys_addr_t entry = LookupEntry(virtualAddress, NULL);
	if (entry == 0)
		return B_OK;

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0)
		return B_OK;

	*_physicalAddress = tte & TTE_PA_MASK;
	*_flags = attributes_for_tte(tte);

	return B_OK;
}


status_t
SPARCVMTranslationMap::Protect(addr_t base, addr_t top, uint32 attributes,
	uint32 memoryType)
{
	base = ROUNDDOWN(base, B_PAGE_SIZE);
	if (base > top)
		return B_OK;

	TRACE("SPARCVMTranslationMap::Protect(%#" B_PRIxADDR ", %#" B_PRIxADDR
		")\n", base, top);

	uint64 flags = tte_flags_for_attributes(attributes, fIsKernel);

	RecursiveLocker locker(fLock);

	for (addr_t address = base; address < top; address += B_PAGE_SIZE) {
		phys_addr_t entry = LookupEntry(address, NULL);
		if (entry == 0)
			continue;

		uint64 tte = sparc_read_physical(entry);
		if ((tte & TTE_VALID) == 0)
			continue;

		// Keep the size, the physical address, and what has been observed about
		// the page; replace what the protection decides. Losing ACCESSED or
		// MODIFIED here would tell the VM a written page was clean.
		uint64 preserved = tte
			& (TTE_VALID | ((uint64)TTE_SIZE_MASK << TTE_SIZE_SHIFT)
				| TTE_PA_MASK | TTE_SOFT_ACCESSED | TTE_SOFT_MODIFIED);

		sparc_write_physical(entry, preserved | flags);
		InvalidateCaches(address);
	}

	return B_OK;
}


status_t
SPARCVMTranslationMap::ClearFlags(addr_t virtualAddress, uint32 flags)
{
	RecursiveLocker locker(fLock);

	phys_addr_t entry = LookupEntry(virtualAddress, NULL);
	if (entry == 0)
		return B_OK;

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0)
		return B_OK;

	uint64 cleared = tte;
	if ((flags & PAGE_ACCESSED) != 0)
		cleared &= ~TTE_SOFT_ACCESSED;
	// PAGE_MODIFIED is deliberately not cleared. Clearing it would mean arming
	// the protection trap that sets it again, by taking the hardware W bit off,
	// and nothing handles that trap yet -- the next write would stop the machine
	// rather than be recorded. Leaving the page marked modified costs a writeback
	// that was not needed; the alternative loses the machine.

	if (cleared == tte)
		return B_OK;

	sparc_write_physical(entry, cleared);
	InvalidateCaches(virtualAddress);

	return B_OK;
}


bool
SPARCVMTranslationMap::ClearAccessedAndModified(VMArea* area, addr_t address,
	bool unmapIfUnaccessed, bool& _modified)
{
	ASSERT(address % B_PAGE_SIZE == 0);

	RecursiveLocker locker(fLock);

	phys_addr_t entry = LookupEntry(address, NULL);
	if (entry == 0) {
		_modified = false;
		return false;
	}

	uint64 tte = sparc_read_physical(entry);
	if ((tte & TTE_VALID) == 0) {
		_modified = false;
		return false;
	}

	bool accessed = (tte & TTE_SOFT_ACCESSED) != 0;
	_modified = (tte & TTE_SOFT_MODIFIED) != 0;

	if (!accessed && unmapIfUnaccessed) {
		// The caller asked us to reclaim the mapping if it has not been touched
		// since the last check, which is how the page daemon ages pages.
		sparc_write_physical(entry, 0);
		InvalidateCaches(address);
		fMapCount--;

		locker.Unlock();
		UnaccessedPageUnmapped(area, (tte & TTE_PA_MASK) / B_PAGE_SIZE);
		return false;
	}

	// Only accessed is re-armed, and that one is free: the miss handler sets it
	// on the next refill. Modified would need the hardware W bit taken off so the
	// next write traps, and nothing handles that trap yet.
	uint64 rearmed = tte & ~TTE_SOFT_ACCESSED;
	if (rearmed != tte) {
		sparc_write_physical(entry, rearmed);
		InvalidateCaches(address);
	}

	return accessed;
}


void
SPARCVMTranslationMap::Flush()
{
	// Nothing is deferred. Every method here invalidates the TSB line and demaps
	// the TLB entry as it changes the table, because a single CPU with no queued
	// invalidations has nothing to gain by waiting and something to lose: an
	// interrupt handler that ran in the window would see the stale entry.
	//
	// The batching architectures do -- collect addresses, flush once, and on SMP
	// send the list to the other CPUs -- becomes worth having when there are
	// other CPUs to send it to.
}


void
SPARCVMTranslationMap::DebugPrintMappingInfo(addr_t virtualAddress)
{
	if (fPageTable == 0) {
		kprintf("no page table\n");
		return;
	}

	if (!sparc_is_valid_virtual_address(virtualAddress)) {
		kprintf("%#" B_PRIxADDR " is in the sun4u address hole\n",
			virtualAddress);
		return;
	}

	kprintf("page table root %#" B_PRIxPHYSADDR "\n", fPageTable);

	// Print the walk rather than just the answer: a missing level is a different
	// problem from a present level with an invalid leaf, and from KDL the
	// difference is otherwise invisible.
	const struct {
		const char*	name;
		uint32		shift;
	} kLevels[] = {
		{ "segment  ", SPARC_SEGMENT_TABLE_SHIFT },
		{ "directory", SPARC_PAGE_DIRECTORY_SHIFT },
		{ "table    ", SPARC_PAGE_TABLE_SHIFT },
	};

	phys_addr_t table = fPageTable;
	for (size_t level = 0; level < 3; level++) {
		uint32 index = (virtualAddress >> kLevels[level].shift)
			& SPARC_PAGE_TABLE_MASK;
		phys_addr_t slot = table + (phys_addr_t)index * sizeof(uint64);
		uint64 value = sparc_read_physical(slot);

		kprintf("  %s index %4" B_PRIu32 " at %#" B_PRIxPHYSADDR ": %#018"
			B_PRIx64 "\n", kLevels[level].name, index, slot, value);

		if (value == 0) {
			kprintf("  not mapped\n");
			return;
		}

		table = value;
	}

	kprintf("  pa %#" B_PRIxPHYSADDR "%s%s%s%s%s\n",
		(phys_addr_t)(table & TTE_PA_MASK),
		(table & TTE_VALID) != 0 ? " valid" : " INVALID",
		(table & TTE_WRITABLE) != 0 ? " w" : "",
		(table & TTE_SOFT_REAL_WRITABLE) != 0 ? " real-w" : "",
		(table & TTE_SOFT_ACCESSED) != 0 ? " accessed" : "",
		(table & TTE_SOFT_MODIFIED) != 0 ? " modified" : "");
}


// #pragma mark - SPARCVMPhysicalPageMapper


SPARCVMPhysicalPageMapper::SPARCVMPhysicalPageMapper()
{
}


SPARCVMPhysicalPageMapper::~SPARCVMPhysicalPageMapper()
{
}


/*!	Not implemented, and deliberately loud about it.

	Handing back a virtual address for an arbitrary physical page needs either a
	physical map covering all of RAM or the generic slot-pool mapper, and neither
	exists yet. Nothing on the paths the kernel currently takes calls this: the
	bulk operations below are what the VM actually uses, and they need no mapping
	at all.

	Returning an error rather than a wrong address is the point. A caller that
	got a plausible-looking virtual address for the wrong page would corrupt
	memory quietly.
*/
status_t
SPARCVMPhysicalPageMapper::GetPage(phys_addr_t physicalAddress,
	addr_t* _virtualAddress, void** _handle)
{
	return B_NOT_SUPPORTED;
}


status_t
SPARCVMPhysicalPageMapper::PutPage(addr_t virtualAddress, void* handle)
{
	return B_NOT_SUPPORTED;
}


status_t
SPARCVMPhysicalPageMapper::GetPageCurrentCPU(phys_addr_t physicalAddress,
	addr_t* _virtualAddress, void** _handle)
{
	return B_NOT_SUPPORTED;
}


status_t
SPARCVMPhysicalPageMapper::PutPageCurrentCPU(addr_t virtualAddress,
	void* _handle)
{
	return B_NOT_SUPPORTED;
}


status_t
SPARCVMPhysicalPageMapper::GetPageDebug(phys_addr_t physicalAddress,
	addr_t* _virtualAddress, void** _handle)
{
	return B_NOT_SUPPORTED;
}


status_t
SPARCVMPhysicalPageMapper::PutPageDebug(addr_t virtualAddress, void* handle)
{
	return B_NOT_SUPPORTED;
}


/*!	Fills physical memory, without mapping it anywhere.

	ASI_PHYS_USE_EC makes this a straight loop of physical stores. Eight bytes at
	a time once aligned, because the ASI store forms are the ordinary ones and
	the wide one is no more expensive.
*/
status_t
SPARCVMPhysicalPageMapper::MemsetPhysical(phys_addr_t address, int value,
	phys_size_t length)
{
	uint8 byte = (uint8)value;
	uint64 word = 0;
	for (int i = 0; i < 8; i++)
		word = (word << 8) | byte;

	while (length > 0 && (address & 7) != 0) {
		// The physical accessors work in 64-bit units, so a leading partial word
		// is handled by reading it, replacing the bytes that fall inside the
		// range, and writing it back.
		phys_addr_t aligned = address & ~(phys_addr_t)7;
		uint64 existing = sparc_read_physical(aligned);
		size_t offset = address - aligned;
		size_t count = 8 - offset;
		if (count > length)
			count = length;

		for (size_t i = 0; i < count; i++) {
			// Big-endian: byte 0 of the word is the most significant.
			uint32 shift = (7 - (offset + i)) * 8;
			existing &= ~((uint64)0xff << shift);
			existing |= (uint64)byte << shift;
		}

		sparc_write_physical(aligned, existing);
		address += count;
		length -= count;
	}

	while (length >= 8) {
		sparc_write_physical(address, word);
		address += 8;
		length -= 8;
	}

	if (length > 0) {
		uint64 existing = sparc_read_physical(address);
		for (size_t i = 0; i < length; i++) {
			uint32 shift = (7 - i) * 8;
			existing &= ~((uint64)0xff << shift);
			existing |= (uint64)byte << shift;
		}
		sparc_write_physical(address, existing);
	}

	return B_OK;
}


/*!	Copies out of physical memory a byte at a time.

	Byte at a time because the source and destination alignments are independent
	and this is not on any hot path -- correctness first, and the wide-word
	version can come with a measurement that says it matters.
*/
status_t
SPARCVMPhysicalPageMapper::MemcpyFromPhysical(void* to, phys_addr_t from,
	size_t length, bool user)
{
	if (user)
		return B_NOT_SUPPORTED;

	uint8* destination = (uint8*)to;

	for (size_t i = 0; i < length; i++) {
		phys_addr_t address = from + i;
		phys_addr_t aligned = address & ~(phys_addr_t)7;
		uint64 word = sparc_read_physical(aligned);
		uint32 shift = (7 - (address - aligned)) * 8;
		destination[i] = (uint8)(word >> shift);
	}

	return B_OK;
}


status_t
SPARCVMPhysicalPageMapper::MemcpyToPhysical(phys_addr_t to, const void* from,
	size_t length, bool user)
{
	if (user)
		return B_NOT_SUPPORTED;

	const uint8* source = (const uint8*)from;

	for (size_t i = 0; i < length; i++) {
		phys_addr_t address = to + i;
		phys_addr_t aligned = address & ~(phys_addr_t)7;
		uint64 word = sparc_read_physical(aligned);
		uint32 shift = (7 - (address - aligned)) * 8;
		word &= ~((uint64)0xff << shift);
		word |= (uint64)source[i] << shift;
		sparc_write_physical(aligned, word);
	}

	return B_OK;
}


void
SPARCVMPhysicalPageMapper::MemcpyPhysicalPage(phys_addr_t to, phys_addr_t from)
{
	for (size_t offset = 0; offset < B_PAGE_SIZE; offset += 8)
		sparc_write_physical(to + offset, sparc_read_physical(from + offset));
}
