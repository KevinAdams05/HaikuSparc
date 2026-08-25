/*
   Copyright 2019, Adrien Destugues <pulkomandy@pulkomandy.tk>
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


/**	This file contains the first part of the ".init" and ".fini" sections in
 *	the ELF executable.
 *	The functions defined here will be called during initialization/termination
 *	of the loaded executable/library. The ".init" and ".fini" sections are
 *	stacked together like this:
 *
 *	crti.S		entry point
 *	crtbegin.S	GCC specific: constructors/destructors are called, ...
 *	crtend.S
 *	crtn.S		call to _init_after/_term_after
 *				exit
 *
 *	The point of the arrangement is that no one file contains a whole function:
 *	this one opens the two sections and the matching crtn.S closes them, and
 *	whatever the compiler contributes is linked in between. So the `save` below
 *	has its `restore` in the other file, and neither half assembles into anything
 *	that looks like a complete function on its own.
 */


/*	176 bytes because that is what SPARC V9 requires of every stack frame: a
 *	sixteen-register window save area and six argument slots, all eight bytes
 *	wide. Not a constant this file can include from anywhere -- the glue is built
 *	without the kernel's headers -- and the same number appears as
 *	SPARC_MINIMUM_FRAME_SIZE in arch_thread_types.h.
 */
#define FRAME_SIZE	176


/*	These two labels used to be declared and never defined.
 *
 *	`.global _init` and `.type _init, #function` announce a symbol; only `_init:`
 *	creates one. Without it the section was emitted, the compiler's constructor
 *	code was linked into it, and the symbol naming its entry point stayed
 *	undefined -- so the linker wrote DT_INIT as zero rather than omitting it.
 *
 *	The runtime loader adds the image's load address to DT_INIT, and zero plus a
 *	load address is the first byte of the image. So every program calling
 *	libroot's initialiser jumped to the ELF header: `\x7fELF` has its top two bits
 *	set to one and zero, which is the encoding of `call`, and the byte pair after
 *	it decodes as an illegal instruction. The failure was an illegal-instruction
 *	trap four bytes into a mapping, in a program that had linked and relocated
 *	perfectly.
 */
.section .init
.align 4
.global _init
.type _init, #function
_init:
	save	%sp, -FRAME_SIZE, %sp
	// crtbegin.o stuff comes here

.section .fini
.align 4
.global _fini
.type _fini, #function
_fini:
	save	%sp, -FRAME_SIZE, %sp
	// crtend.o stuff comes here
