/*
 * Copyright 2003, Axel Dörfler, axeld@pinc-software.de.
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "Handle.h"

#include <SupportDefs.h>

#include <platform/openfirmware/openfirmware.h>
#include <util/kernel_cpp.h>


Handle::Handle(intptr_t handle, bool takeOwnership)
	:
	fHandle(handle),
	fOwnHandle(takeOwnership)
{
}


Handle::Handle(void)
	:
	fHandle(0)
{
}


Handle::~Handle()
{
	if (fOwnHandle)
		of_close(fHandle);
}


void
Handle::SetHandle(intptr_t handle, bool takeOwnership)
{
	if (fHandle && fOwnHandle)
		of_close(fHandle);

	fHandle = handle;
	fOwnHandle = takeOwnership;
}


/*!	Reads through the firmware, into the caller's buffer.

	"Buffer" is meant literally: the firmware stores into the memory it is handed
	rather than copying from anything of its own, and it chooses the access width
	itself. OpenBIOS's IDE path reads the data register with halfword PIO and
	stores halfwords, so a buffer at an odd address takes a
	mem_address_not_aligned trap inside the firmware, where no amount of looking
	at Haiku's code explains it.

	There is nothing to be done about that here -- the alignment has to come from
	the caller -- but it is worth knowing that a caller can hand this an odd
	address and be blamed for it three frames away. src/system/boot/loader/elf.cpp
	is where that happened.
*/
ssize_t
Handle::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	if (pos == -1 || of_seek(fHandle, pos) != OF_FAILED)
		return of_read(fHandle, buffer, bufferSize);

	return B_ERROR;
}


ssize_t
Handle::WriteAt(void *cookie, off_t pos, const void *buffer, size_t bufferSize)
{
	if (pos == -1 || of_seek(fHandle, pos) != OF_FAILED)
		return of_write(fHandle, buffer, bufferSize);

	return B_ERROR;
}


off_t
Handle::Size() const
{
	// ToDo: fix this!
	return 1024LL * 1024 * 1024 * 1024;
		// 1024 GB
}

