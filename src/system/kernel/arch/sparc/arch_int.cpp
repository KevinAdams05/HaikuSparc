/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <setjmp.h>
#include <stddef.h>
#include <string.h>

#include <arch_debug.h>
#include <arch_thread_types.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <arch_mmu.h>
#include <kernel.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>

#include "SPARCVMTranslationMap.h"
#include <util/AutoLock.h>

#include <platform/openfirmware/openfirmware.h>


extern "C" void sparc_iframe_offsets(uint64 *out);
extern "C" void sparc_enter_userspace(addr_t entry, addr_t stackPointer,
	addr_t arg1, addr_t arg2);
extern "C" uint32 sparc_user_test_program[];
extern "C" uint32 sparc_user_test_program_end[];

// Set up by sparc_test_userspace(), read by sparc_syscall().
static jmp_buf sUserTestReturn;
static bool sUserTestActive = false;
static uint64 sUserTestValue = 0;

// Defined below sparc_interrupt(), which is where it is easiest to read, and
// called from both of the paths that reach it.
static void sparc_interrupt_epilogue();


/*	The SOFTINT register, ASR 22, and the two write-only aliases that set and
	clear bits in it without a read-modify-write (UltraSPARC-IIi manual TABLE
	11-9, printed p.123). Bits 15:1 are software interrupts at the matching
	levels; bit 0 is TICK_INT, which the %TICK comparison sets and which arrives
	as a level-14 interrupt.
*/
#define SOFTINT_TICK		(1ULL << 0)
#define SOFTINT_LEVEL_14	(1ULL << 14)

/*	Reached by ASR number rather than by name. The assembler rejects %softint,
	%set_softint, %clear_softint and %tick_cmpr with "architecture mismatch"
	under the architecture the kernel is compiled for, while accepting the same
	registers addressed numerically. The numbers are from the ASR table on
	printed p.83: SET_SOFTINT is 20, CLEAR_SOFTINT 21, SOFTINT 22 and
	TICK_CMPR 23.
*/


static inline uint64
sparc_read_softint()
{
	uint64 value;
	asm volatile("rd %%asr22, %0" : "=r"(value));
	return value;
}


static inline void
sparc_clear_softint(uint64 bits)
{
	asm volatile("wr %0, 0, %%asr21" : : "r"(bits));
}


/*	sabre's interrupt controller.

	On sun4u the thing that decides whether a device interrupt reaches the
	processor is not a separate chip on a bus that could be driven by a driver --
	it is a register file inside the host bridge, which on UltraSPARC-IIi is
	inside the processor module. So it belongs to the kernel, the way the openpic
	belongs to the kernel on PowerPC, and not to the PCI bus driver. The bus
	driver's job stops at telling us which INO a device is wired to.

	Two register files, and which one an INO uses is decided by its value rather
	than by what kind of device it is. That distinction is easy to get wrong and
	the consequence is silent: the IDE controller on this machine is a PCI device
	whose interrupt is INO 0x20 -- the on-board SCSI slot, because that is the
	slot the onboard storage occupies on an Ultra 5/10 -- so it is programmed
	through the OBIO registers and not the PCI ones. The firmware's interrupt-map
	says 0x20, and believing "PCI device therefore PCI register" would have
	programmed a register belonging to nothing.

	Addresses from the UltraSPARC-IIi User's Manual, section 19.3.3, printed
	pages 316 to 319. The mapping registers are the "partial" kind: the INO is
	fixed by which register it is and only the group number and the valid bit are
	writable, which is why enabling one is a read-modify-write of a single bit and
	never a write of the number itself.

	Note that a mapping register covers a whole PCI slot -- all four of its pins
	share one -- while a clear register is per pin. Hence the different shifts.
*/
#define SABRE_PCI_INTERRUPT_MAP		0x0c00
#define SABRE_OBIO_INTERRUPT_MAP	0x1000
#define SABRE_PCI_INTERRUPT_CLEAR	0x1400
#define SABRE_OBIO_INTERRUPT_CLEAR	0x1800

/*	Physical, non-cacheable. Not the 0x14 the page table walk uses, which is
	"physical, external cacheable only": these are device registers, and reads of
	them have to reach the bridge rather than a cache line that was filled from it
	once. Big-endian, unlike PCI configuration space -- these registers belong to
	the host side of the bridge, so no byte swap.
*/
#define ASI_PHYS_NON_CACHED		0x15

// The bridge's own register block, from its Open Firmware node. Zero until
// arch_int_init_io() finds it, and every accessor below declines while it is.
static phys_addr_t sInterruptControllerBase;


static inline uint64
sparc_read_physical64(phys_addr_t address)
{
	uint64 value;
	asm volatile("ldxa [%[address]] %[asi], %[value]"
		: [value] "=r"(value)
		: [address] "r"(address), [asi] "i"(ASI_PHYS_NON_CACHED));
	return value;
}


static inline void
sparc_write_physical64(phys_addr_t address, uint64 value)
{
	asm volatile("stxa %[value], [%[address]] %[asi]"
		: : [value] "r"(value), [address] "r"(address),
			[asi] "i"(ASI_PHYS_NON_CACHED)
		: "memory");
}


/*!	Physical address of the mapping register that gates this INO. */
static phys_addr_t
sparc_interrupt_map_register(int32 ino)
{
	if (sInterruptControllerBase == 0)
		return 0;

	if (ino >= INO_FIRST_OBIO) {
		return sInterruptControllerBase + SABRE_OBIO_INTERRUPT_MAP
			+ ((ino & 0x1f) << 3);
	}

	// One register per slot rather than per pin, so the two pin bits drop out.
	return sInterruptControllerBase + SABRE_PCI_INTERRUPT_MAP
		+ ((ino & 0x3c) << 1);
}


/*!	Physical address of the clear register for this INO. */
static phys_addr_t
sparc_interrupt_clear_register(int32 ino)
{
	if (sInterruptControllerBase == 0)
		return 0;

	return sInterruptControllerBase
		+ (ino >= INO_FIRST_OBIO
			? SABRE_OBIO_INTERRUPT_CLEAR : SABRE_PCI_INTERRUPT_CLEAR)
		+ ((ino & 0x1f) << 3);
}


/*!	Finds the interrupt controller's registers in the device tree.

	The bridge's "reg" property is its register block, two cells of address on a
	machine whose root has #address-cells of 2. Read rather than hardcoded for
	the same reason the PCI address ranges are: sabre is on-die and always at
	0x1fe.00000000 on the machines this port has run on, and a base address is
	exactly the kind of constant that turns out to differ on the next one.
*/
static status_t
sparc_find_interrupt_controller()
{
	intptr_t root = of_finddevice("/");
	if (root == OF_FAILED)
		return B_ERROR;

	for (intptr_t node = of_child(root); node != 0 && node != OF_FAILED;
			node = of_peer(node)) {
		char type[16];
		if (of_getprop(node, "device_type", type, sizeof(type)) == OF_FAILED)
			continue;
		if (strcmp(type, "pci") != 0)
			continue;

		uint32 reg[2];
		if (of_getprop(node, "reg", reg, sizeof(reg)) == OF_FAILED)
			continue;

		sInterruptControllerBase = ((phys_addr_t)reg[0] << 32) | reg[1];
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


/*!	Services one interrupt packet.

	The vector is read before anything else, because BUSY has to be cleared for
	the processor to accept another packet and the vector is gone once it is. Then
	the handler, and only then the bridge's state machine back to IDLE -- in that
	order because the interrupt is level-sensitive at the device: clearing before
	the handler has quietened the device just means taking it again immediately.

	Nothing here can nest. The trap cleared PSTATE.IE and the entry path raised
	%pil, so a second packet waits until this returns.
*/
static void
sparc_interrupt_vector()
{
	uint64 inr;
	asm volatile("ldxa [%[offset]] %[asi], %[value]"
		: [value] "=r"(inr)
		: [offset] "r"((uint64)INTR_DATA_0), [asi] "i"(ASI_INTR_DATA));

	asm volatile("stxa %%g0, [%%g0] %[asi]" : : [asi] "i"(ASI_INTR_RECEIVE)
		: "memory");

	int32 ino = inr & INR_INO_MASK;

	// The first packet on each INO, said once. Whether a device interrupt is
	// being *delivered* is not otherwise visible from a boot log -- a driver
	// that polls and a driver whose interrupts work look the same from outside --
	// and one line per source is a cheap way to be sure rather than hopeful.
	// There are 64 INOs, so the record of which have been seen is one word.
	static uint64 sReported;
	if ((sReported & (1ULL << ino)) == 0) {
		sReported |= 1ULL << ino;
		dprintf("sparc_int: first interrupt packet on vector %" B_PRId32 "\n",
			ino);
	}

	io_interrupt_handler(ino, true);

	phys_addr_t clear = sparc_interrupt_clear_register(ino);
	if (clear != 0)
		sparc_write_physical64(clear, INTCLR_IDLE);
}


/*!	Checks that the assembly and arch_int.h agree about struct iframe.

	The entry path reaches the frame through numeric offsets, and two copies of
	the same numbers drift. Here the drift would be silent and specific: the
	handler would restore the interrupted code's %g3 from where its %g4 was
	saved, and the interrupted code would resume with plausible-looking garbage.
*/
static void
sparc_verify_iframe_layout()
{
	uint64 assembler[18];
	sparc_iframe_offsets(assembler);

	const uint64 declared[18] = {
		offsetof(iframe, tstate), offsetof(iframe, tpc),
		offsetof(iframe, tnpc), offsetof(iframe, tt), offsetof(iframe, y),
		offsetof(iframe, g1), offsetof(iframe, g2), offsetof(iframe, g3),
		offsetof(iframe, g4), offsetof(iframe, g5), offsetof(iframe, g6),
		offsetof(iframe, g7), offsetof(iframe, pil), offsetof(iframe, sfsr),
		offsetof(iframe, sfar), offsetof(iframe, tagAccess),
		offsetof(iframe, out), IFRAME_SIZEOF,
	};

	for (int i = 0; i < 18; i++) {
		if (assembler[i] != declared[i]) {
			panic("sparc iframe layout %d: arch_traps.S says %#" B_PRIx64
				", arch_int.h says %#" B_PRIx64, i, assembler[i], declared[i]);
		}
	}

	if (sizeof(iframe) > IFRAME_SIZEOF || IFRAME_SIZEOF % 16 != 0)
		panic("sparc iframe size %d does not fit a 16-byte aligned frame",
			(int)sizeof(iframe));
}


/*!	Every interrupt the kernel takes, from sparc_interrupt_entry().

	Runs at trap level zero on the interrupted thread's kernel stack, with
	PSTATE.IE still clear -- the hardware cleared it on the trap and nothing here
	sets it, so this cannot nest.

	Two unrelated paths arrive here, and only the entry code is shared. A device
	interrupt is an interrupt *packet* -- trap type 0x60, the vector read from an
	ASI register. The processor's own SOFTINT register arrives as one of the
	level traps, and that is what the %TICK comparator uses.

	The level-14 handler has to check both SOFTINT<14> and TICK_INT, because the
	two share a level: section 14.5.1 of the manual says so outright, and treating
	the level as implying the source would misreport a software interrupt as a
	clock tick the day one is used.
*/
extern "C" void
sparc_interrupt(struct iframe *frame)
{
	if (frame->tt == TRAP_INTERRUPT_VECTOR) {
		sparc_interrupt_vector();
		sparc_interrupt_epilogue();
		return;
	}

	// The first arrival of each trap type, said once. Which interrupt paths a
	// machine actually uses is not something a manual settles -- an emulator may
	// deliver as a packet what silicon delivers as a level, or the reverse -- and
	// this is how to find out rather than assume.
	static bool sReportedTrap[256];
	if (frame->tt < 256 && !sReportedTrap[frame->tt]) {
		sReportedTrap[frame->tt] = true;
		dprintf("sparc_int: first interrupt with trap type %#" B_PRIx64
			", softint %#" B_PRIx64 "\n", frame->tt, sparc_read_softint());
	}

	uint64 pending = sparc_read_softint();

	if ((pending & SOFTINT_TICK) != 0) {
		// Cleared before the handler runs, not after: the handler reprograms
		// the comparator, and clearing afterwards could drop a tick that
		// arrived in between.
		sparc_clear_softint(SOFTINT_TICK);
		timer_interrupt();
	}

	if ((pending & SOFTINT_LEVEL_14) != 0)
		sparc_clear_softint(SOFTINT_LEVEL_14);

	if ((pending & ~(SOFTINT_TICK | SOFTINT_LEVEL_14)) != 0) {
		panic("sparc: interrupt with nothing to service it, trap %#" B_PRIx64
			", softint %#" B_PRIx64, frame->tt, pending);
	}

	sparc_interrupt_epilogue();
}


/*!	What every interrupt does on the way out, whichever path brought it here.

	The scheduler cannot be invoked from inside the trap: it would switch stacks
	with a trap frame still on this one. Haiku's convention is to note the request
	and act on it here, on the way out, which is also where a thread's
	post-interrupt callback runs.
*/
static void
sparc_interrupt_epilogue()
{
	if (debug_debugger_running())
		return;

	// Interrupts are off -- the trap cleared PSTATE.IE and the entry path raised
	// %pil to 15 -- and they stay that way across the reschedule. Restoring the
	// state explicitly afterwards is not a formality: rescheduling switches to
	// another thread and comes back later, and what the interrupt-enable bit
	// holds on the way back is whatever the other thread left it as.
	Thread *thread = thread_get_current_thread();
	cpu_status state = disable_interrupts();

	if (thread->post_interrupt_callback != NULL) {
		void (*callback)(void *) = thread->post_interrupt_callback;
		void *data = thread->post_interrupt_data;

		thread->post_interrupt_callback = NULL;
		thread->post_interrupt_data = NULL;

		restore_interrupts(state);
		callback(data);
	} else if (thread->cpu->invoke_scheduler) {
		SpinLocker schedulerLocker(thread->scheduler_lock);
		scheduler_reschedule(B_THREAD_READY);
		schedulerLocker.Unlock();
		restore_interrupts(state);
	} else {
		restore_interrupts(state);
	}

}


status_t
arch_int_init(kernel_args *args)
{
	sparc_verify_iframe_layout();
	return B_OK;
}


status_t
arch_int_init_post_vm(kernel_args *args)
{
	return B_OK;
}


/*!	Takes the system call trap, from kernel mode.

	`ta` traps the same at either privilege level, so the whole path can be
	checked before there is a userland to call it from: the table entry, the
	iframe, the drop to trap level zero, the arguments arriving in the right
	registers, the result getting home, and the return landing after the trap
	rather than on it.

	The last of those is the one worth testing deliberately. A trap returns with
	`retry`, which re-executes the trapping instruction -- so a system call that
	does not move %tnpc into %tpc loops forever, and the symptom is a hang with no
	output rather than a wrong answer.

	Six arguments summed, so a wrong total says the arguments did not arrive while
	a right total from a wrong register says the result did not get back.
*/
static void
sparc_test_syscall()
{
	const uint64 kArguments[6] = { 1, 2, 4, 8, 16, 32 };
	const uint64 kExpected = 63;

	uint64 result = 0;
	asm volatile(
		"mov	%[index], %%g1\n\t"
		"mov	%[a0], %%o0\n\t"
		"mov	%[a1], %%o1\n\t"
		"mov	%[a2], %%o2\n\t"
		"mov	%[a3], %%o3\n\t"
		"mov	%[a4], %%o4\n\t"
		"mov	%[a5], %%o5\n\t"
		"ta	%[trap]\n\t"
		"mov	%%o0, %[result]"
		: [result] "=r"(result)
		: [index] "r"((uint64)SPARC_SYSCALL_TEST_ECHO),
		  [a0] "r"(kArguments[0]), [a1] "r"(kArguments[1]),
		  [a2] "r"(kArguments[2]), [a3] "r"(kArguments[3]),
		  [a4] "r"(kArguments[4]), [a5] "r"(kArguments[5]),
		  [trap] "i"(SPARC_SYSCALL_TRAP)
		: "g1", "o0", "o1", "o2", "o3", "o4", "o5", "memory");

	dprintf("sparc_int: syscall returned %" B_PRIu64 " of %" B_PRIu64
		" -- %s\n", result, kExpected,
		result == kExpected ? "trap taken and returned" : "WRONG");

	if (result != kExpected) {
		panic("sparc: the system call trap returned %" B_PRIu64 ", wanted %"
			B_PRIu64, result, kExpected);
	}
}


/*!	Runs an instruction in userspace.

	The first one this port has ever run, and it exercises the pieces that could
	not be tested any other way: an address space of its own, so the mappings are
	context-tagged and the miss handler has to find them through the
	per-address-space page table root; a drop of privilege through `done`; a trap
	back out of unprivileged code; and the trap entry choosing a kernel stack
	rather than the user's.

	The program is three instructions, assembled rather than hand-encoded -- see
	sparc_user_test_program in arch_asm.S -- and copied into a page. It reports a
	value the kernel did not choose, so "the trap happened" and "the trap happened
	where I meant it to" stay distinguishable.

	Two deliberate compromises, both because this is a probe and not a thread.

	The address space is never destroyed. A user page table's teardown is not
	written yet and panics if reached, so keeping the reference forever is the
	quiet option and the alternative is a boot that dies on cleanup.

	And the trap out of userspace is pointed at a scratch stack instead of this
	thread's own. A trap from userspace builds its frame at the top of the kernel
	stack, which is right for a thread that really left the kernel and wrong here:
	this function is still on that stack, and its frames are what the top holds.
*/
static void
sparc_test_userspace()
{
	const addr_t kCodeAddress = 0x20000000;
	const addr_t kStackAddress = 0x20100000;
	const uint64 kExpected = 0x5ac;

	// The stack the trap out of userspace lands on. See above.
	static uint8 sTrapStack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

	VMAddressSpace* addressSpace;
	status_t status = VMAddressSpace::Create(1234, USER_BASE, USER_SIZE, false,
		&addressSpace);
	if (status != B_OK) {
		panic("sparc: the userspace test could not make an address space: %s",
			strerror(status));
		return;
	}

	SPARCVMTranslationMap* map = static_cast<SPARCVMTranslationMap*>(
		addressSpace->TranslationMap());

	// Two pages for the program and its stack, and room for the page table
	// levels underneath them -- a root, a directory and a leaf table, which the
	// two addresses share because they are close enough to land in the same
	// entries above the leaf. Reserved generously rather than exactly: getting it
	// wrong asserts inside vm_page_allocate_page() rather than failing the map.
	vm_page_reservation reservation;
	vm_page_reserve_pages(&reservation, 16, VM_PRIORITY_SYSTEM);

	vm_page* codePage = vm_page_allocate_page(&reservation,
		PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
	vm_page* stackPage = vm_page_allocate_page(&reservation,
		PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);

	map->Lock();
	// Writable as well as executable, because the kernel has to put the program
	// there and the only mapping of that page is this one.
	status_t codeStatus = map->Map(kCodeAddress,
		(phys_addr_t)codePage->physical_page_number * B_PAGE_SIZE,
		B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA, 0, &reservation);
	status_t stackStatus = map->Map(kStackAddress,
		(phys_addr_t)stackPage->physical_page_number * B_PAGE_SIZE,
		B_READ_AREA | B_WRITE_AREA, 0, &reservation);
	map->Unlock();

	if (codeStatus != B_OK || stackStatus != B_OK) {
		panic("sparc: the userspace test could not map its pages: code %s, "
			"stack %s", strerror(codeStatus), strerror(stackStatus));
		return;
	}

	vm_page_unreserve_pages(&reservation);

	dprintf("sparc_int: userspace: context %" B_PRIu32 ", page table %#"
		B_PRIxPHYSADDR ", code at %#" B_PRIxADDR "\n", map->Context(),
		map->PageTable(), kCodeAddress);

	// Nothing may preempt this. A reschedule would switch address spaces, and
	// the return into userspace would then be matched against another team's
	// context -- a wrong answer rather than a failure.
	cpu_status interruptState = disable_interrupts();

	Thread* thread = thread_get_current_thread();
	addr_t savedKernelStack = thread->kernel_stack_top;

	sparc_switch_address_space(map->Context(), map->PageTable());
	sparc_set_kernel_stack((addr_t)sTrapStack + sizeof(sTrapStack));

	memcpy((void*)kCodeAddress, (const void*)&sparc_user_test_program,
		(size_t)((addr_t)&sparc_user_test_program_end
			- (addr_t)&sparc_user_test_program));
	arch_cpu_sync_icache((void*)kCodeAddress, B_PAGE_SIZE);

	sUserTestActive = true;
	sUserTestValue = 0;

	if (setjmp(sUserTestReturn) == 0) {
		addr_t frame = ROUNDDOWN(kStackAddress + B_PAGE_SIZE
			- SPARC_MINIMUM_FRAME_SIZE, 16);
		sparc_enter_userspace(kCodeAddress, frame - SPARC_STACK_BIAS, 0, 0);
	}

	// Back here through longjmp() from the system call handler.
	//
	// The root goes back to the kernel's own, not to zero. The miss handler
	// selects between the two roots by address, and "user address" there means
	// anything below KERNEL_BASE -- which includes the firmware's low identity
	// mappings. A zero root makes the next miss on one of those walk from
	// physical address zero. That is what the first attempt did, and it stopped
	// the boot at the first device interrupt.
	sparc_set_kernel_stack(savedKernelStack);
	sparc_switch_address_space(SPARC_KERNEL_CONTEXT, sparc_kernel_page_table());
	restore_interrupts(interruptState);

	dprintf("sparc_int: userspace returned %#" B_PRIx64 " of %#" B_PRIx64
		" -- %s\n", sUserTestValue, kExpected,
		sUserTestValue == kExpected ? "ran in userspace" : "WRONG");

	if (sUserTestValue != kExpected) {
		panic("sparc: the userspace test reported %#" B_PRIx64 ", wanted %#"
			B_PRIx64, sUserTestValue, kExpected);
	}
}


status_t
arch_int_init_post_device_manager(struct kernel_args *args)
{
	// Runs inside main2(), which is the first thread the scheduler ever picks.
	// That matters: nothing is scheduled before scheduler_start(), as main.cpp
	// says where it spawns this thread, so the obvious earlier hooks -- including
	// arch_platform_init_post_thread(), where this was first put -- can create
	// threads and resume them and watch them never run.
	sparc_test_context_switch();
	sparc_test_thread_wait();
	sparc_test_preemption();
	sparc_test_backtrace();
	sparc_test_user_memory();
	sparc_test_syscall();
	sparc_test_userspace();

	return B_OK;
}


status_t
arch_int_init_io(kernel_args* args)
{
	status_t status = sparc_find_interrupt_controller();
	if (status != B_OK) {
		dprintf("sparc_int: no host bridge to take interrupts from; devices "
			"will have to poll\n");
		return B_OK;
	}

	dprintf("sparc_int: interrupt controller at %#" B_PRIxPHYSADDR "\n",
		sInterruptControllerBase);

	return B_OK;
}


/*!	Lets an interrupt through, or holds it.

	The valid bit is the only field of a partial mapping register this may touch:
	the interrupt number is fixed by which register it is, and the target
	processor is read-only. So read, change one bit, write -- which is also what
	makes disabling safe, since a held interrupt is not a lost one.
*/
void
arch_int_enable_io_interrupt(int32 irq)
{
	phys_addr_t map = sparc_interrupt_map_register(irq);
	if (map == 0)
		return;

	// The group number goes in at the same time. On this processor it is fixed
	// and the firmware has usually already written it, but a register the
	// firmware never touched reads as zero, and an INR of zero is not this
	// bridge's.
	uint64 value = sparc_read_physical64(map);
	value &= ~(uint64)INR_IGN_MASK;
	value |= SPARC_IIi_IGN | INTMAP_VALID;

	sparc_write_physical64(map, value);

	// Read back, because there is no other way to know the register is there.
	// This runs a handful of times a boot -- once per installed handler -- and
	// the address it writes is arithmetic from a manual, against a bridge that
	// may be emulated. A register that silently ignores writes would otherwise
	// present as a device whose interrupts never arrive, which is a much longer
	// road to the same conclusion.
	uint64 readback = sparc_read_physical64(map);
	if (readback != value) {
		dprintf("sparc_int: vector %" B_PRId32 " mapping register %#"
			B_PRIxPHYSADDR " reads %#" B_PRIx64 " after writing %#" B_PRIx64
			"\n", irq, map, readback, value);
	} else {
		dprintf("sparc_int: vector %" B_PRId32 " enabled (%#" B_PRIxPHYSADDR
			" = %#" B_PRIx64 ")\n", irq, map, value);
	}

	// A device may have been asserting while the interrupt was held, in which
	// case the state machine is already out of IDLE and would deliver a packet
	// for something no handler has seen yet.
	phys_addr_t clear = sparc_interrupt_clear_register(irq);
	if (clear != 0)
		sparc_write_physical64(clear, INTCLR_IDLE);
}


void
arch_int_disable_io_interrupt(int32 irq)
{
	phys_addr_t map = sparc_interrupt_map_register(irq);
	if (map == 0)
		return;

	sparc_write_physical64(map, sparc_read_physical64(map) & ~INTMAP_VALID);
}


int32
arch_int_assign_to_cpu(int32 irq, int32 cpu)
{
	// Not yet supported.
	return 0;
}


// #pragma mark - the preemption test


static int32 sSpinnerCount;
static bool sStopSpinner;


/*!	Spins, and never gives up the CPU on purpose.

	No yield, no blocking call, nothing that reaches the scheduler. The only way
	this thread can lose the processor is for a timer interrupt to take it away,
	which is the point.
*/
static status_t
sparc_preemption_spinner(void *data)
{
	while (!sStopSpinner)
		atomic_add(&sSpinnerCount, 1);

	return B_OK;
}


/*!	Proves a periodic tick preempts a busy loop.

	The context switch test alternates two threads with thread_yield(), which
	demonstrates that switching works but says nothing about preemption: both
	threads there ask to be switched away. This one has neither thread ask.

	A spinner is started, and then this thread busy-waits for it without
	yielding. If nothing can take the CPU away from a running thread, whichever
	of the two started first keeps it and the counter stays at zero forever. A
	non-zero counter means the timer interrupt took the processor from one thread
	and gave it to the other, which is the whole of Phase 4.

	The deadline uses system_time(), which is the other half of this phase, so a
	failure of either shows up here.
*/
void
sparc_test_preemption()
{
	sSpinnerCount = 0;
	sStopSpinner = false;

	thread_id spinner = spawn_kernel_thread(sparc_preemption_spinner,
		"sparc spinner", B_NORMAL_PRIORITY, NULL);
	if (spinner < 0) {
		dprintf("sparc_int: could not spawn the preemption test\n");
		return;
	}

	resume_thread(spinner);

	bigtime_t start = system_time();
	const bigtime_t kDeadline = 2000000;

	while (atomic_get(&sSpinnerCount) == 0
		&& system_time() - start < kDeadline) {
		// Deliberately empty, and deliberately without a yield.
	}

	bigtime_t elapsed = system_time() - start;
	int32 count = atomic_get(&sSpinnerCount);
	sStopSpinner = true;

	dprintf("sparc_int: spinner reached %" B_PRId32 " after %" B_PRIdBIGTIME
		" us without either thread yielding -- %s\n", count, elapsed,
		count > 0 ? "preempted" : "NOT PREEMPTED");

	if (count == 0) {
		panic("sparc: a busy loop was never preempted in %" B_PRIdBIGTIME
			" us; the timer interrupt is not reaching the scheduler", elapsed);
	}
}


/*!	The traps that mean the VM has to look at an address.

	Runs at trap level zero on the faulting thread's own stack, which is what
	makes it possible to block here at all -- and blocking is normal for a page
	fault, since resolving one can mean paging something in or taking a mutex.

	Interrupts are re-enabled if the faulting context had them enabled, and not
	otherwise. That is not caution for its own sake: a thread that blocks with
	interrupts disabled never gets the CPU back, because the timer that would
	preempt whoever it is waiting for cannot fire. And a context that faulted
	*with* interrupts already off is one that must not block -- user_memcpy() is
	the case that matters -- which is exactly what the checks below handle
	instead.
*/
/*!	The system call trap.

	Reached by `ta SPARC_SYSCALL_TRAP` from either privilege level, with the index
	in %g1 and up to six arguments in %o0-%o5 -- which the entry has put in
	frame->out, because window registers cannot reach C any other way.

	Advancing past the trapping instruction is this function's job and not the
	entry's. TRAP_TO_C reloads %tpc and %tnpc from the frame on the way out, so
	moving %tnpc into %tpc is the whole of "return after the `ta`" -- and doing it
	here rather than in assembly is what lets a handler that needs to restart or
	redirect the call simply not do it.

	Not yet a dispatcher. The kernel's syscall table is reached through
	syscall_dispatcher(), which wants a contiguous argument list, and building one
	from six registers plus the caller's stack is the next piece of work. This
	answers a fixed set of calls so that the trap path itself can be tested --
	including from kernel mode, since a `ta` traps the same either way.
*/
extern "C" void
sparc_syscall(struct iframe *frame)
{
	uint64 index = frame->g1;
	bool isUser = (frame->tstate & TSTATE_PRIV) == 0;

	// Past the `ta`. %tnpc is the instruction after it, and on a trap from a
	// delay slot it is not %tpc + 4, which is why it is copied rather than
	// computed.
	frame->tpc = frame->tnpc;
	frame->tnpc = frame->tpc + 4;

	static bool sReported = false;
	if (!sReported) {
		sReported = true;
		dprintf("sparc_syscall: index %" B_PRIu64 ", args %#" B_PRIx64 " %#"
			B_PRIx64 " %#" B_PRIx64 ", from %s, tpc %#" B_PRIx64 "\n", index,
			frame->out[0], frame->out[1], frame->out[2],
			isUser ? "userspace" : "the kernel", frame->tpc);
	}

	switch (index) {
		case SPARC_SYSCALL_TEST_EXIT:
			if (sUserTestActive) {
				sUserTestActive = false;
				sUserTestValue = frame->out[0];

				// Straight back to the test rather than to a userspace with
				// nothing left to do. This abandons the trap return, which is
				// only safe because TRAP_TO_C has already dropped to trap level
				// zero: nothing is owed, and longjmp() puts the stack and the
				// register windows back itself.
				longjmp(sUserTestReturn, 1);
			}
			frame->out[0] = (uint64)B_NOT_SUPPORTED;
			break;

		case SPARC_SYSCALL_TEST_ECHO:
			// Sums its arguments, so a wrong answer distinguishes "the arguments
			// did not arrive" from "the result did not get back".
			frame->out[0] = frame->out[0] + frame->out[1] + frame->out[2]
				+ frame->out[3] + frame->out[4] + frame->out[5];
			break;

		default:
			dprintf("sparc_syscall: unimplemented call %" B_PRIu64 " from %s\n",
				index, isUser ? "userspace" : "the kernel");
			frame->out[0] = (uint64)B_NOT_SUPPORTED;
			break;
	}
}


extern "C" void
sparc_page_fault(struct iframe *frame)
{
	uint64 trap = frame->tt;
	bool isInstructionMiss = (trap & ~3) == TRAP_INSTRUCTION_MMU_MISS;
	bool isDataMiss = (trap & ~3) == TRAP_DATA_MMU_MISS;
	bool isProtection = (trap & ~3) == TRAP_DATA_PROTECTION;
	bool isExecute = trap == TRAP_INSTRUCTION_ACCESS || isInstructionMiss;
	bool isUser = (frame->tstate & TSTATE_PRIV) == 0;

	// Where the address comes from depends on which trap this is. The "fast" MMU
	// traps -- the two misses and the protection trap -- leave it in Tag Access
	// rather than in a fault address register, which is most of what makes them
	// fast.
	addr_t address;
	if (isDataMiss || isInstructionMiss || isProtection) {
		address = (addr_t)(frame->tagAccess & ~(uint64)0x1fff);
	} else if (trap == TRAP_INSTRUCTION_ACCESS) {
		// An instruction fetch has no address register of its own; it faulted on
		// the address it was fetching from.
		address = (addr_t)frame->tpc;
	} else {
		address = (addr_t)frame->sfar;
	}

	// A protection trap is a write by definition -- it is what the hardware
	// raises when a store finds a read-only entry. Otherwise SFSR says, and it
	// is written for TLB misses too: SFSR's fault type has a bit for exactly
	// that case.
	bool isWrite = isProtection || (frame->sfsr & SFSR_WRITE) != 0;

	Thread *thread = thread_get_current_thread();

	// Faulting inside the kernel debugger is not recoverable by paging, and
	// blocking there would hang the machine with everything else stopped. The
	// debugger installs a handler for exactly this.
	if (debug_debugger_running()) {
		if (thread != NULL && thread->fault_handler != NULL) {
			debug_set_page_fault_info(address, (addr_t)frame->tpc,
				isWrite ? DEBUG_PAGE_FAULT_WRITE : 0);
			frame->tpc = (uint64)(addr_t)thread->fault_handler;
			frame->tnpc = frame->tpc + 4;
			return;
		}

		panic("sparc: page fault in the debugger with no fault handler, "
			"address %#" B_PRIxADDR " from pc %#" B_PRIx64, address,
			frame->tpc);
		return;
	}

	// A fault with interrupts already disabled cannot be resolved by blocking,
	// so it had better be a fault somebody was expecting. user_memcpy() and the
	// rest of user_access() set a handler precisely so that a bad user address
	// becomes an error return rather than a dead kernel.
	if ((frame->tstate & TSTATE_IE) == 0) {
		if (thread != NULL && thread->fault_handler != NULL) {
			frame->tpc = (uint64)(addr_t)thread->fault_handler;
			frame->tnpc = frame->tpc + 4;
			return;
		}

		panic("sparc: page fault with interrupts disabled and no fault handler, "
			"address %#" B_PRIxADDR " from pc %#" B_PRIx64 " (trap %#" B_PRIx64
			", sfsr %#" B_PRIx64 ")", address, frame->tpc, frame->tt,
			frame->sfsr);
		return;
	}

	enable_interrupts();

	addr_t newInstructionPointer = 0;
	vm_page_fault(address, (addr_t)frame->tpc, isWrite, isExecute, isUser,
		&newInstructionPointer);

	if (newInstructionPointer != 0) {
		frame->tpc = (uint64)newInstructionPointer;
		frame->tnpc = frame->tpc + 4;
	}

	disable_interrupts();
}


// #pragma mark - the user memory test


/*!	Verifies the shared address space model, and that a bad user address is an
	error rather than a dead kernel.

	The porting plan singles this out as the thing to check early: section 4.3
	chose one address space with kernel mappings marked Global and user mappings
	tagged by context, precisely so that no shared Haiku code needs changing. If
	that model did not hold -- if IS_USER_ADDRESS and IS_KERNEL_ADDRESS could not
	both be simple range checks -- the scope of userspace support would be
	completely different.

	The second half is the page fault handler doing its job. user_access() sets
	thread->fault_handler and longjmps out of a fault, which means a bad address
	has to arrive as a fault the handler recognises. Before there was a handler
	this would have been an unhandled trap and a panic; before setjmp worked it
	would have been worse than that.
*/
void
sparc_test_user_memory()
{
	// The two ranges must not overlap, and each must recognise its own.
	bool ranges = IS_KERNEL_ADDRESS(KERNEL_BASE)
		&& IS_KERNEL_ADDRESS(KERNEL_TOP)
		&& !IS_KERNEL_ADDRESS(USER_BASE)
		&& IS_USER_ADDRESS(USER_BASE)
		&& IS_USER_ADDRESS(USER_TOP)
		&& !IS_USER_ADDRESS(KERNEL_BASE);

	dprintf("sparc_int: address space: user %#lx-%#lx, kernel %#lx-%#lx -- %s\n",
		(addr_t)USER_BASE, (addr_t)USER_TOP, (addr_t)KERNEL_BASE,
		(addr_t)KERNEL_TOP, ranges ? "disjoint" : "WRONG");

	if (!ranges) {
		panic("sparc: the user and kernel address ranges overlap or do not "
			"recognise their own addresses");
		return;
	}

	char buffer[16];
	const char source[16] = "sparc user copy";

	// A copy that must work, so that the failures below mean something. A kernel
	// address is legitimate here: user_memcpy() only refuses a range that
	// *crosses* the user/kernel boundary, not one entirely on either side, and
	// kernel code does use it for kernel-to-kernel copies.
	status_t good = user_memcpy(buffer, source, sizeof(buffer));
	bool copied = good == B_OK && memcmp(buffer, source, sizeof(buffer)) == 0;

	// A user address that is certainly not mapped. Not the low megabyte, which
	// looks unmapped and is not -- the boot loader's identity mapping covers
	// everything below 0x802000, and it survives into the kernel's page table.
	// A gigabyte up is past everything the loader touched and still well below
	// KERNEL_BASE.
	status_t unmapped = user_memcpy(buffer, (const void*)0x40000000,
		sizeof(buffer));

	// And a range straddling the boundary, which has to be refused by the range
	// check rather than by faulting: that check is what stops userland handing
	// the kernel a pointer that starts in its own space and ends in the
	// kernel's.
	status_t straddling = user_memcpy(buffer,
		(const void*)(KERNEL_BASE - sizeof(buffer) / 2), sizeof(buffer));

	bool ok = copied && unmapped == B_BAD_ADDRESS
		&& straddling == B_BAD_ADDRESS;

	dprintf("sparc_int: user_memcpy good %#x, unmapped %#x, straddling %#x "
		"-- %s\n", good, unmapped, straddling,
		ok ? "faults caught" : "WRONG");

	if (!ok) {
		panic("sparc: user_memcpy gave %#x for a valid copy, %#x for an "
			"unmapped user address and %#x for a straddling range; wanted OK, "
			"B_BAD_ADDRESS, B_BAD_ADDRESS", good, unmapped, straddling);
	}
}
