/*
 * Copyright 2012, Haiku, Inc.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		François Revol <revol@free.fr>
 */

#include <OS.h>

#include <arch_cpu.h>
#include <libroot_private.h>
#include <real_time_data.h>

static vuint32 *sConversionFactor;


/*!	The elapsed cycle counter, read without entering the kernel.

	%TICK counts CPU clock cycles in bits 62:0. Bit 63 is NPT, which is a control
	bit rather than part of the count and reads back as whatever the kernel left
	there -- masking it off is not optional, and a read that keeps it is an
	enormous number that looks like a plausible timestamp until two of them are
	compared.

	Reading it from here at all depends on the kernel having cleared NPT, because
	"if set, an attempt by non-privileged software to read the TICK register
	causes a privileged_action trap", and it is set out of reset
	(UltraSPARC-IIi manual TABLE 14-1, printed p.186). arch_init_timer() clears
	it.

	This replaces a placeholder that returned an incrementing static -- the file
	said so: "XXX: this is a hack / remove me when platform code works". With the
	conversion factor never set either, system_time() returned zero for the whole
	life of the port, and the first thing to notice was a test that measured a
	two-second wait as having taken none.
*/
static inline uint64
__sparc_get_time_base(void)
{
	uint64 tick;
	__asm__ __volatile__("rd %%tick, %0" : "=r"(tick));
	return tick & 0x7fffffffffffffffULL;
}


void
__sparc_setup_system_time(vuint32 *cvFactor)
{
	sConversionFactor = cvFactor;
}


/*!	Microseconds since the kernel started counting.

	The factor is scaled by 2^32 and the multiply is split in half so that a
	64-bit counter times a 32-bit factor cannot overflow: the result is
	(timeBase * factor) >> 32, computed as the high half plus the carry out of
	the low one. arch_rtc_init() is where the factor comes from.

	Zero while the pointer is null, which is the window before __arch_init_time()
	has run rather than a failure -- and is what the whole file used to return.
*/
bigtime_t
system_time(void)
{
	uint64 timeBase = __sparc_get_time_base();

	uint32 cv = sConversionFactor ? *sConversionFactor : 0;
	return (timeBase >> 32) * cv + (((timeBase & 0xffffffff) * cv) >> 32);
}
