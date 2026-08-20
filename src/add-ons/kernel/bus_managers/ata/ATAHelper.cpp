/*
 * Copyright 2009, Michael Lotz, mmlr@mlotz.ch.
 * Copyright 2004-2007, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002/03, Thomas Kurschel. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */

#include "ATAPrivate.h"

#include <vm/vm.h>
#include <string.h>


/*!	Copy data between ccb data and buffer
	ccb			- ccb to copy data from/to
	offset			- offset of data in ccb
	allocation_length- limit of ccb's data buffer according to CDB
	buffer			- data to copy data from/to
	size			- number of bytes to copy
	to_buffer		- true: copy from ccb to buffer
					  false: copy from buffer to ccb
	return: true, if data of ccb was large enough
*/
bool
copy_sg_data(scsi_ccb *ccb, uint offset, uint allocationLength,
	void *buffer, int size, bool toBuffer)
{
	const physical_entry *sgList = ccb->sg_list;
	int sgCount = ccb->sg_count;

	// skip unused S/G entries
	while (sgCount > 0 && offset >= sgList->size) {
		offset -= sgList->size;
		++sgList;
		--sgCount;
	}

	if (sgCount == 0)
		return false;

	// remaining bytes we are allowed to copy from/to ccb
	int requestSize = MIN(allocationLength, ccb->data_length) - offset;

	// copy one S/G entry at a time
	for (; size > 0 && requestSize > 0 && sgCount > 0; ++sgList, --sgCount) {
		size_t bytes;

		bytes = MIN(size, requestSize);
		bytes = MIN(bytes, sgList->size);

		if (toBuffer) {
			vm_memcpy_from_physical(buffer, sgList->address + offset, bytes,
				false);
		} else {
			vm_memcpy_to_physical(sgList->address + offset, buffer, bytes,
				false);
		}

		buffer = (char *)buffer + bytes;
		size -= bytes;
		offset = 0;
	}

	return size == 0;
}


void
swap_words(void *data, size_t size)
{
	uint16 *word = (uint16 *)data;
	size_t count = size / 2;
	while (count--) {
		*word = B_BENDIAN_TO_HOST_INT16(*word);
		word++;
	}
}


/*!	Turns an identify block as the device sent it into one this host can read.

	The transfer that fetched it preserved the byte order the device sent, which
	is what a data transfer needs and what every field here is the wrong way
	round for: the block is a list of 16-bit numbers, little endian on the wire,
	and reading them on a big-endian host without conversion gives every value
	byte-reversed. That is not a subtle failure -- the capability bits land in the
	wrong half of each word, so a perfectly ordinary disk reports itself as
	having no LBA support.

	The strings are deliberately left alone. ATA puts the first character of a
	string in the *high* byte of its word, so swapping the word puts the pair in
	reading order, which is exactly the state swap_words() expects to be handed
	and leaves untouched on a big-endian host.

	Fields wider than a word need a second step, because swapping each word
	individually does not reorder the words themselves. There are three of them,
	all little endian across words as well as within them.

	Every line of this compiles away on a little-endian host, where the wire order
	is already the host's.
*/
void
ata_info_block_to_host(ata_device_infoblock *infoBlock)
{
#if B_HOST_IS_BENDIAN
	uint16 *word = (uint16 *)infoBlock;
	for (size_t i = 0; i < sizeof(*infoBlock) / sizeof(uint16); i++)
		word[i] = B_LENDIAN_TO_HOST_INT16(word[i]);

	infoBlock->lba_sector_count
		= (infoBlock->lba_sector_count << 16)
			| (infoBlock->lba_sector_count >> 16);

	infoBlock->logical_sector_size
		= (infoBlock->logical_sector_size << 16)
			| (infoBlock->logical_sector_size >> 16);

	uint64 sectors = infoBlock->lba48_sector_count;
	infoBlock->lba48_sector_count = ((sectors & 0x000000000000ffffULL) << 48)
		| ((sectors & 0x00000000ffff0000ULL) << 16)
		| ((sectors & 0x0000ffff00000000ULL) >> 16)
		| ((sectors & 0xffff000000000000ULL) >> 48);
#endif
}
