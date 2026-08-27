#include <arch/real_time_clock.h>

#include <KernelExport.h>

#include <real_time_clock.h>
#include <real_time_data.h>


extern uint64 sparc_clock_frequency();


/*!	Publishes what userspace needs to turn %TICK into microseconds.

	libroot's system_time() reads the cycle counter itself rather than making a
	system call -- that is the whole point of the commpage -- and the only thing
	it cannot work out for itself is how fast the counter runs. This is where the
	kernel tells it.

	The factor is scaled by 2^32, because the arithmetic on the other side is

	    microseconds = (ticks * factor) >> 32

	split into two halves so that a 64-bit counter times a 32-bit factor does not
	overflow. So the factor is 10^6 * 2^32 / frequency: on the 100 MHz part QEMU
	models that is 42949673, and one part in 4 * 10^7 is lost to the rounding,
	which is two microseconds every hundred seconds.

	Called after timer_init(), which is where the frequency is read from Open
	Firmware -- main.cpp runs them in that order, and this would publish a
	division by zero if it did not.
*/
status_t
arch_rtc_init(kernel_args *args, struct real_time_data *data)
{
	uint64 frequency = sparc_clock_frequency();
	if (frequency == 0) {
		panic("arch_rtc_init: the timer has no frequency yet");
		return B_ERROR;
	}

	data->arch_data.system_time_conversion_factor
		= (uint32)((1000000ULL << 32) / frequency);

	dprintf("arch_rtc: %" B_PRIu64 " Hz, userspace conversion factor %"
		B_PRIu32 "\n", frequency,
		data->arch_data.system_time_conversion_factor);

	return B_OK;
}


uint64
arch_rtc_get_hw_time(void)
{
	return 0;
}


void
arch_rtc_set_hw_time(uint64 seconds)
{
}


void
arch_rtc_set_system_time_offset(struct real_time_data *data, bigtime_t offset)
{
}


bigtime_t
arch_rtc_get_system_time_offset(struct real_time_data *data)
{
	return 0;
}
