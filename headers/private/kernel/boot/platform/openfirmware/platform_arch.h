/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef KERNEL_BOOT_PLATFORM_OPENFIRMWARE_ARCH_H
#define KERNEL_BOOT_PLATFORM_OPENFIRMWARE_ARCH_H


#include <SupportDefs.h>

struct kernel_args;

#ifdef __cplusplus
extern "C" {
#endif

/* memory management */
	
extern status_t arch_set_callback(void);
extern void *arch_mmu_allocate(void *address, size_t size, uint8 protection,
	bool exactAddress);
extern status_t arch_mmu_free(void *address, size_t size);
extern status_t arch_mmu_init(void);

/* Physical address behind an address the firmware uses.

   Some of what Open Firmware hands over is described by the address the
   firmware itself uses rather than by a physical one -- a display node's
   "address" property above all -- and the kernel, which builds its own
   mappings, needs the physical address. B_ERROR if there is no translation for
   it, in which case the caller must not pass the virtual address off as a
   physical one. */
extern status_t arch_mmu_translate(addr_t virtualAddress,
	phys_addr_t *_physicalAddress);

/* CPU */

extern status_t boot_arch_cpu_init(void);

/* kernel start */

status_t arch_start_kernel(struct kernel_args *kernelArgs, addr_t kernelEntry,
	addr_t kernelStackTop);

#ifdef __cplusplus
}
#endif

#endif	/* KERNEL_BOOT_PLATFORM_OPENFIRMWARE_ARCH_H */
