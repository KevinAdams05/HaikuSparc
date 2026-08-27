/*
 * Copyright 2019, Haiku Inc. All rights reserved.
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Adrien Destugues <pulkomandy@pulkomandy.tk>
 */


#include <string.h>

#include <arch/timer.h>
#include <debug.h>
#include <kernel.h>
#include <platform/openfirmware/openfirmware.h>
#include <timer.h>


/*	The %TICK register counts elapsed CPU clock cycles in bits 62:0, with bit 63
	being NPT, the non-privileged trap enable (UltraSPARC-IIi manual TABLE 14-1,
	printed p.186). Masking it off is not optional: NPT is set out of reset, so
	the raw register reads as an enormous number that only looks like a plausible
	timestamp until something compares two of them.
*/
#define TICK_COUNTER_MASK	0x7fffffffffffffffULL

// Cycles per second, from Open Firmware. Zero until arch_init_timer() runs, and
// system_time() returns zero while it is -- callers before the timer exists get
// a clock that has not started rather than a division by zero.
static uint64 sClockFrequency;


static inline uint64
sparc_read_tick()
{
	uint64 tick;
	asm volatile("rd %%tick, %0" : "=r"(tick));
	return tick & TICK_COUNTER_MASK;
}


/*!	Finds the CPU node's clock-frequency property.

	Open Firmware describes each CPU as a node with device_type "cpu", and its
	clock-frequency is what %TICK counts in. Walking to it rather than reading a
	fixed path because the path differs between machines -- /SUNW,UltraSPARC-IIi
	on an Ultra 10, something else on a Blade -- while the device_type does not.

	The search is one level deep from the root, which is where every sun4u
	workstation puts its processors. A machine that nests them deeper would need
	a recursive walk; there is no point writing one before there is a machine
	that needs it.
*/
static uint64
sparc_find_clock_frequency()
{
	intptr_t root = of_finddevice("/");
	if (root == OF_FAILED)
		return 0;

	for (intptr_t node = of_child(root); node != 0 && node != OF_FAILED;
			node = of_peer(node)) {
		char type[16];
		if (of_getprop(node, "device_type", type, sizeof(type)) == OF_FAILED)
			continue;
		if (strcmp(type, "cpu") != 0)
			continue;

		// A 32-bit big-endian value, as Open Firmware properties are.
		uint32 frequency;
		if (of_getprop(node, "clock-frequency", &frequency, sizeof(frequency))
				== OF_FAILED) {
			continue;
		}

		return frequency;
	}

	return 0;
}


/*!	Microseconds since the counter started.

	Two divisions rather than one, because the obvious form overflows: %TICK
	reaches 2^62 and multiplying that by a million does not fit. Splitting the
	count into whole seconds and a remainder keeps every intermediate in range --
	the remainder is below the clock frequency, so scaling it by a million stays
	well inside 64 bits -- and the result is exact rather than approximated by a
	fixed-point reciprocal.

	Two 64-bit divides is not cheap, and system_time() is called often. Whether
	that matters is a question for a profile rather than for a guess, and an exact
	clock is the right thing to be optimising away from later.
*/
bigtime_t
system_time(void)
{
	if (sClockFrequency == 0)
		return 0;

	uint64 tick = sparc_read_tick();

	return (tick / sClockFrequency) * 1000000
		+ ((tick % sClockFrequency) * 1000000) / sClockFrequency;
}


/*!	Checks what can be checked about the clock from inside the kernel.

	Not against Open Firmware's "milliseconds", which was the obvious idea and is
	wrong: under QEMU that clock runs about eleven times fast. Twenty samples each
	claiming to have waited 101 firmware milliseconds arrived 17 milliseconds
	apart by the host's wall clock, while system_time() reported 8.9 ms for the
	same interval -- so %TICK at the advertised frequency is right and the
	firmware's clock is not. See PROGRESS.md section 24; that measurement is the
	reason this function does not compare against it.

	What is left is worth having anyway, because it catches the three ways this
	can be wrong:

	The absolute value catches a %TICK read that kept bit 63. NPT is set out of
	reset, so failing to mask it makes system_time() return something around
	thirty million years, which is obviously wrong the moment anyone looks and
	silently enormous if nobody does.

	Monotonicity catches a clock that wraps or jitters backwards, which would make
	every timeout in the kernel occasionally enormous.

	And the elapsed time against the raw %TICK delta catches the division: the two
	are computed by different routes, so agreement means the frequency and the
	two-step arithmetic are consistent with the counter they came from.
*/
static void
sparc_verify_system_time()
{
	bigtime_t now = system_time();

	// Nothing has taken an hour to get here.
	if (now < 0 || now > 3600 * 1000000LL) {
		panic("arch_timer: system_time() is %" B_PRIdBIGTIME " us at boot; "
			"%%TICK bit 63 is NPT and has to be masked off", now);
		return;
	}

	uint64 startTick = sparc_read_tick();
	bigtime_t startTime = system_time();
	bigtime_t previous = startTime;

	for (int i = 0; i < 200000; i++) {
		bigtime_t sample = system_time();
		if (sample < previous) {
			panic("arch_timer: system_time() went backwards, %" B_PRIdBIGTIME
				" then %" B_PRIdBIGTIME, previous, sample);
			return;
		}
		previous = sample;
	}

	uint64 tickDelta = sparc_read_tick() - startTick;
	bigtime_t elapsed = previous - startTime;
	bigtime_t fromTicks = tickDelta / (sClockFrequency / 1000000);

	// A few microseconds apart at most: the two %TICK reads bracket the two
	// system_time() calls rather than coinciding with them.
	bigtime_t difference = elapsed > fromTicks
		? elapsed - fromTicks : fromTicks - elapsed;
	bool ok = elapsed > 0 && difference < 100;

	dprintf("arch_timer: %" B_PRIdBIGTIME " us elapsed, %" B_PRIdBIGTIME
		" us of %%TICK -- %s\n", elapsed, fromTicks,
		ok ? "consistent" : "WRONG");

	if (!ok) {
		panic("arch_timer: system_time() says %" B_PRIdBIGTIME " us where %%TICK "
			"says %" B_PRIdBIGTIME, elapsed, fromTicks);
	}
}


/*!	Arms the %TICK comparator.

	TICK_CMPR is bit 63 INT_DIS and bits 62:0 a compare value, and the hardware
	posts TICK_INT when %TICK's counter *equals* it (manual TABLE 14-11, printed
	p.199). Equality, not "greater than", which is the whole hazard here: a
	comparator set to a value the counter has already passed does not fire late,
	it fires in about three thousand years.

	So the timeout has a floor, and after arming, the comparator is checked
	against the counter once more. If the counter got there first the interrupt
	is posted by hand, which is exactly what the hardware would have done.
*/
void
arch_timer_set_hardware_timer(bigtime_t timeout)
{
	const uint64 kMinimumTicks = 1000;

	uint64 ticks = (uint64)timeout * (sClockFrequency / 1000000);
	if (ticks < kMinimumTicks)
		ticks = kMinimumTicks;

	uint64 target = (sparc_read_tick() + ticks) & TICK_COUNTER_MASK;

	// INT_DIS clear, so the comparison is live.
	asm volatile("wr %0, 0, %%asr23" : : "r"(target));

	// Both values are well inside the counter's range, so the difference is a
	// small signed number and the comparison needs no wraparound reasoning.
	if ((int64)(target - sparc_read_tick()) <= 0) {
		// SET_SOFTINT ORs into SOFTINT, and bit 0 is TICK_INT.
		asm volatile("wr %0, 0, %%asr20" : : "r"(1ULL));
	}
}


void
arch_timer_clear_hardware_timer()
{
	// Bit 63 is INT_DIS. Setting it stops the comparison without having to
	// choose a compare value that will never be reached.
	asm volatile("wr %0, 0, %%asr23" : : "r"(1ULL << 63));
}


/*!	Cycles per second, for the one other file that needs it.

	arch_rtc_init() turns it into the factor userspace multiplies %TICK by, and
	nothing else outside this file has any business with it -- which is why it is
	an accessor rather than a shared variable.
*/
uint64
sparc_clock_frequency()
{
	return sClockFrequency;
}


/*!	Lets userspace read %TICK.

	Bit 63 is NPT, and "if set, an attempt by non-privileged software to read the
	TICK register causes a privileged_action trap" -- and it "is set ... after
	both a Power-On-Reset (POR) and an Externally Initiated Reset (XIR)"
	(UltraSPARC-IIi manual TABLE 14-1, printed p.186). So out of reset the
	register is privileged, and stays that way until something clears the bit.

	Which is why libroot's system_time() could not read it and shipped a fake
	counter instead. The whole of that file was a placeholder: an incrementing
	static, multiplied by a conversion factor nothing ever set, which returned
	zero forever.

	Read-modify-write rather than a plain store, because bits 62:0 are the counter
	and writing zero to them would restart time. The few cycles that pass between
	the read and the write are lost from the count, which is the only cost and is
	measured in nanoseconds.

	Privileged software only, per the same table -- so this belongs here and not
	in a commpage routine.
*/
static void
sparc_allow_user_tick_reads()
{
	uint64 tick;
	asm volatile("rdpr %%tick, %0" : "=r"(tick));
	tick &= TICK_COUNTER_MASK;
	asm volatile("wrpr %0, 0, %%tick" : : "r"(tick));
}


int
arch_init_timer(kernel_args *args)
{
	sClockFrequency = sparc_find_clock_frequency();
	if (sClockFrequency == 0) {
		panic("arch_init_timer: no cpu node with a clock-frequency property; "
			"system_time() would stand still");
		return B_ERROR;
	}

	dprintf("arch_timer: %" B_PRIu64 " Hz, %" B_PRIu64 " ticks per microsecond\n",
		sClockFrequency, sClockFrequency / 1000000);

	sparc_verify_system_time();
	sparc_allow_user_tick_reads();

	return B_OK;
}
