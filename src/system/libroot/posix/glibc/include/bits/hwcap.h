/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _BITS_HWCAP_H
#define _BITS_HWCAP_H	1

/*	Hardware capability bits -- which Haiku does not have, and this file exists to
	say so in the one place that asks.

	glibc's SPARC `sysdep.h` includes <bits/hwcap.h> unconditionally, because
	upstream glibc dispatches between several implementations of the same routine
	at load time -- VIS against no VIS, one multiply against another -- by reading
	AT_HWCAP out of the auxiliary vector. That machinery is ifunc resolvers and a
	populated auxv, and Haiku's import of glibc carries neither: nothing in this
	tree references an HWCAP_SPARC_* constant, on any architecture.

	So the include had nothing to find and every file reaching it failed to
	compile -- the seven multi-precision assembly routines the string and stdio
	code is built on, which is why the failure presented as posix_string.o rather
	than as anything to do with capabilities.

	**Deliberately defines no capability constants.** Providing glibc's list would
	compile, and would let a future ifunc resolver test a bit against a value this
	platform never computes, which reads as a supported feature and behaves as an
	absent one. An undefined constant is a build error naming the file that wants
	it, which is the failure worth having.
*/

#endif /* bits/hwcap.h */
