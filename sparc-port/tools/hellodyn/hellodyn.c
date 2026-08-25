/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*	The other kind of userland: dynamically linked, against the real libroot.
 *
 *	tools/usertest is freestanding on purpose -- it tests the kernel with nothing
 *	else in the way. This one tests everything that comes after: the kernel loads
 *	runtime_loader, runtime_loader relocates itself, loads libroot.so, relocates
 *	that, resolves this program's imports against it, and only then is there a
 *	main() to call.
 *
 *	Which makes it the first thing on this architecture to exercise
 *	arch_relocate_image(), the PLT, the GOT, and libroot's C library at once. It
 *	deliberately calls into three different corners of that library -- formatted
 *	output, string handling, and the standard streams -- because a relocation
 *	table that is subtly wrong tends to work for whatever was called first.
 */

#include <stdio.h>
#include <string.h>


/*	Where the answer actually goes.
 *
 *	printf() writes to file descriptor one, and this program runs as the
 *	launch_daemon, which is started by the kernel before there is anything on the
 *	other end of that descriptor -- so a run that works and a run that dies
 *	silently look identical in the log. _kern_debug_output() is the syscall
 *	behind the kernel's own dprintf(), and it reaches the serial console the rest
 *	of the boot is already being read on.
 *
 *	Declared rather than included: the declaration lives in a private system
 *	header, and this program is built by hand against the sysroot rather than by
 *	the Jam rules that put those headers on the include path.
 *
 *	printf() is still called, and first. It is part of what this program exists
 *	to test -- if stdio is broken it should be broken visibly, before the line
 *	that reports success.
 */
extern void _kern_debug_output(const char* message);


int
main(int argc, char** argv)
{
	char buffer[64];

	snprintf(buffer, sizeof(buffer), "hellodyn ok, argc %d", argc);
	printf("%s, strlen %d\n", buffer, (int)strlen(buffer));
	fflush(stdout);

	_kern_debug_output(buffer);
	_kern_debug_output("\n");

	return 0;
}
