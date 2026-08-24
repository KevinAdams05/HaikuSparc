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


int
main(int argc, char** argv)
{
	char buffer[64];

	snprintf(buffer, sizeof(buffer), "hellodyn ok, argc %d", argc);
	printf("%s, strlen %d\n", buffer, (int)strlen(buffer));
	fflush(stdout);

	return 0;
}
