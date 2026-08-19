/*
 * Copyright 2003-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2011, Alexander von Gluck, kallisti5@unixzen.com
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Distributed under the terms of the MIT License.
 */


#include "start.h"

#include <string.h>

#include <OS.h>

#include <boot/addr_range.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/heap.h>
#include <platform/openfirmware/openfirmware.h>
#include <platform_arch.h>

#include "console.h"
#include "machine.h"
#include "real_time_clock.h"


// GCC defined globals
extern void (*__ctor_list)(void);
extern void (*__ctor_end)(void);

uint32 gMachine;
static uint32 sBootOptions;


void
call_ctors(void)
{
	void (**f)(void);

	for (f = &__ctor_list; f < &__ctor_end; f++) {
		(**f)();
	}
}


static addr_t
get_kernel_entry(void)
{
	if (gKernelArgs.kernel_image->elf_class == ELFCLASS64) {
		preloaded_elf64_image *image = static_cast<preloaded_elf64_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	} else if (gKernelArgs.kernel_image->elf_class == ELFCLASS32) {
		preloaded_elf32_image *image = static_cast<preloaded_elf32_image *>(
			gKernelArgs.kernel_image.Pointer());
		return image->elf_header.e_entry;
	}
	panic("Unknown kernel format! Not 32-bit or 64-bit!");
	return 0;
}


extern "C" void
platform_start_kernel(void)
{
	addr_t kernelEntry = get_kernel_entry();
	addr_t stackTop = gKernelArgs.cpu_kstack[0].start
		+ gKernelArgs.cpu_kstack[0].size;

	printf("kernel entry at %p\n", (void*)kernelEntry);
	printf("kernel stack top: %p\n", (void*)stackTop);

	// The kernel's early allocators require these arrays to be in ascending
	// order and do not check. allocate_early_virtual() looks for a gap between
	// range[i-1] and range[i], and vm_allocate_early_physical_page() checks that
	// the next page does not run into range[i+1]; neither means anything on an
	// unsorted array.
	//
	// insert_address_range() does not sort. It merges ranges that touch or
	// overlap, but appends a range that touches nothing wherever there is room,
	// so the order is whatever order things were allocated in. Every other
	// platform sorts here -- bios_ia32, all the EFI architectures, riscv and the
	// m68k ones -- and this one was the exception.
	//
	// Left unsorted the failure is quiet and destructive. Open Firmware's own
	// regions are recorded first and sit at the top of the address space, so the
	// kernel image's range ends up last; allocate_early_virtual() then takes its
	// "gap after the last range" path and hands out addresses straight through
	// whatever the loader allocated above the kernel. The symptom appears much
	// later, as a failed address range reservation.
	sort_address_ranges(gKernelArgs.physical_memory_range,
		gKernelArgs.num_physical_memory_ranges);
	sort_address_ranges(gKernelArgs.physical_allocated_range,
		gKernelArgs.num_physical_allocated_ranges);
	sort_address_ranges(gKernelArgs.virtual_allocated_range,
		gKernelArgs.num_virtual_allocated_ranges);

	/* TODO: ?
	mmu_init_for_kernel();
	smp_boot_other_cpus();
	*/

	status_t error = arch_start_kernel(&gKernelArgs, kernelEntry, stackTop);

	panic("Kernel returned! Return value: %" B_PRId32 "\n", error);
}


extern "C" void
platform_exit(void)
{
	of_interpret("reset-all", 0, 0);
}


extern "C" uint32
platform_boot_options(void)
{
	return sBootOptions;
}


extern "C" void
start(void *openFirmwareEntry)
{
	static char bootargs[512];

	// stage2 args - might be set via the command line one day
	stage2_args args;
	args.heap_size = 0;
	args.arguments = NULL;

	if (of_init((intptr_t (*)(void*))openFirmwareEntry) != B_OK)
		return;

	// check for arguments
	if (of_getprop(gChosen, "bootargs", bootargs, sizeof(bootargs))
			!= OF_FAILED) {
		static const char *sArgs[] = { NULL, NULL };
		sArgs[0] = (const char *)bootargs;
		args.arguments = sArgs;
		args.arguments_count = 1;
	}

	determine_machine();
	if (console_init() != B_OK)
		return;

#ifdef __powerpc__
	if ((gMachine & MACHINE_QEMU) != 0)
		dprintf("OpenBIOS (QEMU?) OpenFirmware machine detected\n");
	else if ((gMachine & MACHINE_PEGASOS) != 0)
		dprintf("Pegasos PowerPC machine detected\n");
	else
		dprintf("Apple PowerPC machine assumed\n");
#endif

	// Initialize and take over MMU and set the OpenFirmware callbacks - it
	// will ask us for memory after that instead of maintaining it itself
	// (the kernel will need to adjust the callback later on as well)
	arch_mmu_init();

	if (boot_arch_cpu_init() != B_OK)
		of_exit();

	init_real_time_clock();

	// check for key presses once
	sBootOptions = 0;
	int key = console_check_for_key();
	if (key == 32) {
		// space bar: option menu
		sBootOptions |= BOOT_OPTION_MENU;
	} else if (key == 27) {
		// ESC: debug output
		sBootOptions |= BOOT_OPTION_DEBUG_OUTPUT;
	}

	gKernelArgs.platform_args.openfirmware_entry = openFirmwareEntry;

	main(&args);
		// if everything goes fine, main() never returns

	of_exit();
}
