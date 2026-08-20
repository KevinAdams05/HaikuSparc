/*
 * Copyright 2006, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "pci.h"
#include "pci_private.h"
#include "arch_cpu.h"

#include <ByteOrder.h>


//#define TRACE_PCI_IO
#undef TRACE
#ifdef TRACE_PCI_IO
#	define TRACE(x...) dprintf("PCI_IO: " x)
#else
#	define TRACE(x...) ;
#endif


#if defined(__i386__) || defined(__x86_64__)

uint8
pci_read_io_8(int mapped_io_addr)
{
	return in8(mapped_io_addr);
}


void
pci_write_io_8(int mapped_io_addr, uint8 value)
{
	out8(value, mapped_io_addr);
}


uint16
pci_read_io_16(int mapped_io_addr)
{
	return in16(mapped_io_addr);
}


void
pci_write_io_16(int mapped_io_addr, uint16 value)
{
	out16(value, mapped_io_addr);
}


uint32
pci_read_io_32(int mapped_io_addr)
{
	return in32(mapped_io_addr);
}


void
pci_write_io_32(int mapped_io_addr, uint32 value)
{
	out32(value, mapped_io_addr);
}

#else

/*	PCI I/O space is little endian, and on a big-endian host that has to be said
	out loud.

	These reach the ports through a mapping of the host bridge's I/O window
	rather than through an instruction that knows what bus it is talking to, so a
	multi-byte load returns the bytes in the host's order and the value is
	byte-reversed. The conversions below are what the specification already says
	about the bus; they compile away entirely on a little-endian host, which is
	every platform that took this path before SPARC.

	The eight-bit accessors need nothing, and a caller reading a byte stream
	rather than a number -- an ATA PIO data transfer, say -- has to convert back,
	because a stream of bytes has no byte order to correct. See
	ata_adapter_read_pio().
*/

static uint8*
get_io_port_address(int ioPort)
{
	uint8 domain;
	pci_resource_range range;
	uint8 *mappedAdr;

	if (gPCI->LookupRange(B_IO_PORT, ioPort, domain, range, &mappedAdr) < B_OK)
		return NULL;

	return mappedAdr + ioPort;
}


uint8
pci_read_io_8(int mapped_io_addr)
{
	TRACE("pci_read_io_8(%d)\n", mapped_io_addr);
	vuint8* ptr = get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return 0;

	return *ptr;
}


void
pci_write_io_8(int mapped_io_addr, uint8 value)
{
	TRACE("pci_write_io_8(%d)\n", mapped_io_addr);
	vuint8* ptr = get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return;

	*ptr = value;
}


uint16
pci_read_io_16(int mapped_io_addr)
{
	TRACE("pci_read_io_16(%d)\n", mapped_io_addr);
	vuint16* ptr = (uint16*)get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return 0;

	return B_LENDIAN_TO_HOST_INT16(*ptr);
}


void
pci_write_io_16(int mapped_io_addr, uint16 value)
{
	TRACE("pci_write_io_16(%d)\n", mapped_io_addr);
	vuint16* ptr = (uint16*)get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return;

	*ptr = B_HOST_TO_LENDIAN_INT16(value);
}


uint32
pci_read_io_32(int mapped_io_addr)
{
	TRACE("pci_read_io_32(%d)\n", mapped_io_addr);
	vuint32* ptr = (uint32*)get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return 0;

	return B_LENDIAN_TO_HOST_INT32(*ptr);
}


void
pci_write_io_32(int mapped_io_addr, uint32 value)
{
	TRACE("pci_write_io_32(%d)\n", mapped_io_addr);
	vuint32* ptr = (vuint32*)get_io_port_address(mapped_io_addr);
	if (ptr == NULL)
		return;

	*ptr = B_HOST_TO_LENDIAN_INT32(value);
}

#endif
