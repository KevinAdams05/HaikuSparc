/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*!	The one thing this driver needs that its donor got from somewhere this port
	does not have: the station address.

	FreeBSD's if_hme_pci.c calls OF_getetheraddr(), which asks the FreeBSD
	Open Firmware bus layer for the address belonging to the device's node.
	Haiku has no such bus layer -- its PCI enumeration comes from the bus manager,
	which does not carry a firmware node with it -- so the node has to be found
	from the device's identity instead. That is what this file does, and it is the
	entire Haiku-specific part of the driver.

	Why the address cannot come from the chip: it does not live there. On an
	Ultra 10 the station address is in the machine's IDPROM, and Open Firmware
	publishes it on each network node as `local-mac-address`, with a
	machine-global fallback on the root node as `mac-address`. The hme registers
	hold whatever the driver programs into them; reading them before programming
	them returns the reset value, not an address.

	Nothing here talks to the hardware, so the two lookups are cheap and happen
	once at attach.
*/

#include <string.h>

#include <KernelExport.h>

#include <platform/openfirmware/openfirmware.h>


#define ETHERNET_ADDRESS_LENGTH		6


/*!	Whether an Open Firmware node describes a network device.

	`device_type` rather than the node's name, because the name varies with the
	part -- a Sun HME node is called `network` on an Ultra 10 and `SUNW,hme` on
	other machines that carry the same silicon -- while the type is the property
	IEEE 1275 defines for exactly this question.
*/
static bool
is_network_node(intptr_t node)
{
	char type[32];
	if (of_getprop(node, "device_type", type, sizeof(type)) == OF_FAILED)
		return false;

	type[sizeof(type) - 1] = '\0';
	return strcmp(type, "network") == 0;
}


/*!	Depth-first walk looking for a network node that has an address on it.

	Recursive, and bounded by the depth of the firmware's device tree rather than
	by anything this code enforces -- which is safe here because the tree is built
	by the firmware before the kernel runs and is four or five levels deep on
	every machine this port targets. A malformed tree would be a firmware bug that
	had already broken the boot.

	The first match wins. A machine with two HME ports publishes two nodes with
	different `local-mac-address` values, and telling them apart needs the PCI
	address the bus manager has and the firmware node does not obviously carry --
	so this deliberately does not try. Both target machines have one.
*/
static bool
find_mac_address(intptr_t node, uint8* address)
{
	for (; node != 0 && node != OF_FAILED; node = of_peer(node)) {
		if (is_network_node(node)) {
			intptr_t length = of_getprop(node, "local-mac-address", address,
				ETHERNET_ADDRESS_LENGTH);
			if (length == ETHERNET_ADDRESS_LENGTH)
				return true;
		}

		if (find_mac_address(of_child(node), address))
			return true;
	}

	return false;
}


/*!	The station address out of the machine's IDPROM.

	Where a Sun keeps it, and on this machine the only place it is. The firmware
	publishes the whole 32-byte PROM as the root node's `idprom` property; the
	address is six bytes at offset 2, after the format byte and the machine type.
	NetBSD and OpenBSD read the same bytes from the same place.

	The checksum is verified rather than trusted. It is byte 15, defined as the
	XOR of the fifteen bytes before it, and checking it is what tells a PROM whose
	layout is what this code assumes from one that merely happens to be 32 bytes
	long -- which matters because the alternative to noticing is an interface that
	comes up with six bytes of something else as its address.
*/
static bool
mac_address_from_idprom(intptr_t root, uint8* address)
{
	uint8 idprom[32];
	if (of_getprop(root, "idprom", idprom, sizeof(idprom))
			!= (intptr_t)sizeof(idprom))
		return false;

	uint8 checksum = 0;
	for (int i = 0; i < 15; i++)
		checksum ^= idprom[i];

	if (checksum != idprom[15]) {
		dprintf("hme: the idprom checksum is %#x, expected %#x -- not reading "
			"an address out of it\n", checksum, idprom[15]);
		return false;
	}

	memcpy(address, idprom + 2, ETHERNET_ADDRESS_LENGTH);
	return true;
}


/*!	Fills in the station address, or reports that it could not.

	Called from if_hme_pci.c in place of OF_getetheraddr().

	Four places, because on this hardware they are genuinely different things
	rather than four spellings of one, and which of them answers depends on the
	firmware:

	  1. `local-mac-address` on the device's own node -- the address of *this
	     port*, which is what a multi-port machine has and what a driver wants.
	     Sun's OpenBoot publishes it; OpenBIOS does not.
	  2. `mac-address` on /chosen -- the machine's address, published by firmware
	     that net-booted.
	  3. The root node's `idprom`, which is the machine's own PROM and the place a
	     Sun has always kept this. **This is the one that answers under QEMU**, and
	     the reason the first two are still tried first is that they are more
	     specific: on a machine with two ports the IDPROM has one address and each
	     port has its own.
	  4. The Forth word `mac-address`, which is not a property at all -- it leaves
	     a length and a pointer on the stack.

	Trying one and giving up is how a driver ends up with no address on hardware
	where the address was there all along; this port spent three boots proving
	that.
*/
extern "C" bool
hme_get_ethernet_address(uint8* address)
{
	intptr_t root = of_finddevice("/");

	if (root != OF_FAILED && find_mac_address(of_child(root), address))
		return true;

	if (gChosen != OF_FAILED && of_getprop(gChosen, "mac-address", address,
			ETHERNET_ADDRESS_LENGTH) == ETHERNET_ADDRESS_LENGTH)
		return true;

	if (root != OF_FAILED && mac_address_from_idprom(root, address))
		return true;

	/*	Not a property. The pointer it returns is into the firmware's own memory,
		which is still mapped here because the kernel kept the firmware's
		translations when it took the MMU over.
	 */
	size_t length = 0;
	void* data = NULL;
	if (of_interpret("mac-address", 0, 2, &length, &data) != OF_FAILED
		&& length == ETHERNET_ADDRESS_LENGTH && data != NULL) {
		memcpy(address, data, ETHERNET_ADDRESS_LENGTH);
		return true;
	}

	dprintf("hme: no station address -- no local-mac-address on any network "
		"node, no mac-address on /chosen, no usable idprom on /\n");
	return false;
}
