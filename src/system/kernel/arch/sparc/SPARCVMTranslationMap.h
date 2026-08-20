/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _SPARC_VM_TRANSLATION_MAP_H
#define _SPARC_VM_TRANSLATION_MAP_H


#include <arch_mmu.h>
#include <arch_vm_translation_map.h>
#include <vm/VMTranslationMap.h>


struct kernel_args;


/*!	Where a new page table level comes from.

	There are two sources and they are available at different times. Before the
	VM is up, the only allocator is the boot-time physical page allocator, which
	needs the kernel_args. Afterwards it is the page allocator, which needs a
	reservation so that a mapping cannot fail halfway through being built.

	Making the caller say which one applies -- rather than inferring it from a
	global boot flag -- means the early path cannot accidentally be taken late,
	and a caller with neither is asking not to allocate at all.
*/
struct SPARCPageTableAllocator {
			kernel_args*		earlyArgs;
			vm_page_reservation* reservation;

			phys_addr_t			Allocate() const;
};


/*!	Walks a page table to the leaf entry for an address.

	A free function rather than only a method, because the early boot path needs
	the same walk before any translation map object exists -- and duplicating a
	three-level walk is exactly how the two copies end up disagreeing about the
	geometry.

	Returns the leaf entry's *physical* address, which is the form the assembly
	walk in the TLB miss handler uses. Zero if the address is unrepresentable, if
	a level is absent and \a allocator is NULL, or if allocating one failed.
*/
phys_addr_t sparc_page_table_lookup(phys_addr_t root, addr_t virtualAddress,
	const SPARCPageTableAllocator* allocator);


/*!	Records where physical RAM ends, so mappings above it can be made uncached.

	Has to be called before anything is mapped: a device page mapped write-back
	is read from the cache and never reaches the device. See
	tte_cache_flags_for_page() in the implementation for why the physical address
	is what decides this.
*/
void sparc_record_physical_memory_top(kernel_args* args);


/*!	Reserves the window through which a physical page can be handed out as a
	pointer, and builds the page tables that will cover it.

	Call from arch_vm_translation_map_init(), after the kernel page table exists:
	it builds the leaf tables for the window while the boot-time allocator is
	still available, because the code that fills them in later cannot allocate.
*/
status_t sparc_iospace_init(kernel_args* args);


/*!	A sun4u address space's translation map.

	The page table this wraps is the authoritative record; the TSB is a cache in
	front of it and the TLB a cache in front of that. Every method here maintains
	all three in that order -- table, then TSB, then TLB -- because a reader that
	sees a stale cache and a fresh table refills harmlessly, while the reverse
	hands out a translation the table has already disowned.

	The geometry and the reasoning behind it are in arch_vm_translation_map.h.
*/
struct SPARCVMTranslationMap : public VMTranslationMap {
								SPARCVMTranslationMap(bool kernel,
									phys_addr_t pageTable = 0);
	virtual						~SPARCVMTranslationMap();

	virtual	bool				Lock();
	virtual	void				Unlock();

	virtual	addr_t				MappedSize() const;
	virtual	size_t				MaxPagesNeededToMap(addr_t start,
									addr_t end) const;

	virtual	status_t			Map(addr_t virtualAddress,
									phys_addr_t physicalAddress,
									uint32 attributes, uint32 memoryType,
									vm_page_reservation* reservation);
	virtual	status_t			Unmap(addr_t start, addr_t end);

	virtual	status_t			UnmapPage(VMArea* area, addr_t address,
									bool updatePageQueue,
									bool deletingAddressSpace,
									uint32* _flags);

	virtual	status_t			Query(addr_t virtualAddress,
									phys_addr_t* _physicalAddress,
									uint32* _flags);
	virtual	status_t			QueryInterrupt(addr_t virtualAddress,
									phys_addr_t* _physicalAddress,
									uint32* _flags);

	virtual	status_t			Protect(addr_t base, addr_t top,
									uint32 attributes, uint32 memoryType);

	virtual	status_t			ClearFlags(addr_t virtualAddress,
									uint32 flags);

	virtual	bool				ClearAccessedAndModified(VMArea* area,
									addr_t address, bool unmapIfUnaccessed,
									bool& _modified);

	virtual	void				Flush();

	virtual	void				DebugPrintMappingInfo(addr_t virtualAddress);

			phys_addr_t			PageTable() const	{ return fPageTable; }

private:
			phys_addr_t			LookupEntry(addr_t virtualAddress,
									const SPARCPageTableAllocator* allocator);
			void				InvalidateCaches(addr_t virtualAddress);

			bool				fIsKernel;
			phys_addr_t			fPageTable;
};


/*!	The physical page mapper.

	sun4u needs less from this than most architectures, because ASI_PHYS_USE_EC
	lets privileged code read and write physical memory directly. The bulk
	operations are therefore real work done here rather than a mapping followed
	by an ordinary access.

	What that does not solve is handing a caller a usable virtual address for a
	physical page, which is what GetPage() is for. There is no ASI that produces
	one, so those go through a reserved window of kernel address space with page
	table entries written into it on demand -- see SPARC_IOSPACE_SIZE and
	sparc_iospace_init() in the implementation.
*/
struct SPARCVMPhysicalPageMapper : public VMPhysicalPageMapper {
								SPARCVMPhysicalPageMapper();
	virtual						~SPARCVMPhysicalPageMapper();

	virtual	status_t			GetPage(phys_addr_t physicalAddress,
									addr_t* _virtualAddress, void** _handle);
	virtual	status_t			PutPage(addr_t virtualAddress, void* handle);

	virtual	status_t			GetPageCurrentCPU(phys_addr_t physicalAddress,
									addr_t* _virtualAddress, void** _handle);
	virtual	status_t			PutPageCurrentCPU(addr_t virtualAddress,
									void* _handle);

	virtual	status_t			GetPageDebug(phys_addr_t physicalAddress,
									addr_t* _virtualAddress, void** _handle);
	virtual	status_t			PutPageDebug(addr_t virtualAddress,
									void* handle);

	virtual	status_t			MemsetPhysical(phys_addr_t address, int value,
									phys_size_t length);
	virtual	status_t			MemcpyFromPhysical(void* to, phys_addr_t from,
									size_t length, bool user);
	virtual	status_t			MemcpyToPhysical(phys_addr_t to,
									const void* from, size_t length, bool user);
	virtual	void				MemcpyPhysicalPage(phys_addr_t to,
									phys_addr_t from);
};


#endif	/* _SPARC_VM_TRANSLATION_MAP_H */
