/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


/*!	PCI host bridge driver for the sun4u on-die bridges.
 *
 *	Every sun4u workstation reaches PCI through a bridge built into the CPU
 *	module rather than through a chipset on a bus that could be probed: sabre on
 *	the UltraSPARC-IIi machines this port targets (Blade 150, Ultra 10, and
 *	QEMU's sun4u), psycho on the larger Ultra 30 and 60, and schizo later still.
 *	They differ in how many PCI leaves they present and in their error handling,
 *	but they agree on the two things this driver needs: configuration space is a
 *	linear window of physical address space, and Open Firmware describes where
 *	that window is.
 *
 *	So there is nothing to detect. The device tree already says which bridges
 *	exist and what address ranges each one owns, because the firmware set them up
 *	before handing over. This driver reads that description and turns it into the
 *	pci_controller_module_info the PCI bus manager expects to find on its parent
 *	node -- which is the whole reason it exists. Haiku's PCI bus manager does not
 *	find controllers, it is attached beneath one, and on a machine with neither
 *	ACPI nor a flattened device tree there was nothing to attach it to.
 */


#include "SabrePCIController.h"

#include <string.h>
#include <new>

#include <AutoDeleter.h>
#include <debug.h>
#include <platform/openfirmware/openfirmware.h>


#define CHECK_RET(err)	{ status_t _err = (err); if (_err < B_OK) return _err; }


/*	An entry of a PCI node's "ranges" property, in cells.
 *
 *	Seven, from the PCI binding to Open Firmware: a three-cell PCI address, then
 *	a two-cell parent (physical) address, then a two-cell size. The parent and
 *	size halves are two cells each because the root node's #address-cells and
 *	#size-cells are both 2 on sun4u -- a 64-bit machine with more physical
 *	address space than a single cell can hold.
 *
 *	The first cell of the PCI address is the phys.hi word, whose top two bits say
 *	which of PCI's address spaces the entry describes; the address itself is the
 *	remaining two cells. That is why a PCI address is three cells where the
 *	physical one is two.
 */
#define PCI_RANGE_CELLS				7

#define PCI_SPACE_MASK				0x03000000
#define PCI_SPACE_CONFIGURATION		0x00000000
#define PCI_SPACE_IO				0x01000000
#define PCI_SPACE_MEMORY_32		 	0x02000000
#define PCI_SPACE_MEMORY_64			0x03000000

#define PCI_RANGE_PREFETCHABLE		0x40000000

// Eight is more entries than any sun4u bridge publishes -- configuration, I/O
// and memory is the whole set on sabre -- and the read below reports rather than
// silently truncates if that ever stops being true.
#define PCI_MAX_RANGES				8

/*	Physical, non-cacheable, little endian.
 *
 *	All three parts matter. Physical because configuration space is never mapped
 *	into the kernel's address space, non-cacheable because it is a register block
 *	whose reads have to reach the bridge, and little endian because PCI is little
 *	endian on a machine that is not -- the ASI does the byte swap that would
 *	otherwise have to be done in software on every access.
 *
 *	UltraSPARC-IIi User's Manual TABLE 6-2, printed page 78.
 */
#define ASI_PHYS_NON_CACHED_LITTLE	0x1d


// #pragma mark - device tree


/*!	Finds the next Open Firmware node describing a PCI host bridge.
 *
 *	Pass 0 to start. Returns 0 when there are none left.
 *
 *	One level below the root, which is where sun4u puts its bridges -- both of
 *	psycho's leaves appear there as siblings, as do sabre's single leaf and the
 *	one QEMU's sun4u publishes. The identifying property is device_type "pci",
 *	which is the Open Firmware binding rather than anything machine-specific, so
 *	this does not need a list of node names to match against. Which is the point:
 *	the node is called pci@1fe,0 under QEMU and pci@1f,0 on a Blade, and matching
 *	on the name would mean knowing every machine in advance.
 */
static intptr_t
next_host_bridge(intptr_t previous)
{
	intptr_t node;
	if (previous == 0) {
		intptr_t root = of_finddevice("/");
		if (root == OF_FAILED)
			return 0;
		node = of_child(root);
	} else
		node = of_peer(previous);

	for (; node != 0 && node != OF_FAILED; node = of_peer(node)) {
		char type[16];
		if (of_getprop(node, "device_type", type, sizeof(type)) == OF_FAILED)
			continue;
		if (strcmp(type, "pci") == 0)
			return node;
	}

	return 0;
}


// #pragma mark - driver


float
SabrePCIController::SupportsDevice(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			< B_OK) {
		return -1.0f;
	}

	if (strcmp(bus, "root") != 0)
		return 0.0f;

	// Not just "this is a sparc kernel". The add-on is only built for sparc, but
	// the port is developed against QEMU and aimed at hardware nobody has run it
	// on yet, so a machine whose firmware describes its bridges differently
	// should decline here rather than register a node whose driver then cannot
	// initialize.
	if (next_host_bridge(0) == 0)
		return 0.0f;

	return 1.0f;
}


/*!	Registers one node per host bridge.
 *
 *	One per bridge rather than one for the machine, because psycho presents two
 *	independent PCI leaves and they are separate domains: same driver, different
 *	configuration windows, different address ranges. The bus manager already
 *	handles that -- it keeps a domain per controller -- so the only thing needed
 *	here is for each node to know which bridge it stands for, which is what the
 *	phandle attribute is.
 */
status_t
SabrePCIController::RegisterDevice(device_node* parent)
{
	uint32 registered = 0;

	for (intptr_t node = next_host_bridge(0); node != 0;
			node = next_host_bridge(node)) {
		char path[128];
		if (of_package_to_path(node, path, sizeof(path)) == OF_FAILED)
			strlcpy(path, "SPARC PCI Host Bridge", sizeof(path));

		device_attr attrs[] = {
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = path } },
			{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
				{ .string = "bus_managers/pci/root/driver_v1" } },
			{ SABRE_PCI_NODE_ITEM, B_UINT32_TYPE, { .ui32 = (uint32)node } },
			{}
		};

		status_t status = gDeviceManager->register_node(parent,
			SABRE_PCI_DRIVER_MODULE_NAME, attrs, NULL, NULL);

		// B_NAME_IN_USE means the node is already there, which is a success from
		// here: the device manager rescans the root node, and a bridge that was
		// registered the first time round does not need registering again.
		if (status == B_OK || status == B_NAME_IN_USE)
			registered++;
		else {
			dprintf("sabre: could not register a node for %s: %s\n", path,
				strerror(status));
		}
	}

	// Registering none is a failure even though the loop had nothing to report,
	// because SupportsDevice() said there was at least one bridge. If that has
	// stopped being true between the two calls something is wrong that is worth
	// hearing about.
	return registered > 0 ? B_OK : B_ERROR;
}


status_t
SabrePCIController::InitDriver(device_node* node, SabrePCIController*& _driver)
{
	uint32 openFirmwareNode;
	CHECK_RET(gDeviceManager->get_attr_uint32(node, SABRE_PCI_NODE_ITEM,
		&openFirmwareNode, false));

	ObjectDeleter<SabrePCIController> driver(
		new(std::nothrow) SabrePCIController());
	if (!driver.IsSet())
		return B_NO_MEMORY;

	driver->fNode = node;
	driver->fOpenFirmwareNode = (intptr_t)openFirmwareNode;

	CHECK_RET(driver->_ReadRanges(driver->fOpenFirmwareNode));

	_driver = driver.Detach();
	return B_OK;
}


void
SabrePCIController::UninitDriver()
{
	delete this;
}


/*!	Reads the bridge's address ranges out of the device tree.
 *
 *	The configuration window is the one this driver needs for itself; the I/O and
 *	memory windows are handed to the bus manager, which maps the I/O one and uses
 *	both to translate the addresses it finds in device BARs. A PCI address is not
 *	a physical address on this machine -- a BAR holding 0x400000 means 0x400000
 *	within the bridge's memory window, which lives at physical 0x1ff.00000000 --
 *	and the pci_address/host_address pair in each range is what makes that
 *	translation possible.
 */
status_t
SabrePCIController::_ReadRanges(intptr_t node)
{
	uint32 ranges[PCI_RANGE_CELLS * PCI_MAX_RANGES];
	intptr_t length = of_getprop(node, "ranges", ranges, sizeof(ranges));
	if (length == OF_FAILED) {
		dprintf("sabre: host bridge has no \"ranges\" property\n");
		return B_ERROR;
	}

	if (of_getproplen(node, "ranges") > (intptr_t)sizeof(ranges)) {
		dprintf("sabre: \"ranges\" has more than %d entries; the rest are "
			"ignored\n", PCI_MAX_RANGES);
	}

	uint32 cells = length / sizeof(uint32);
	for (uint32 i = 0; i + PCI_RANGE_CELLS <= cells; i += PCI_RANGE_CELLS) {
		uint32 phraseHigh = ranges[i];
		uint64 pciAddress = ((uint64)ranges[i + 1] << 32) | ranges[i + 2];
		phys_addr_t hostAddress
			= ((phys_addr_t)ranges[i + 3] << 32) | ranges[i + 4];
		uint64 size = ((uint64)ranges[i + 5] << 32) | ranges[i + 6];

		pci_resource_range range = {};
		range.host_address = hostAddress;
		range.pci_address = pciAddress;
		range.size = size;

		if ((phraseHigh & PCI_RANGE_PREFETCHABLE) != 0)
			range.address_type |= PCI_address_prefetchable;

		const char* what;
		switch (phraseHigh & PCI_SPACE_MASK) {
			case PCI_SPACE_CONFIGURATION:
				what = "configuration";
				fConfigurationBase = hostAddress;
				fConfigurationSize = size;
				break;

			case PCI_SPACE_IO:
				what = "I/O";
				range.type = B_IO_PORT;
				fResourceRanges.Add(range);
				break;

			case PCI_SPACE_MEMORY_32:
				what = "memory";
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_32;
				fResourceRanges.Add(range);
				break;

			case PCI_SPACE_MEMORY_64:
				what = "64 bit memory";
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_64;
				fResourceRanges.Add(range);
				break;

			default:
				what = "?";
				break;
		}

		dprintf("sabre: %s space, pci %#" B_PRIx64 " at physical %#"
			B_PRIxPHYSADDR ", %" B_PRIu64 " KB\n", what, pciAddress,
			hostAddress, size / 1024);
	}

	if (fConfigurationBase == 0) {
		dprintf("sabre: \"ranges\" describes no configuration space\n");
		return B_ERROR;
	}

	// Absent, this stays 0 to 0, which is a bridge with only bus zero behind it.
	// That is the conservative reading: guessing 255 would have the bus manager
	// probing configuration space the bridge does not decode, which on real
	// hardware is a bus error rather than a read of all ones.
	uint32 busRange[2];
	if (of_getprop(node, "bus-range", busRange, sizeof(busRange))
			!= OF_FAILED) {
		fFirstBus = (uint8)busRange[0];
		fLastBus = (uint8)busRange[1];
	}

	dprintf("sabre: buses %u to %u\n", fFirstBus, fLastBus);

	return B_OK;
}


// #pragma mark - configuration space


/*!	Physical address of a configuration register, or zero if there is none.
 *
 *	sabre decodes configuration space linearly, with the bus, device and function
 *	numbers packed into the address exactly as the PCI specification packs them
 *	into a type 1 configuration address. Sixteen megabytes is what eight bits of
 *	bus, five of device, three of function and eight of register add up to, which
 *	is the size the firmware reports.
 *
 *	The bounds check is not decoration. On a bus the bridge does not decode, real
 *	sabre silicon signals a master abort and the access takes a data access error
 *	trap, where QEMU quietly returns all ones -- so a driver developed against
 *	the emulator will not find out it is reading outside the window until it is
 *	on hardware. Refusing here means the bus manager gets an error instead.
 */
phys_addr_t
SabrePCIController::_ConfigAddress(uint8 bus, uint8 device, uint8 function,
	uint16 offset)
{
	if (bus < fFirstBus || bus > fLastBus)
		return 0;

	phys_addr_t address = ((phys_addr_t)bus << 16)
		| ((phys_addr_t)(device & 0x1f) << 11)
		| ((phys_addr_t)(function & 0x07) << 8) | offset;

	if (address + 4 > fConfigurationSize)
		return 0;

	return fConfigurationBase + address;
}


status_t
SabrePCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	phys_addr_t address = _ConfigAddress(bus, device, function, offset);
	if (address == 0)
		return ERANGE;

	uint64 read;
	switch (size) {
		case 1:
			asm volatile("lduba [%[address]] %[asi], %[value]"
				: [value] "=r"(read)
				: [address] "r"(address), [asi] "i"(ASI_PHYS_NON_CACHED_LITTLE));
			break;

		case 2:
			asm volatile("lduha [%[address]] %[asi], %[value]"
				: [value] "=r"(read)
				: [address] "r"(address), [asi] "i"(ASI_PHYS_NON_CACHED_LITTLE));
			break;

		case 4:
			asm volatile("lduwa [%[address]] %[asi], %[value]"
				: [value] "=r"(read)
				: [address] "r"(address), [asi] "i"(ASI_PHYS_NON_CACHED_LITTLE));
			break;

		default:
			return B_BAD_VALUE;
	}

	value = (uint32)read;
	return B_OK;
}


status_t
SabrePCIController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	phys_addr_t address = _ConfigAddress(bus, device, function, offset);
	if (address == 0)
		return ERANGE;

	switch (size) {
		case 1:
			asm volatile("stba %[value], [%[address]] %[asi]"
				: : [value] "r"(value), [address] "r"(address),
					[asi] "i"(ASI_PHYS_NON_CACHED_LITTLE)
				: "memory");
			break;

		case 2:
			asm volatile("stha %[value], [%[address]] %[asi]"
				: : [value] "r"(value), [address] "r"(address),
					[asi] "i"(ASI_PHYS_NON_CACHED_LITTLE)
				: "memory");
			break;

		case 4:
			asm volatile("stwa %[value], [%[address]] %[asi]"
				: : [value] "r"(value), [address] "r"(address),
					[asi] "i"(ASI_PHYS_NON_CACHED_LITTLE)
				: "memory");
			break;

		default:
			return B_BAD_VALUE;
	}

	return B_OK;
}


// #pragma mark - PCI controller


status_t
SabrePCIController::GetMaxBusDevices(int32& count)
{
	count = 32;
	return B_OK;
}


/*!	Interrupt routing, which this does not do yet.
 *
 *	The number in a device's configuration-space interrupt line register is not
 *	an interrupt this machine can install a handler for. sun4u routes PCI
 *	interrupts through the bridge's own interrupt mapping registers, and what
 *	connects a device to one of those is the "interrupt-map" property on the PCI
 *	node -- the same mechanism the flattened-device-tree platforms use, and the
 *	reason ECAMPCIControllerFDT has a Finalize() that walks it.
 *
 *	Reporting that rather than returning a plausible wrong number, because a
 *	wrong interrupt is a driver that installs a handler and then waits forever,
 *	which is a much harder thing to recognise than a driver that says it has no
 *	interrupt. Nothing the port needs to boot uses interrupts yet: the disk stack
 *	polls until an interrupt is available to it.
 */
status_t
SabrePCIController::ReadIrq(uint8 bus, uint8 device, uint8 function, uint8 pin,
	uint8& irq)
{
	return B_UNSUPPORTED;
}


status_t
SabrePCIController::WriteIrq(uint8 bus, uint8 device, uint8 function, uint8 pin,
	uint8 irq)
{
	return B_UNSUPPORTED;
}


status_t
SabrePCIController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index >= (uint32)fResourceRanges.Count())
		return B_BAD_INDEX;

	*range = fResourceRanges[index];
	return B_OK;
}


status_t
SabrePCIController::Finalize()
{
	// Where the interrupt-map walk will go, once ReadIrq() has something to
	// report. See the comment there.
	return B_OK;
}
