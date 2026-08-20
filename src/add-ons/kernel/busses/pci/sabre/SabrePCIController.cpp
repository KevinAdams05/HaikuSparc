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
#include <arch_int.h>
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

// The bus, device and function bits of a configuration-space address, which is
// what a PCI child's "reg" property begins with.
#define PCI_CONFIG_ADDRESS_MASK			0x00ffff00

// An interrupt-map entry is six cells on this machine and a bridge publishes one
// per device it has wiring for, so 128 covers a fully populated bus with room to
// spare. Reported rather than silently truncated if it ever does not.
#define PCI_MAX_INTERRUPT_MAP_CELLS		128

/*	How a PCI master reaches host memory, which on this machine is a decision
	rather than a given.
 *
 *	TABLE 10-3 lists three modes. With the IOMMU enabled a 32-bit DMA address is
 *	a *virtual* one, translated through a table in memory. With it disabled, an
 *	address that hits the target address space register is used directly as a
 *	physical DRAM address -- "pass-through", section 10.3.3, with the high bits
 *	padded with zero. The third, bypass, needs a 64-bit dual-cycle address and is
 *	therefore unavailable to anything whose descriptors are 32-bit, which
 *	includes ATA's.
 *
 *	Pass-through is what Haiku's DMA model already assumes: drivers are handed
 *	physical addresses and expect a device to reach them. And it is sufficient on
 *	this generation rather than merely convenient -- UltraSPARC-IIi supports at
 *	most a gigabyte of physical memory (the TSB base register documents bits
 *	33:30 as "always zero, since only 1-Gbyte of physical memory is supported"),
 *	so every byte of DRAM is reachable in 32 bits and there is nothing an aperture
 *	would have to be rationed for.
 *
 *	What it does not give is protection: a device can write anywhere in DRAM. That
 *	is the same guarantee x86 offers without an IOMMU, so it costs Haiku nothing
 *	it was relying on -- but it is a reason a real translation table is still
 *	worth building later, along with the machines that have more memory than this.
 *
 *	Register addresses from section 19.3, printed pages 298 and 308.
 */
#define SABRE_IOMMU_CONTROL			0x0200
#define SABRE_IOMMU_TSB_BASE			0x0208
#define SABRE_IOMMU_FLUSH			0x0210
#define SABRE_TARGET_ADDRESS_SPACE		0x2028

#define IOMMU_CONTROL_ENABLE			(1ULL << 0)
#define IOMMU_CONTROL_DIAGNOSTIC		(1ULL << 1)

// Each bit of the target address space register enables one 512 MB region as a
// PCI target, bit 0 being 0x0000.0000-0x1fff.ffff. TABLE 19-6.
#define TARGET_ADDRESS_SPACE_GRANULE		(512 * 1024 * 1024)

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


/*	The bridge's own registers, which are neither configuration space nor a PCI
 *	address: they are host-side control registers at a fixed physical address, so
 *	they are read big-endian and non-cacheable. Note the ASI is 0x15 and not the
 *	0x1d used for configuration space -- there is no byte swap here.
 */
static inline uint64
read_bridge_register(phys_addr_t address)
{
	uint64 value;
	asm volatile("ldxa [%[address]] 0x15, %[value]"
		: [value] "=r"(value) : [address] "r"(address));
	return value;
}


static inline void
write_bridge_register(phys_addr_t address, uint64 value)
{
	asm volatile("stxa %[value], [%[address]] 0x15"
		: : [value] "r"(value), [address] "r"(address) : "memory");
}


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

	// Not fatal. A bridge that will not go into pass-through is a bridge whose
	// devices have to use programmed I/O, which is slower and works; refusing to
	// initialise would mean no disk at all.
	driver->_SetUpDma();

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
	// The bridge's own control registers, which the "reg" property points at --
	// two cells of address, because the root node's #address-cells is 2 on a
	// machine with more physical address space than one cell holds.
	uint32 reg[2];
	if (of_getprop(node, "reg", reg, sizeof(reg)) != OF_FAILED)
		fRegisterBase = ((phys_addr_t)reg[0] << 32) | reg[1];

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


/*!	Finds the Open Firmware node for a PCI function.

	A PCI child's "reg" property begins with the configuration-space address of
	the function, which packs the bus, device and function exactly as a type 1
	configuration address does. So the node can be found by the same number the
	bus manager asks about, with no name matching and no assumption about where
	in the tree it sits.

	Depth-first from the bridge, because a function can be behind a
	PCI-to-PCI bridge -- which on this machine every function is: sabre's own bus
	holds nothing but the two halves of the simba.
*/
static intptr_t
find_pci_node(intptr_t node, uint32 configurationAddress, int depth)
{
	const int kMaxDepth = 8;
	if (depth > kMaxDepth)
		return 0;

	for (; node != 0 && node != OF_FAILED; node = of_peer(node)) {
		uint32 reg[8];
		intptr_t length = of_getprop(node, "reg", reg, sizeof(reg));
		if (length != OF_FAILED && length >= (intptr_t)sizeof(uint32)
			&& (reg[0] & PCI_CONFIG_ADDRESS_MASK) == configurationAddress) {
			return node;
		}

		intptr_t found = find_pci_node(of_child(node), configurationAddress,
			depth + 1);
		if (found != 0)
			return found;
	}

	return 0;
}


/*!	Which interrupt a PCI function is wired to, as the host bridge numbers them.

	This is a translation, not a lookup: what a device knows is its own pin, one
	of INTA to INTD, and what the bridge's mapping registers are indexed by is an
	Interrupt Number Offset. The thing that connects them is the "interrupt-map"
	property on the device's parent, and it is the parent's because the wiring it
	describes is the parent's wiring.

	An interrupt-map entry is a child unit address, a child interrupt specifier,
	the parent's phandle, and the parent's interrupt specifier. Only the bits the
	"interrupt-map-mask" selects take part in the comparison, which is how a
	multifunction device shares one line: on this machine the mask keeps the bus
	and device numbers and drops the function, so the ebus and the Ethernet
	controller -- functions 0 and 1 of the same device -- both resolve to INO
	0x21.

	The INO is returned bare, without the interrupt group number. The kernel's
	interrupt controller adds that when it programs a mapping register, because
	the group is a property of the bridge rather than of the device, and keeping
	it out of the number a driver sees means the number a driver sees is the one
	the firmware published.

	Declining is better than guessing. A wrong interrupt is a driver that installs
	a handler and waits forever, which is a much harder thing to recognise than a
	driver told it has no interrupt -- and the disk stack does have a polling
	fallback to be told that with.
*/
status_t
SabrePCIController::ReadIrq(uint8 bus, uint8 device, uint8 function, uint8 pin,
	uint8& irq)
{
	if (pin == 0)
		return B_BAD_VALUE;

	uint32 configurationAddress = ((uint32)bus << 16)
		| ((uint32)(device & 0x1f) << 11) | ((uint32)(function & 0x07) << 8);

	intptr_t node = find_pci_node(of_child(fOpenFirmwareNode),
		configurationAddress, 0);
	if (node == 0)
		return B_ENTRY_NOT_FOUND;

	intptr_t parent = of_parent(node);
	if (parent == 0 || parent == OF_FAILED)
		return B_ENTRY_NOT_FOUND;

	// The mask's length is the child half of an entry: a PCI unit address is
	// three cells and its interrupt specifier one, but reading the length rather
	// than assuming four is what makes this work unchanged if a machine ever
	// describes itself differently.
	uint32 mask[8];
	intptr_t maskLength = of_getprop(parent, "interrupt-map-mask", mask,
		sizeof(mask));
	if (maskLength == OF_FAILED || maskLength < (intptr_t)(4 * sizeof(uint32)))
		return B_ENTRY_NOT_FOUND;

	uint32 maskCells = maskLength / sizeof(uint32);

	// The parent's specifier is as many cells as the parent says. One, on every
	// sun4u bridge; asking costs nothing and removes an assumption.
	uint32 parentCells = 1;
	of_getprop(parent, "#interrupt-cells", &parentCells, sizeof(parentCells));
	if (parentCells < 1 || parentCells > 4)
		parentCells = 1;

	uint32 map[PCI_MAX_INTERRUPT_MAP_CELLS];
	intptr_t mapLength = of_getprop(parent, "interrupt-map", map, sizeof(map));
	if (mapLength == OF_FAILED)
		return B_ENTRY_NOT_FOUND;

	if (of_getproplen(parent, "interrupt-map") > (intptr_t)sizeof(map)) {
		dprintf("sabre: \"interrupt-map\" is longer than %d cells; the rest is "
			"ignored\n", PCI_MAX_INTERRUPT_MAP_CELLS);
	}

	uint32 mapCells = mapLength / sizeof(uint32);
	uint32 entryCells = maskCells + 1 + parentCells;

	for (uint32 i = 0; i + entryCells <= mapCells; i += entryCells) {
		// The child unit address, then its interrupt specifier. Both sides are
		// masked, because an entry may carry bits outside the mask and the
		// comparison is only over what the mask keeps.
		if ((map[i] & mask[0]) != (configurationAddress & mask[0]))
			continue;

		bool matches = true;
		for (uint32 j = 1; j < maskCells - 1; j++) {
			if ((map[i + j] & mask[j]) != 0) {
				matches = false;
				break;
			}
		}
		if (!matches)
			continue;

		if ((map[i + maskCells - 1] & mask[maskCells - 1])
			!= ((uint32)pin & mask[maskCells - 1])) {
			continue;
		}

		irq = map[i + maskCells + 1] & INR_INO_MASK;
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
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


/*!	Puts the bridge into pass-through mode, so a physical address is a DMA address.

	Haiku hands drivers physical addresses and expects a device to reach them,
	which on this machine is true only in pass-through mode. Two things have to
	hold for that: the address must hit the target address space register, and
	translation must be off.

	Neither is inherited rather than asserted. What the firmware actually leaves
	behind here is translation already disabled -- so the second condition comes
	free -- but a target address space register of 0x40, which enables region 6
	and *not* the low gigabyte where DRAM lives. That half matters: an address
	that misses the register is not translated and not passed through, it is
	treated as a peer-to-peer transfer and ignored by the bridge entirely. So a
	DMA to DRAM would have gone nowhere.

	Hence the order below -- enable the regions first, then disable translation.
	Doing it the other way round would open a window in which a transfer already
	in flight is neither translated nor claimed.

	The low gigabyte, unconditionally, because that is every address DRAM can have
	on this processor: the IOMMU TSB base register documents physical address bits
	33:30 as "always zero, since only 1-Gbyte of physical memory is supported".
	Enabling more would have the bridge claim PCI addresses no DRAM answers to,
	which the manual warns against -- "no other PCI device should be enabled to
	respond to the UltraSPARC-IIi target address space".

	Writing the control register at all, when the bit it clears is already clear,
	is deliberate: it makes the mode this driver requires a statement rather than
	an assumption about what ran before us, and the read-back below turns a wrong
	assumption into a diagnostic instead of silent memory corruption. Doing so is
	safe by this point because nothing the kernel asks Open Firmware for uses DMA.
	Console output is programmed I/O to a 16550 and property reads are interpreter
	work; the disk driver the firmware used has long since stopped being the one
	in charge.

	Note that QEMU does not model the target address space register -- its sun4u
	IOMMU passes any address straight through once translation is off -- so the
	half of this that silicon needs is the half QEMU cannot confirm.
*/
status_t
SabrePCIController::_SetUpDma()
{
	if (fRegisterBase == 0) {
		dprintf("sabre: no register block, so DMA cannot be set up\n");
		return B_ERROR;
	}

	phys_addr_t control = fRegisterBase + SABRE_IOMMU_CONTROL;
	phys_addr_t target = fRegisterBase + SABRE_TARGET_ADDRESS_SPACE;

	uint64 wasControl = read_bridge_register(control);
	uint64 wasTarget = read_bridge_register(target);

	// One bit per 512 MB region, and DRAM occupies at most the low two.
	uint32 regions = 1024 * 1024 * 1024 / TARGET_ADDRESS_SPACE_GRANULE;
	uint64 enable = (1ULL << regions) - 1;

	write_bridge_register(target, wasTarget | enable);
	write_bridge_register(control, wasControl
		& ~(IOMMU_CONTROL_ENABLE | IOMMU_CONTROL_DIAGNOSTIC));

	uint64 nowControl = read_bridge_register(control);
	uint64 nowTarget = read_bridge_register(target);

	dprintf("sabre: iommu control %#" B_PRIx64 " -> %#" B_PRIx64 ", target "
		"address space %#" B_PRIx64 " -> %#" B_PRIx64 "\n", wasControl,
		nowControl, wasTarget, nowTarget);

	// Said plainly rather than left to be inferred from two hex numbers, because
	// this is the difference between a DMA that lands where the driver said and
	// one that lands somewhere else.
	if ((nowControl & IOMMU_CONTROL_ENABLE) != 0) {
		dprintf("sabre: iommu translation is still on; DMA addresses are not "
			"physical addresses and DMA must stay disabled\n");
		return B_ERROR;
	}

	if ((nowTarget & enable) != enable) {
		dprintf("sabre: target address space register did not take the low "
			"gigabyte; DMA would be ignored rather than translated\n");
		return B_ERROR;
	}

	dprintf("sabre: DMA is pass-through -- a physical address is a bus "
		"address\n");

	return B_OK;
}


/*!	Writes each device's resolved interrupt number into its configuration space.

	This is what actually makes interrupt routing take effect, and it is the
	controller's job rather than the bus manager's: nothing calls
	read_pci_irq() -- the interface has it, but the mechanism is that a
	controller walks its own devices here and pushes the answer in with
	update_interrupt_line(). ECAMPCIControllerFDT::Finalize() does the same for
	the flattened-device-tree platforms.

	What is in the interrupt line register before this runs is whatever the
	firmware left, and on this machine that is the PCI pin rather than anything a
	handler can be installed on: the CMD646 reads back 3 and the Ethernet 1. A
	driver installing on those would be waiting on interrupts belonging to other
	devices.

	Failing to resolve one is reported per device and not fatal. A device with no
	interrupt-map entry is a device nothing has wiring for, which is a fact about
	the machine rather than an error, and leaving its line alone is better than
	writing a number that means something else.
*/
status_t
SabrePCIController::Finalize()
{
	for (uint8 bus = fFirstBus; bus <= fLastBus; bus++) {
		for (uint8 device = 0; device < 32; device++) {
			uint32 id;
			if (ReadConfig(bus, device, 0, PCI_vendor_id, 4, id) != B_OK)
				continue;
			if ((id & 0xffff) == 0xffff || (id & 0xffff) == 0)
				continue;

			// A single-function device answers for function 0 only, and reading
			// the others would be reading the same registers again.
			uint32 headerType = 0;
			ReadConfig(bus, device, 0, PCI_header_type, 1, headerType);
			uint8 functions = (headerType & PCI_multifunction) != 0 ? 8 : 1;

			for (uint8 function = 0; function < functions; function++) {
				_FinalizeInterrupt(bus, device, function);
			}
		}

		// fLastBus is inclusive and a uint8, so a bridge reporting 255
		// subordinate buses would wrap the loop rather than end it.
		if (bus == 0xff)
			break;
	}

	return B_OK;
}


void
SabrePCIController::_FinalizeInterrupt(uint8 bus, uint8 device, uint8 function)
{
	uint32 pin = 0;
	if (ReadConfig(bus, device, function, PCI_interrupt_pin, 1, pin) != B_OK)
		return;
	if (pin == 0 || pin == 0xff)
		return;

	uint8 irq;
	status_t status = ReadIrq(bus, device, function, (uint8)pin, irq);
	if (status != B_OK) {
		dprintf("sabre: no interrupt for %u:%u:%u pin %u: %s\n", bus, device,
			function, (unsigned)pin, strerror(status));
		return;
	}

	dprintf("sabre: %u:%u:%u pin %u is interrupt %u\n", bus, device, function,
		(unsigned)pin, irq);

	gPCI->update_interrupt_line(bus, device, function, irq);
}
