/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*	A userland, in the smallest form that is still a real one.
 *
 *	Freestanding: no libc, no libroot, no runtime_loader, no dynamic linking. The
 *	kernel's ELF loader sees a static executable with no interpreter and jumps
 *	straight to _start, which is what makes this a test of the kernel rather than
 *	a test of the userland's own bootstrap.
 *
 *	That is the point. Three things in the port have been built and never
 *	exercised, because exercising them needs user code the kernel loaded rather
 *	than a blob the kernel copied into a page: loading a user ELF at all,
 *	arch_thread_enter_userspace() for a real thread with a real stack and a real
 *	address space, and the window machinery under a program nobody hand-assembled.
 *
 *	It reports through _kern_debug_output(), because that lands on the same serial
 *	console as the kernel's own output -- so success is a line in the boot log
 *	rather than something that has to be inferred from an exit status.
 *
 *	The system call convention is the one in <arch_syscall_defs.h> and libroot's
 *	syscalls.inc: index in %g1, arguments in %o0 onwards, `ta`.
 */

#define SYSCALL_TRAP			0x40
#define SYSCALL_EXIT_TEAM		41
#define SYSCALL_DEBUG_OUTPUT		254

#define FRAME				176


/*	The message is built on the stack rather than kept in .rodata, and that is
 *	not a stylistic choice.
 *
 *	Haiku executables are ET_DYN -- the toolchain produces nothing else -- so the
 *	kernel loads this at a base of its own choosing. A `sethi` of a data label
 *	would need a relocation, and nothing applies relocations to a static image
 *	with no interpreter. Storing the bytes as immediates needs no address at all,
 *	so the program works wherever it lands.
 *
 *	Big-endian, so `stx` of 0x7573657274657374 puts 'u' at the lowest address.
 */
#define MESSAGE_HIGH			0x7573657274657374	/* "usertest" */
#define MESSAGE_LOW			0x206f6b0a00000000	/* " ok\n\0"    */

/*	Scratch inside this frame, past the sixteen words a spill of this window
 *	writes -- so building the message cannot be undone by the spill that the
 *	nesting below provokes.
 */
#define SCRATCH				(BIAS + 128)
#define BIAS				2047


/*	%g2 and %g3 are the SPARC V9 application globals, so the assembler wants them
 *	declared before use. Scratch is exactly what they are here: this program has
 *	no other code to share them with.
 */
	.register %g2, #scratch
	.register %g3, #scratch


	.text
	.global _start
	.type _start, %function
_start:
	/*	Nest deep enough to spill, before saying anything.
	 *
	 *	sparc_enter_userspace() leaves CANSAVE at NWINDOWS-2, so the seventh save
	 *	has nowhere to go and traps -- into the handler that stores through
	 *	ASI_AS_IF_USER_PRIMARY, to this stack, with this program's privilege. Eight
	 *	to be sure of crossing it, and eight restores to bring them back.
	 *
	 *	Done first so that if the window machinery is wrong, it is wrong before
	 *	anything has been printed, and the log says so by staying silent.
	 */
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp
	save	%sp, -FRAME, %sp

	restore
	restore
	restore
	restore
	restore
	restore
	restore
	restore

	/*	Say so, on the same serial console the kernel prints to. Reached only if
		all eight spills and fills above worked, which is why nothing is printed
		before them.
	 */
	setx	MESSAGE_HIGH, %g2, %g3
	stx	%g3, [%sp + SCRATCH]
	setx	MESSAGE_LOW, %g2, %g3
	stx	%g3, [%sp + SCRATCH + 8]

	add	%sp, SCRATCH, %o0
	set	SYSCALL_DEBUG_OUTPUT, %g1
	ta	SYSCALL_TRAP

	/* And out. _kern_exit_team() does not return. */
	clr	%o0
	set	SYSCALL_EXIT_TEAM, %g1
	ta	SYSCALL_TRAP

	/*	Not reached. Spinning rather than falling into whatever the linker put
		next, so a failure to exit looks like a hang at a known address instead of
		executing rodata.
	 */
1:	ba,a	1b
	 nop
	.size _start, . - _start
