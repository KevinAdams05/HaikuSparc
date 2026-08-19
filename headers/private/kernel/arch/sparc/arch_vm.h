/*
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2091, Adrien Destugues, pulkomandy@pulkomandy.tk. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_SPARC_VM_H
#define ARCH_SPARC_VM_H

/* This many pages will be read/written on I/O if possible */

#define NUM_IO_PAGES	4
	/* 16 kB */

// sun4u uses 8 KB pages, which is the smallest TTE size the MMU offers and what
// headers/posix/arch/sparc64/limits.h sets PAGESIZE to. PAGE_SHIFT has to agree
// with it: the two describe the same number and the kernel uses them
// interchangeably, shifting a page number by PAGE_SHIFT in some places and
// multiplying by B_PAGE_SIZE in others.
//
// This said 12 -- copied from every other architecture, all of which really do
// use 4 KB pages -- and the mismatch was silent and destructive. vm_page.cpp
// clears a freshly allocated page with
//
//     vm_memset_physical(page->physical_page_number << PAGE_SHIFT, 0, ...)
//
// so every such clear landed at half the page's real address: page N was zeroed
// at N * 4096 instead of N * 8192, somewhere else entirely, in the bottom half
// of physical memory. That is where the boot loader's data lives, and it was
// being overwritten a page at a time. The failure surfaced as the kernel image
// structure reading back as zeroes, thousands of instructions later.
#define PAGE_SHIFT 13

#endif	/* ARCH_SPARC_VM_H */

