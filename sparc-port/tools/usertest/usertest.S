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
#define SYSCALL_SEND_SIGNAL		91
#define SYSCALL_SIGACTION		93
#define SYSCALL_DEBUG_OUTPUT		254

#define FRAME				176

/*	Roomier than the minimum, so that scratch past the sixteen-word save area
 *	still fits inside the frame. 176 leaves 48 bytes there; 256 leaves 128.
 */
#define SIGNAL_FRAME			256

/*	Deep enough to cross several 8 KB stack pages: DEPTH frames of FRAME bytes
 *	is about 53 KB, which is six pages and change. Well inside the stack area
 *	Haiku gives a main thread, and far past the one page the kernel touched
 *	setting the program up.
 */
#define DEPTH				300

/*	struct sigaction, from <signal.h>, laid out for a 64-bit big-endian target:
 *	an 8-byte handler pointer, an 8-byte sigset_t mask, a 4-byte flag word with
 *	4 bytes of padding behind it, and an 8-byte userdata pointer.
 */
#define SIGACTION_HANDLER		0
#define SIGACTION_MASK			8
#define SIGACTION_FLAGS			16
#define SIGACTION_USERDATA		24
#define SIGACTION_SIZE			32

#define SIGUSR1				18


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

#define SIGNAL_HIGH			0x7573657274657374	/* "usertest" */
#define SIGNAL_LOW			0x207369670a000000	/* " sig\n\0"   */

/*	Scratch inside this frame, past the sixteen words a spill of this window
 *	writes -- so building the message cannot be undone by the spill that the
 *	nesting below provokes.
 */
#define SCRATCH				(BIAS + 128)
#define BIAS				2047


/*	%g2 and %g3 are the SPARC V9 application globals, so the assembler wants them
 *	declared before use. Scratch is exactly what they are here: this program has
 *	no other code to share them with. %g4 needs no such declaration -- .register
 *	only accepts %g2, %g3, %g6 and %g7 -- and needs no protection either: a trap
 *	runs on the alternate global bank, so the window handlers' scratch registers
 *	are a different register file from this program's.
 */
	.register %g2, #scratch
	.register %g3, #scratch


	.text

	/*	Not decoration. This section carries no alignment of its own -- gas gives
	 *	a bare `.text` in a freestanding file sh_addralign 1 -- so the linker puts
	 *	it wherever the section before it ends, and the section before it is
	 *	`.dynstr`, whose length is the length of the symbol names.
	 *
	 *	Which means the entry point's alignment depends on how many characters are
	 *	in this program's symbols. The first version of it landed on 0x1a8 and ran;
	 *	adding one symbol moved it to 0x1cf, and an instruction fetch from an
	 *	address that is not a multiple of four is trap 0x34, on the first
	 *	instruction, before anything can report anything.
	 */
	.align	8

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

	/*	Then nest far enough that the spills land on stack pages that do not
	 *	exist yet, which is the case the kernel could not survive until winfixup.
	 *
	 *	Nothing here writes to the stack. A `save` only moves the stack pointer, so
	 *	the *first* thing to touch each new page is the spill handler's own store
	 *	-- at trap level one, where a fault nests one deeper and cannot be paged in
	 *	where it happens. Which is the entire point: a program does not have to do
	 *	anything unusual to provoke it, only recurse on a stack it has not used
	 *	before, and every program does that.
	 *
	 *	DEPTH frames of FRAME bytes crosses several 8 KB pages, so this is not one
	 *	fixup but a run of them, and the unwind exercises the fill side against the
	 *	pages the spills created.
	 *
	 *	The counter is a global because it has to outlive the window: %l and %i
	 *	registers are what `save` and `restore` rotate, so a loop counter in one
	 *	would be a different register on every iteration.
	 */
	set	DEPTH, %g4
1:	save	%sp, -FRAME, %sp
	deccc	%g4
	bne	1b
	 nop

	set	DEPTH, %g4
2:	restore
	deccc	%g4
	bne	2b
	 nop

	/*	Then a signal, to itself, with a handler of its own.
	 *
	 *	This is the piece the kernel could not test on its own: nothing registers a
	 *	handler until there is a userland, so the signal frame, the commpage
	 *	trampoline and _kern_restore_signal_frame() were all written and never run.
	 *	Sending one here exercises the whole path -- delivery on the way out of a
	 *	trap, the handler entered at an address the kernel computed, and the return
	 *	landing back on the instruction after the call that raised it.
	 *
	 *	Deliberately raised BEFORE the success message. If the handler runs but the
	 *	return does not, the log says "usertest sig" and stops -- which is a
	 *	different and much more useful failure than silence.
	 */
	/*	In a frame of its own, and not by shifting %sp by the size of the struct.
	 *
	 *	Scratch has to live past the sixteen words at %sp + BIAS, because that is
	 *	where a window spill writes -- and both calls below can provoke one. Moving
	 *	%sp down by 32 bytes would put the struct exactly in the region the next
	 *	spill overwrites, which works right up until the window state happens to
	 *	be one save short.
	 */
	save	%sp, -SIGNAL_FRAME, %sp

	/*	The handler's address, computed rather than named. `call` records the
	 *	address of the call instruction itself in %o7, so the label two
	 *	instructions later is %o7 + 8 -- and the difference between two labels in
	 *	one section is a constant the assembler folds, needing no relocation. Which
	 *	is the whole constraint this program runs under.
	 */
	call	1f
	 nop
1:	add	%o7, signal_handler - 1b + 8, %g3
	stx	%g3, [%sp + SCRATCH + SIGACTION_HANDLER]
	stx	%g0, [%sp + SCRATCH + SIGACTION_MASK]
	st	%g0, [%sp + SCRATCH + SIGACTION_FLAGS]
	stx	%g0, [%sp + SCRATCH + SIGACTION_USERDATA]

	mov	SIGUSR1, %o0
	add	%sp, SCRATCH, %o1
	clr	%o2
	set	SYSCALL_SIGACTION, %g1
	ta	SYSCALL_TRAP

	/*	And raise it. Team zero is this one, so kill() semantics deliver it here;
	 *	no thread id is needed and none can be obtained without another call.
	 */
	clr	%o0
	mov	SIGUSR1, %o1
	clr	%o2
	clr	%o3
	set	SYSCALL_SEND_SIGNAL, %g1
	ta	SYSCALL_TRAP

	restore

	/*	Say so, on the same serial console the kernel prints to. Reached only if
		all eight spills and fills above worked, and the signal went out and came
		back, which is why nothing is printed before them.
	 */
	setx	MESSAGE_HIGH, %g2, %g3
	stx	%g3, [%sp + SCRATCH]
	setx	MESSAGE_LOW, %g2, %g3
	stx	%g3, [%sp + SCRATCH + 8]

	add	%sp, SCRATCH, %o0
	set	SYSCALL_DEBUG_OUTPUT, %g1
	ta	SYSCALL_TRAP

	/*	And out. _kern_exit_team() does not return.
	 *
	 *	The boot log then says
	 *
	 *	  error starting "/boot/system/servers/launch_daemon" error = -1
	 *
	 *	and that is this program being honest about what it is. load_image() waits
	 *	on a condition the loaded image signals once it has finished loading the
	 *	program, and team_loading_info::result starts at B_ERROR. The real
	 *	runtime_loader signals it; this one exits instead, so the launcher is told
	 *	the load failed -- which it did, in the sense that no program was loaded.
	 *
	 *	It appears only when the exit wins the race against the launcher, so it
	 *	comes and goes with how long this program takes.
	 */
	clr	%o0
	set	SYSCALL_EXIT_TEAM, %g1
	ta	SYSCALL_TRAP

	/*	Not reached. Spinning rather than falling into whatever the linker put
		next, so a failure to exit looks like a hang at a known address instead of
		executing rodata.
	 */
2:	ba,a	2b
	 nop
	.size _start, . - _start


/*	The signal handler: void signal_handler(int signal, void* userData,
 *	vregs* registers).
 *
 *	An ordinary C-convention function, because that is how the commpage
 *	trampoline calls it -- three arguments in %o0-%o2, its own frame to make. It
 *	prints and returns; the interesting part is entirely in what has to work for
 *	those two things to happen.
 *
 *	It does `save` deliberately rather than running as a leaf. A leaf would prove
 *	less: the kernel has to have handed the trampoline a stack pointer that a
 *	`save` beneath it can use, and if it has not then the fault is here rather
 *	than somewhere later and harder to attribute.
 */
	.global signal_handler
	.type signal_handler, %function
signal_handler:
	save	%sp, -FRAME, %sp

	setx	SIGNAL_HIGH, %g2, %g3
	stx	%g3, [%sp + SCRATCH]
	setx	SIGNAL_LOW, %g2, %g3
	stx	%g3, [%sp + SCRATCH + 8]

	add	%sp, SCRATCH, %o0
	set	SYSCALL_DEBUG_OUTPUT, %g1
	ta	SYSCALL_TRAP

	/*	Back to the trampoline, which traps to _kern_restore_signal_frame() and
	 *	does not return. `ret; restore` is the ordinary pair: the return address
	 *	is this window's %i7, and the restore gives the trampoline its own window
	 *	back.
	 */
	ret
	 restore
	.size signal_handler, . - signal_handler
