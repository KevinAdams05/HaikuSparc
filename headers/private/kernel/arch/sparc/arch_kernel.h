/*
** Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
** Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
** Distributed under the terms of the MIT License.
*/
#ifndef _KERNEL_ARCH_SPARC_KERNEL_H
#define _KERNEL_ARCH_SPARC_KERNEL_H

#include <arch/cpu.h>

// Memory layout.
//
// The kernel is built with -mcmodel=medlow (build/jam/ArchitectureRules), which
// requires the whole kernel to be linked within the low 32 bits of the address
// space -- that model addresses code and data with 32-bit absolute forms. The
// loader and the linker script already act on that: the kernel is loaded at
// 0x80000000 and every early mapping it makes lands between there and the top of
// the 32-bit range.
//
// So the kernel address space is the upper 2 GB of the low 4 GB, which is the
// same shape as 32-bit x86 and for the same reason. Everything the kernel needs
// is inside it: the kernel image from 0x80000000, the early allocations from
// 0x81000000 upward, the frame buffer at 0xfd000000, and Open Firmware's own
// regions at 0xfef80000, 0xffd00000, 0xffe00000 and 0xfffce000.
//
// This file previously carried x86_64's values -- a 512 GB kernel space based at
// 0xffffff0000000000 -- which described an address space the kernel has never
// been anywhere near. The effect was silent: IS_KERNEL_ADDRESS rejected every
// address the kernel actually uses, so reserve_boot_loader_ranges() skipped all
// of them and the first attempt to create an area for already-mapped memory
// failed with B_BAD_VALUE.
//
// Userspace therefore gets the low 2 GB. That is a real limit rather than a
// generous one, and it is a consequence of the shared address space chosen in
// section 4.3 of sparc-port/PORTING_PLAN.md: kernel mappings are marked Global
// in their TTEs and user mappings are tagged with a context id, both in one
// address space, which is what avoids having to change IS_USER_ADDRESS and
// user_memcpy() for every architecture. Ticket #19597 discusses giving sparc
// separate kernel and user address spaces; if that is ever done, this is one of
// the files it changes.
#define KERNEL_LOAD_BASE_64_BIT 0x80000000ll

#define KERNEL_BASE				0x80000000
#define KERNEL_SIZE				0x80000000
#define KERNEL_TOP  			(KERNEL_BASE + (KERNEL_SIZE - 1))


// Userspace address space layout.
//
// A 1 MB hole at the bottom keeps NULL from ever being a valid mapping, and a
// 64 KB gap below KERNEL_BASE means a buffer a thread passes into a syscall
// cannot run off the end of userspace and into the kernel's half.
#define USER_BASE				0x100000
#define USER_BASE_ANY			USER_BASE
#define USER_SIZE				(KERNEL_BASE - (0x10000 + USER_BASE))
#define USER_TOP				(USER_BASE + (USER_SIZE - 1))

#define KERNEL_USER_DATA_BASE	0x70000000
#define USER_STACK_REGION		0x70000000
#define USER_STACK_REGION_SIZE	((USER_TOP - USER_STACK_REGION) + 1)

#endif	/* _KERNEL_ARCH_SPARC_KERNEL_H */

