/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ARCH_SETJMP_H_
#define _ARCH_SETJMP_H_

/*	Two words are used and four are reserved.
 *
 *	SPARC needs less here than most architectures, because setjmp() executes
 *	flushw and every window that matters is then in the stack where it belongs.
 *	What has to be recorded is only where that stack is and where to resume:
 *	the callee-saved registers are the window registers, and they save
 *	themselves.
 *
 *	  [0] the caller's stack pointer, biased as %sp always is
 *	  [1] the return address, as %o7 held it -- the call site, so the resume
 *	      point is that plus eight
 *
 *	This was one word, which is not enough for either of them, with a comment
 *	saying the size had yet to be determined.
 */
#define _SETJMP_BUF_SZ	4

typedef unsigned long __jmp_buf[_SETJMP_BUF_SZ];

#endif	/* _ARCH_SETJMP_H_ */
