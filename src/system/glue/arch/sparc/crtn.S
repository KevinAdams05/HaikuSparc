/*
 * Copyright 2019, Adrien Destugues, pulkomandy@pulkomandy.tk
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


/**	This file contains the final part of the ".init" and ".fini" sections in
 *	the ELF executable. It is tightly connected to crti.S.
 *	Have a look at crti.S to find a description of what happens here.
 */


/*	The other half of each function crti.S opened.
 *
 *	`ret` is `jmpl %i7 + 8, %g0` -- the return address of *this* window, which is
 *	the one crti.S's `save` created. `restore` in the delay slot gives it back,
 *	so the two instructions are the exact counterpart of that one and have to
 *	stay paired with it: a `save` closed by anything else would return with the
 *	caller's window still hidden.
 */
.section .init
	ret
	 restore

.section .fini
	ret
	 restore
