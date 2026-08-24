/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FBSD_MACHINE_GENERIC_BUS_H_
#define _FBSD_MACHINE_GENERIC_BUS_H_


#include <machine/_bus.h>


/*	Bus accesses convert; stream accesses do not.
 *
 *	That distinction is what the two families of accessor mean in FreeBSD, and it
 *	is invisible on a little-endian host because there the conversion is nothing.
 *	The device on the other end of a PCI bus is little-endian by specification, so
 *	a big-endian CPU reading a 32-bit register through a mapped BAR gets the bytes
 *	in the opposite order from the value the device holds. bus_space_read_4() is
 *	defined to hand back the value; bus_space_read_stream_4() is defined to hand
 *	back the bytes, which is what a driver moving a packet through a FIFO wants.
 *
 *	Built on the compiler's own byte-order macros rather than an include, so this
 *	header keeps working wherever it is pulled in from.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define __bus_space_convert_2(value)	__builtin_bswap16(value)
#  define __bus_space_convert_4(value)	__builtin_bswap32(value)
#  define __bus_space_convert_8(value)	__builtin_bswap64(value)
#else
#  define __bus_space_convert_2(value)	(value)
#  define __bus_space_convert_4(value)	(value)
#  define __bus_space_convert_8(value)	(value)
#endif


#define	BUS_SPACE_ALIGNED_POINTER(p, t) ALIGNED_POINTER(p, t)

#define	BUS_SPACE_MAXADDR_24BIT	0xFFFFFFUL
#define	BUS_SPACE_MAXADDR_32BIT 0xFFFFFFFFUL
#define	BUS_SPACE_MAXSIZE_24BIT	0xFFFFFFUL
#define	BUS_SPACE_MAXSIZE_32BIT	0xFFFFFFFFUL

#define	BUS_SPACE_MAXADDR 	0xFFFFFFFFFFFFFFFFUL
#define	BUS_SPACE_MAXSIZE 	0xFFFFFFFFFFFFFFFFUL

#define BUS_SPACE_INVALID_DATA	(~0)
#define BUS_SPACE_UNRESTRICTED	(~0)


static __inline u_int8_t
bus_space_read_1(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return (*(volatile u_int8_t *)(handle + offset));
}


static __inline u_int16_t
bus_space_read_2(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return __bus_space_convert_2(*(volatile u_int16_t *)(handle + offset));
}


static __inline u_int32_t
bus_space_read_4(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return __bus_space_convert_4(*(volatile u_int32_t *)(handle + offset));
}


static __inline uint64_t
bus_space_read_8(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return __bus_space_convert_8(*(volatile uint64_t *)(handle + offset));
}


static __inline void
bus_space_write_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int8_t *)(bsh + offset) = value;
}


static __inline void
bus_space_write_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int16_t *)(bsh + offset) = __bus_space_convert_2(value);
}


static __inline void
bus_space_write_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int32_t *)(bsh + offset) = __bus_space_convert_4(value);
}


static __inline void
bus_space_write_8(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, uint64_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile uint64_t *)(bsh + offset) = __bus_space_convert_8(value);
}


static __inline void
bus_space_read_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t *addr, size_t count)
{
	for (; count > 0; offset += 1, addr++, count--)
		*addr = bus_space_read_1(tag, bsh, offset);
}


static __inline void
bus_space_read_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		*addr = bus_space_read_2(tag, bsh, offset);
}


static __inline void
bus_space_read_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		*addr = bus_space_read_4(tag, bsh, offset);
}


static __inline void
bus_space_write_multi_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int8_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_1(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_multi_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_multi_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int8_t *addr, size_t count)
{
	for (; count > 0; offset += 1, addr++, count--)
		bus_space_write_1(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		bus_space_write_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		bus_space_write_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_set_region_1(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int8_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_1(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_2(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_4(tag, bsh, offset, value);
}


#define	BUS_SPACE_BARRIER_READ	0x01		/* force read barrier */
#define	BUS_SPACE_BARRIER_WRITE	0x02		/* force write barrier */


static __inline void
bus_space_barrier(bus_space_tag_t tag __unused, bus_space_handle_t bsh __unused,
	bus_size_t offset __unused, bus_size_t len __unused, int flags)
{
	__compiler_membar();
}


/*	A handle for part of an already-mapped region. Address arithmetic, as it is on
 *	every architecture whose bus handles are plain addresses -- which is all of
 *	the ones this header serves.
 */
static __inline int
bus_space_subregion(bus_space_tag_t tag __unused, bus_space_handle_t bsh,
	bus_size_t offset, bus_size_t size __unused, bus_space_handle_t *result)
{
	*result = bsh + offset;
	return (0);
}


#include <machine/bus_dma.h>

/*	Stream accesses move bytes rather than values, so they are the accessors that
 *	do *not* convert. On a little-endian host that makes them identical to the
 *	ones above and the aliasing below is exact; on a big-endian one it does not,
 *	so the scalars are spelled out.
 *
 *	The byte-wide ones alias in either case, there being no order to a byte.
 */
#define	bus_space_read_stream_1(t, h, o)	bus_space_read_1((t), (h), (o))

static __inline u_int16_t
bus_space_read_stream_2(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return (*(volatile u_int16_t *)(handle + offset));
}


static __inline u_int32_t
bus_space_read_stream_4(bus_space_tag_t tag, bus_space_handle_t handle,
	bus_size_t offset)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return BUS_SPACE_INVALID_DATA;
	return (*(volatile u_int32_t *)(handle + offset));
}


static __inline void
bus_space_write_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int16_t *)(bsh + offset) = value;
}


static __inline void
bus_space_write_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value)
{
	if (tag != BUS_SPACE_TAG_MEM)
		return;
	*(volatile u_int32_t *)(bsh + offset) = value;
}


static __inline void
bus_space_read_multi_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		*addr = bus_space_read_stream_2(tag, bsh, offset);
}


static __inline void
bus_space_read_multi_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		*addr = bus_space_read_stream_4(tag, bsh, offset);
}


static __inline void
bus_space_write_multi_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_stream_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_multi_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; addr++, count--)
		bus_space_write_stream_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_read_region_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		*addr = bus_space_read_stream_2(tag, bsh, offset);
}


static __inline void
bus_space_read_region_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		*addr = bus_space_read_stream_4(tag, bsh, offset);
}


static __inline void
bus_space_write_region_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int16_t *addr, size_t count)
{
	for (; count > 0; offset += 2, addr++, count--)
		bus_space_write_stream_2(tag, bsh, offset, *addr);
}


static __inline void
bus_space_write_region_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, const u_int32_t *addr, size_t count)
{
	for (; count > 0; offset += 4, addr++, count--)
		bus_space_write_stream_4(tag, bsh, offset, *addr);
}


static __inline void
bus_space_set_multi_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_stream_2(tag, bsh, offset, value);
}


static __inline void
bus_space_set_multi_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value, size_t count)
{
	for (; count > 0; count--)
		bus_space_write_stream_4(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_stream_2(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int16_t value, size_t count)
{
	for (; count > 0; offset += 2, count--)
		bus_space_write_stream_2(tag, bsh, offset, value);
}


static __inline void
bus_space_set_region_stream_4(bus_space_tag_t tag, bus_space_handle_t bsh,
	bus_size_t offset, u_int32_t value, size_t count)
{
	for (; count > 0; offset += 4, count--)
		bus_space_write_stream_4(tag, bsh, offset, value);
}

#define	bus_space_read_multi_stream_1(t, h, o, a, c) \
	bus_space_read_multi_1((t), (h), (o), (a), (c))
#define	bus_space_write_stream_1(t, h, o, v) \
	bus_space_write_1((t), (h), (o), (v))
#define	bus_space_write_multi_stream_1(t, h, o, a, c) \
	bus_space_write_multi_1((t), (h), (o), (a), (c))
#define	bus_space_set_multi_stream_1(t, h, o, v, c) \
	bus_space_set_multi_1((t), (h), (o), (v), (c))
#define	bus_space_read_region_stream_1(t, h, o, a, c) \
	bus_space_read_region_1((t), (h), (o), (a), (c))
#define	bus_space_write_region_stream_1(t, h, o, a, c) \
	bus_space_write_region_1((t), (h), (o), (a), (c))
#define	bus_space_set_region_stream_1(t, h, o, v, c) \
	bus_space_set_region_1((t), (h), (o), (v), (c))

#define	bus_space_copy_region_stream_1(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_1((t), (h1), (o1), (h2), (o2), (c))
#define	bus_space_copy_region_stream_2(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_2((t), (h1), (o1), (h2), (o2), (c))
#define	bus_space_copy_region_stream_4(t, h1, o1, h2, o2, c) \
	bus_space_copy_region_4((t), (h1), (o1), (h2), (o2), (c))


#endif /* _FBSD_MACHINE_GENERIC_BUS_H_ */
