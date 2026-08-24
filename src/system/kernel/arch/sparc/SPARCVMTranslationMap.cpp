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

#include "generic_vm_physical_page_mapper.h"

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


/*	Where physical RAM ends, from the boot loader's memory map.

	Recorded because a TTE has to say whether the page it describes may be
	cached, and on this machine the physical address answers that better than the
	caller does -- see tte_cache_flags_for_page().

	Set by sparc_record_physical_memory_top() before anything is mapped.
*/
static phys_addr_t sPhysicalMemoryTop;


/*!	Records the end of physical RAM. Call before the first mapping is made. */
void
sparc_record_physical_memory_top(kernel_args* args)
{
	for (uint32 i = 0; i < args->num_physical_memory_ranges; i++) {
		phys_addr_t end = args->physical_memory_range[i].start
			+ args->physical_memory_range[i].size;
		if (end > sPhysicalMemoryTop)
			sPhysicalMemoryTop = end;
	}
}


/*!	Decides whether a page may be cached, and whether it has side effects.

	The caller's memoryType is honoured when it expresses an opinion, but most
	callers do not have one: map_physical_memory() takes protection flags and
	nothing else, so the PCI bus manager maps sixteen megabytes of I/O ports with
	memoryType zero, meaning "default". Taking that at face value and mapping it
	write-back is what made the ATA driver read nonsense -- the first status
	register read filled a cache line from sixty-four bytes of device registers
	and every read after it was answered from the cache, so the controller was
	never asked again and never saw the commands.

	So the physical address decides when nothing else does. Above the top of RAM
	there is no memory on this machine, only device register blocks and firmware
	ROM, and neither is ever correct to cache: sun4u puts PCI configuration space
	at 0x1fe.01000000, I/O at 0x1fe.02000000 and memory at 0x1ff.00000000, all
	far above any RAM a Blade 150 or an Ultra 10 can hold. The rule is therefore
	exact rather than heuristic here, and it does not depend on drivers
	remembering to ask.

	Uncached implies TTE_SIDE_EFFECT as well, which is the more important half:
	it tells the processor that accesses to the page have consequences beyond
	their value, so they may not be issued speculatively, merged, or reordered
	with respect to each other. A device register block without it is a
	correctness problem that does not show up until the timing changes.
	UltraSPARC-IIi User's Manual section 6.2, printed page 76.

	Both cacheability bits are cleared together. Keeping CV while clearing CP is
	a legal combination the manual describes for aliased pages, and is not what
	is wanted for a device.
*/
static uint64
tte_cache_flags_for_page(phys_addr_t physicalAddress, uint32 memoryType)
{
	switch (memoryType & B_MEMORY_TYPE_MASK) {
		case B_UNCACHED_MEMORY:
		case B_WRITE_COMBINING_MEMORY:
			// sun4u has no write-combining mode to ask for, and combining is
			// exactly what a device page must not do, so both mean uncached.
			return TTE_SIDE_EFFECT;

		case B_WRITE_THROUGH_MEMORY:
		case B_WRITE_PROTECTED_MEMORY:
		case B_WRITE_BACK_MEMORY:
			// The hardware has one cacheable mode. A caller asking for
			// write-through gets write-back, which is a weaker guarantee about
			// when a store becomes visible, not about whether it does.
			return TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL;
	}

	if (physicalAddress >= sPhysicalMemoryTop)
		return TTE_SIDE_EFFECT;

	return TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL;
}


/*!	Turns Haiku's area protection flags into the TTE bits that express them.

	Cacheability is not decided here -- see tte_cache_flags_for_page(), which
	needs the physical address this function does not have. Protect() depends on
	that split: it changes permissions on a page whose physical address it does
	not re-read, so it keeps the cacheability bits already in the entry.

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
	uint64 flags = 0;

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


// #pragma mark - context ids


/*	Which of the 8192 context ids are taken.

	Id zero is the kernel's and is never allocated: kernel mappings are Global,
	so the hardware matches them whatever the context register holds, and the
	kernel needs no id of its own. See section 4.3 of the porting plan.

	A bitmap and a spinlock rather than anything cleverer, because this is touched
	twice in a team's life -- once at creation and once at destruction -- and the
	interesting part is not the allocation policy but what has to happen when an
	id changes hands. That is sparc_context_invalidate(), and it is the reason
	an id is only ever reused after being freed rather than stolen from a live
	address space.
*/
static const uint32 kContextWords = SPARC_CONTEXT_COUNT / 64;
static uint64 sContextBitmap[kContextWords];
static spinlock sContextLock = B_SPINLOCK_INITIALIZER;
static uint32 sContextsInUse;


/*!	Takes a free context id, or zero if there are none left.

	Zero is the kernel's id and therefore usable as a failure value: a caller
	that gets it back was asking for a user id and cannot have one.
*/
static uint32
allocate_context()
{
	InterruptsSpinLocker locker(sContextLock);

	for (uint32 word = 0; word < kContextWords; word++) {
		if (sContextBitmap[word] == ~(uint64)0)
			continue;

		uint32 bit = __builtin_ctzll(~sContextBitmap[word]);
		uint32 context = word * 64 + bit;

		// Only reachable for word zero, where bit zero is the kernel's.
		if (context == SPARC_KERNEL_CONTEXT) {
			if ((sContextBitmap[word] | 1) == ~(uint64)0)
				continue;
			bit = __builtin_ctzll(~(sContextBitmap[word] | 1));
			context = bit;
		}

		sContextBitmap[word] |= (uint64)1 << bit;
		sContextsInUse++;
		return context;
	}

	return SPARC_KERNEL_CONTEXT;
}


/*!	Gives a context id back, after removing every trace of its mappings.

	The order is not negotiable. Invalidating after the id is free would leave a
	window in which another team could be given it while the caches still hold
	the old team's translations under it -- and the failure that produces is not
	a slow path but one team reading another's memory.
*/
static void
free_context(uint32 context)
{
	if (context == SPARC_KERNEL_CONTEXT)
		return;

	sparc_context_invalidate(context);

	InterruptsSpinLocker locker(sContextLock);
	sContextBitmap[context / 64] &= ~((uint64)1 << (context % 64));
	sContextsInUse--;
}


// #pragma mark - SPARCVMTranslationMap


SPARCVMTranslationMap::SPARCVMTranslationMap(bool kernel, phys_addr_t pageTable)
	:
	fIsKernel(kernel),
	fPageTable(pageTable),
	fContext(SPARC_KERNEL_CONTEXT)
{
	// fLock is not ours. It is declared by VMTranslationMap, initialised by its
	// constructor and destroyed by its destructor, so doing either here is
	// duplicated ownership of somebody else's member -- and the destroy half of
	// that was fatal: mutex_destroy() marks a destroyed mutex by setting its
	// holder to 0, so the base class's destructor then found a holder of 0 where
	// it wanted -1 and panicked.
	//
	// It hid for four phases because the panic that used to stand where the page
	// table teardown is now fired first, before the base destructor could run,
	// and because the check is "holder != -1 && current thread != holder" -- so
	// a thread whose id is 0, which is every thread during early boot, matches a
	// destroyed lock and passes.
}


SPARCVMTranslationMap::~SPARCVMTranslationMap()
{
	free_context(fContext);

	if (!fIsKernel && fPageTable != 0)
		_FreePageTable();
}


/*!	Gives a user address space's page table back, one level at a time.

	Only the tables themselves. The pages they described belong to the areas that
	mapped them, and the VM has already unmapped every area by the time an address
	space is destroyed -- so a non-zero leaf entry here is a mapping somebody
	failed to remove, not a page to free, and freeing it would hand out memory
	that is still referenced.

	The kernel's table is never freed: it is the one every address space shares
	through the Global bit, and it outlives all of them.

	Interior entries hold physical addresses and are read with a physical access,
	the same way the miss handler walks them, so this needs nothing mapped.
*/
void
SPARCVMTranslationMap::_FreePageTable()
{
	for (uint32 segment = 0; segment < SPARC_PAGE_TABLE_ENTRIES; segment++) {
		phys_addr_t directory = sparc_read_physical(fPageTable
			+ segment * sizeof(uint64));
		if (directory == 0)
			continue;

		for (uint32 index = 0; index < SPARC_PAGE_TABLE_ENTRIES; index++) {
			phys_addr_t table = sparc_read_physical(directory
				+ index * sizeof(uint64));
			if (table != 0)
				_FreePageTablePage(table);
		}

		_FreePageTablePage(directory);
	}

	_FreePageTablePage(fPageTable);
	fPageTable = 0;
}


/*!	Returns one page of page table to the VM.

	The early boot allocator has no counterpart and needs none: it is only used
	for the kernel's own table, which is never freed.
*/
void
SPARCVMTranslationMap::_FreePageTablePage(phys_addr_t address)
{
	vm_page* page = vm_lookup_page(address / B_PAGE_SIZE);
	if (page == NULL) {
		panic("SPARCVMTranslationMap: page table page at %#" B_PRIxPHYSADDR
			" is not a page the VM knows about", address);
		return;
	}

	DEBUG_PAGE_ACCESS_START(page);
	vm_page_free(NULL, page);
}


/*!	Claims the context id this address space's mappings will be tagged with.

	Separate from the constructor because it can fail, and a translation map with
	no context is not a translation map: every mapping it made would be tagged
	with the kernel's id and matched for every team at once.
*/
status_t
SPARCVMTranslationMap::Init()
{
	if (fIsKernel)
		return B_OK;

	fContext = allocate_context();
	if (fContext == SPARC_KERNEL_CONTEXT) {
		dprintf("sparc: all %d MMU contexts are in use\n",
			SPARC_CONTEXT_COUNT - 1);
		return B_BUSY;
	}

	return B_OK;
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
	// A user map is created before anything is mapped into it, so it starts with
	// no table at all -- and the root is the one level the walk cannot allocate
	// for itself, because it is where the walk starts. Left at zero the walk
	// reads physical address zero and calls the bottom of memory a segment
	// table, which is a hang rather than an error.
	if (fPageTable == 0) {
		if (allocator == NULL)
			return 0;

		fPageTable = allocator->Allocate();
		if (fPageTable == 0)
			return 0;
	}

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
	sparc_tsb_invalidate(virtualAddress, fContext);
	sparc_tlb_demap(virtualAddress, fContext);
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
		| tte_cache_flags_for_page(physicalAddress, memoryType)
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
	// about it.
	//
	// Detach rather than Unlock, because PageUnmapped() releases fLock itself --
	// on both of its paths, the device-cache early return and the ordinary one.
	// Unlocking here as well released it twice, which is not the sort of thing
	// that fails where it happens.
	//
	// And no DEBUG_PAGE_ACCESS_START around the call. It reads as though this
	// were the code that touches the page, but the caller has already taken it:
	// VMTranslationMap::UnmapPages() brackets every UnmapPage() with
	// START/END when DEBUG_PAGE_ACCESS is on, and PageUnmapped() therefore only
	// checks. Taking it again panicked with "Invalid concurrent access to page
	// ... (start), currently accessed by: 15" -- the thread colliding with
	// itself -- the first time a module image was unloaded, which is the first
	// thing that happens after the boot volume is mounted.
	locker.Detach();

	PageUnmapped(area, (tte & TTE_PA_MASK) / B_PAGE_SIZE,
		(tte & TTE_SOFT_ACCESSED) != 0, (tte & TTE_SOFT_MODIFIED) != 0,
		updatePageQueue);

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

		// Keep the size, the physical address, what has been observed about the
		// page, and how it may be cached; replace what the protection decides.
		// Losing ACCESSED or MODIFIED here would tell the VM a written page was
		// clean, and losing the cacheability bits would turn a device register
		// block into cacheable memory the first time anything reprotected it.
		uint64 preserved = tte
			& (TTE_VALID | ((uint64)TTE_SIZE_MASK << TTE_SIZE_SHIFT)
				| TTE_PA_MASK | TTE_SOFT_ACCESSED | TTE_SOFT_MODIFIED
				| TTE_CACHEABLE_PHYSICAL | TTE_CACHEABLE_VIRTUAL
				| TTE_SIDE_EFFECT);

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

		// Detached rather than unlocked: UnaccessedPageUnmapped() releases fLock
		// itself, on every one of its paths. Unlocking here and calling it would
		// unlock twice, and the second one is by a thread that no longer holds
		// it -- which is a panic in the page daemon, minutes after boot, the
		// first time anything ages a page.
		locker.Detach();
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


/*	The window through which a physical page is reached by ordinary loads and
	stores.

	Most of what the kernel does to physical memory on this machine needs no
	window at all: ASI_PHYS_USE_EC addresses physical memory directly, which is
	what MemsetPhysical() and the two Memcpy methods below use, and it is both
	faster and simpler than mapping anything. But a caller that wants to *hand a
	physical page to somebody else as a pointer* -- ATAChannel's PIO transfer
	does, because the SCSI stack gives it a scatter-gather list of physical
	addresses -- needs a virtual address, and there is no ASI that provides one.

	So a reserved range of kernel address space, and page table entries written
	into it on demand. The bookkeeping is Haiku's own: the generic physical page
	mapper keeps a pool of chunks with an LRU, hands out a virtual address for a
	physical one, and asks the architecture only to make the mapping. PowerPC and
	m68k use the same code.

	A chunk of one page, unlike PowerPC's sixteen. The generic mapper's cost is
	per chunk rather than per page, so a larger chunk means fewer descriptors --
	but it also maps fifteen pages nobody asked for on every miss, and on a
	machine whose page size is already 8 KB the descriptors are not the expensive
	part.
*/
#define SPARC_IOSPACE_SIZE			(4 * 1024 * 1024)
#define SPARC_IOSPACE_CHUNK_SIZE	B_PAGE_SIZE

static addr_t sIOSpaceBase;


/*!	Points one chunk of the window at a physical address.

	Called by the generic mapper, which has already decided which chunk to reuse
	and has invalidated whatever was there. Writes the entry, then drops the
	address from the TSB and the TLB -- in that order, because a reader that sees
	a stale cache and a fresh table refills harmlessly while the reverse hands
	out a translation the table has already disowned.

	It never allocates. The leaf page tables covering the window are made once,
	in sparc_iospace_init(), from the boot-time allocator -- so this cannot fail
	partway through, and cannot need a page reservation in a path that has no way
	to ask for one.
*/
static status_t
map_iospace_chunk(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint32 flags)
{
	virtualAddress = ROUNDDOWN(virtualAddress, B_PAGE_SIZE);
	physicalAddress = ROUNDDOWN(physicalAddress, B_PAGE_SIZE);

	if (virtualAddress < sIOSpaceBase
		|| virtualAddress + SPARC_IOSPACE_CHUNK_SIZE
			> sIOSpaceBase + SPARC_IOSPACE_SIZE) {
		panic("map_iospace_chunk: %#" B_PRIxADDR " is outside the window at %#"
			B_PRIxADDR, virtualAddress, sIOSpaceBase);
		return B_BAD_VALUE;
	}

	for (size_t offset = 0; offset < SPARC_IOSPACE_CHUNK_SIZE;
			offset += B_PAGE_SIZE) {
		phys_addr_t entry = sparc_page_table_lookup(sparc_kernel_page_table(),
			virtualAddress + offset, NULL);
		if (entry == 0) {
			panic("map_iospace_chunk: no page table entry for %#" B_PRIxADDR
				"; sparc_iospace_init() should have made one",
				virtualAddress + offset);
			return B_ERROR;
		}

		uint64 tte = TTE_VALID | ((uint64)TTE_SIZE_8K << TTE_SIZE_SHIFT)
			| ((physicalAddress + offset) & TTE_PA_MASK)
			| tte_cache_flags_for_page(physicalAddress + offset, 0)
			| tte_flags_for_attributes(B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				true);

		sparc_write_physical(entry, tte);
		sparc_tsb_invalidate(virtualAddress + offset, SPARC_KERNEL_CONTEXT);
		sparc_tlb_demap(virtualAddress + offset, SPARC_KERNEL_CONTEXT);
	}

	return B_OK;
}


/*!	Reserves the window and builds the page tables that will cover it.

	The leaf tables are made now, while the boot-time allocator is still
	available, because map_iospace_chunk() runs in contexts that cannot allocate.
	Walking the range with a lookup is what builds them: the walk creates each
	level it finds missing and leaves the leaf entries invalid, which is exactly
	the state wanted -- addresses reserved, nothing mapped.
*/
status_t
sparc_iospace_init(kernel_args* args)
{
	status_t status = generic_vm_physical_page_mapper_init(args,
		map_iospace_chunk, &sIOSpaceBase, SPARC_IOSPACE_SIZE,
		SPARC_IOSPACE_CHUNK_SIZE);
	if (status != B_OK)
		return status;

	SPARCPageTableAllocator allocator = { args, NULL };
	for (addr_t address = sIOSpaceBase;
			address < sIOSpaceBase + SPARC_IOSPACE_SIZE;
			address += B_PAGE_SIZE) {
		if (sparc_page_table_lookup(sparc_kernel_page_table(), address,
				&allocator) == 0) {
			panic("sparc_iospace_init: no page table for %#" B_PRIxADDR,
				address);
			return B_NO_MEMORY;
		}
	}

	dprintf("sparc_vm: physical page window at %#" B_PRIxADDR ", %d KB in %"
		B_PRIuSIZE " KB chunks\n", sIOSpaceBase, SPARC_IOSPACE_SIZE / 1024,
		(size_t)SPARC_IOSPACE_CHUNK_SIZE / 1024);

	return B_OK;
}


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
/*!	Hands out a virtual address for a physical page, through the window.

	See the comment on SPARC_IOSPACE_SIZE. This is the case ASI_PHYS_USE_EC
	cannot serve, because what the caller wants is a pointer rather than an
	access.
*/
status_t
SPARCVMPhysicalPageMapper::GetPage(phys_addr_t physicalAddress,
	addr_t* _virtualAddress, void** _handle)
{
	return generic_get_physical_page(physicalAddress, _virtualAddress, 0);
}


status_t
SPARCVMPhysicalPageMapper::PutPage(addr_t virtualAddress, void* handle)
{
	return generic_put_physical_page(virtualAddress);
}


/*!	The same, for a caller pinned to the CPU it is running on.

	On a machine with more than one processor these want per-CPU slots that can
	be filled without a lock, which is what the x86 mapper provides and what the
	generic one declines to. This port runs on one processor -- sun4u
	workstations are uniprocessor, and arch_smp.cpp says so -- so the distinction
	has nothing to express yet, and the honest implementation is the same window.

	Pinning does not forbid blocking, only migration, so going through the pool
	is allowed here. The day this port meets a second processor, these two are
	where to look.
*/
status_t
SPARCVMPhysicalPageMapper::GetPageCurrentCPU(phys_addr_t physicalAddress,
	addr_t* _virtualAddress, void** _handle)
{
	return generic_get_physical_page(physicalAddress, _virtualAddress, 0);
}


status_t
SPARCVMPhysicalPageMapper::PutPageCurrentCPU(addr_t virtualAddress,
	void* _handle)
{
	return generic_put_physical_page(virtualAddress);
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
