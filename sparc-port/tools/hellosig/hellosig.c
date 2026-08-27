/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*	Does a system call resume after a signal interrupts it?
 *
 *	Phase 6 implemented syscall restart and nothing has ever exercised it. The
 *	kernel side is `THREAD_FLAGS_RESTART_SYSCALL` in sparc_kernel_exit(), which
 *	rewinds %tpc to the trap instruction and puts the first argument back --
 *	because a system call's arguments live in the out registers and the handler
 *	that ran in between was entitled to every one of them.
 *
 *	It could not be tested before this port had a userland, because arranging the
 *	situation needs one: a thread has to be *blocked* in an interruptible call at
 *	the moment a signal arrives, which needs a signal that arrives on its own
 *	schedule rather than one the program raises against itself.
 *
 *	The arrangement:
 *
 *	  - a SIGALRM handler installed with SA_RESTART, which is what asks for the
 *	    call to be resumed rather than failed;
 *	  - an alarm half a second out;
 *	  - a semaphore nobody will ever release, acquired with a two-second relative
 *	    timeout.
 *
 *	The two outcomes are far apart, which is the point of choosing these numbers:
 *
 *	  restart works    the handler runs at 0.5s, the call resumes, and it returns
 *	                   Operation timed out at about 2s
 *	  restart is lost  the call returns Interrupted at about 0.5s
 *
 *	Both the status and the elapsed time say which happened, and they cannot both
 *	be wrong in the same direction.
 *
 *	**The elapsed time is the real assertion**, not the status. `acquire_sem_etc`
 *	is written against `syscall_restart_handle_timeout_pre()`, which converts a
 *	relative timeout to an absolute deadline on the first entry and reuses the
 *	stored deadline on every restart. So a correct restart does *not* start the
 *	two seconds again -- a total near 2.0s means the deadline survived the trip
 *	through the signal handler, and a total near 2.5s would mean the call
 *	restarted but forgot when it was supposed to end.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <OS.h>


extern void _kern_debug_output(const char* message);

/*	The clock, as a syscall.
 *
 *	libroot's system_time() reads the commpage, and on this port it returns zero
 *	from userspace -- which made the first run of this test report a two-second
 *	wait as having taken 0us. That is its own bug and worth chasing separately;
 *	this program needs a clock it can trust, and the syscall behind the commpage
 *	is one.
 */
extern bigtime_t _kern_system_time(void);


#define ALARM_DELAY		500000		// half a second
#define WAIT_TIMEOUT	2000000		// two seconds


static volatile int32 sHandlerRuns = 0;


static void
say(const char* format, ...)
{
	char buffer[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(buffer, sizeof(buffer) - 2, format, arguments);
	va_end(arguments);

	strlcat(buffer, "\n", sizeof(buffer));
	_kern_debug_output(buffer);
}


/*!	Deliberately does nothing but count.

	A handler that called anything interesting would be testing that instead. All
	this one has to prove is that it ran, and when.
*/
static void
alarm_handler(int signal)
{
	sHandlerRuns++;
}


int
main(int argc, char** argv)
{
	struct sigaction action;
	sem_id semaphore;
	bigtime_t start;
	bigtime_t elapsed;
	status_t status;

	say("hellosig: start");

	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	action.sa_flags = SA_RESTART;
	if (sigaction(SIGALRM, &action, NULL) != 0) {
		say("hellosig: sigaction failed: %s", strerror(errno));
		return 1;
	}

	semaphore = create_sem(0, "hellosig wait");
	if (semaphore < 0) {
		say("hellosig: create_sem failed: %s", strerror(semaphore));
		return 1;
	}

	say("hellosig: alarm in %dus, then a %dus wait", ALARM_DELAY, WAIT_TIMEOUT);

	start = _kern_system_time();
	set_alarm(ALARM_DELAY, B_ONE_SHOT_RELATIVE_ALARM);

	/*	B_CAN_INTERRUPT is what makes this the interesting case. Without it the
		call is not interruptible, the signal waits until it returns, and nothing
		about restart is exercised.
	 */
	status = acquire_sem_etc(semaphore, 1,
		B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, WAIT_TIMEOUT);
	elapsed = _kern_system_time() - start;

	delete_sem(semaphore);

	say("hellosig: handler ran %d time(s), wait returned %s after %dus",
		(int)sHandlerRuns, strerror(status), (int)elapsed);

	if (sHandlerRuns == 0) {
		say("hellosig: the alarm never arrived -- nothing was tested");
		return 1;
	}

	if (status == B_INTERRUPTED) {
		say("hellosig: NOT restarted -- the call gave up when the signal came");
		return 1;
	}

	if (status != B_TIMED_OUT) {
		say("hellosig: unexpected status; the semaphore should never be "
			"released");
		return 1;
	}

	/*	Generous, because this is a timing assertion on an emulator: anything
		below the alarm plus a margin means the call did not really resume, and
		anything far above the original timeout means it resumed with a fresh
		deadline instead of the one it started with.
	 */
	if (elapsed < WAIT_TIMEOUT - 100000) {
		say("hellosig: returned too early to have resumed");
		return 1;
	}
	if (elapsed > WAIT_TIMEOUT + ALARM_DELAY) {
		say("hellosig: resumed, but the timeout restarted with it");
		return 1;
	}

	say("hellosig ok");
	return 0;
}
