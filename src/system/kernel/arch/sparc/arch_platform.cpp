/*
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * Copyright 2019-2020, Adrien Destugues, pulkomandy@pulkomandy.tk.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <arch_platform.h>

#include <new>
#include <string.h>

#include <KernelExport.h>

#include <arch/generic/debug_uart.h>
#include <boot/kernel_args.h>
#include <platform/openfirmware/openfirmware.h>
#include <real_time_clock.h>
#include <util/kernel_cpp.h>


static SparcPlatform *sSparcPlatform;


// constructor
SparcPlatform::SparcPlatform(sparc_platform_type platformType)
	: fPlatformType(platformType)
{
}

// destructor
SparcPlatform::~SparcPlatform()
{
}

// Default
SparcPlatform *
SparcPlatform::Default()
{
	return sSparcPlatform;
}


// #pragma mark - Open Firmware


namespace BPrivate {

class SparcOpenFirmware : public SparcPlatform {
public:
	SparcOpenFirmware();
	virtual ~SparcOpenFirmware();

	virtual status_t Init(struct kernel_args *kernelArgs);
	virtual status_t InitSerialDebug(struct kernel_args *kernelArgs);
	virtual status_t InitPostVM(struct kernel_args *kernelArgs);
	virtual status_t InitRTC(struct kernel_args *kernelArgs,
		struct real_time_data *data);

	virtual char SerialDebugGetChar();
	virtual void SerialDebugPutChar(char c);

	virtual	void SetHardwareRTC(uint64 seconds);
	virtual	uint32 GetHardwareRTC();

	virtual	void ShutDown(bool reboot);

private:
	int	fInput;
	int	fOutput;
	int	fRTC;
};

}	// namespace BPrivate

using BPrivate::SparcOpenFirmware;


// OF debugger commands

// debug_command_of_exit
static int
debug_command_of_exit(int argc, char **argv)
{
	of_exit();
	kprintf("of_exit() failed!\n");
	return 0;
}

// debug_command_of_enter
static int
debug_command_of_enter(int argc, char **argv)
{
	of_call_client_function("enter", 0, 0);
	return 0;
}


// constructor
SparcOpenFirmware::SparcOpenFirmware()
	: SparcPlatform(SPARC_PLATFORM_OPEN_FIRMWARE),
	  fInput(-1),
	  fOutput(-1),
	  fRTC(-1)
{
}

// destructor
SparcOpenFirmware::~SparcOpenFirmware()
{
}

// Init
status_t
SparcOpenFirmware::Init(struct kernel_args *kernelArgs)
{
	return of_init(
		(intptr_t(*)(void*))kernelArgs->platform_args.openfirmware_entry);
}

// InitSerialDebug
status_t
SparcOpenFirmware::InitSerialDebug(struct kernel_args *kernelArgs)
{
	if (of_getprop(gChosen, "stdin", &fInput, sizeof(int)) == OF_FAILED)
		return B_ERROR;

	int output;
	if (of_getprop(gChosen, "stdout", &output, sizeof(int)) == OF_FAILED)
		return B_ERROR;

	// Previously stdout was only fetched when there was no frame buffer, which
	// left fOutput at -1 and silently discarded every byte of kernel debug
	// output on any machine that had one -- including every machine the loader
	// sets a video mode on.
	//
	// The caution behind that was reasonable, though: if Open Firmware's stdout
	// is the screen, writing through it while Haiku is drawing to the same
	// frame buffer corrupts the display. So suppress it only in that case, and
	// decide by asking the device what it is rather than by inferring it. A
	// machine consoled over serial -- which is how this port is developed, and
	// how a headless Sun is usually run -- keeps its debug output.
	bool outputIsDisplay = false;
	if (kernelArgs->frame_buffer.enabled) {
		intptr_t package = of_instance_to_package(output);
		if (package != OF_FAILED) {
			char type[16];
			if (of_getprop(package, "device_type", type, sizeof(type))
					!= OF_FAILED) {
				type[sizeof(type) - 1] = '\0';
				outputIsDisplay = strcmp(type, "display") == 0;
			}
		}
	}

	if (!outputIsDisplay)
		fOutput = output;

	return B_OK;
}

// InitPostVM
status_t
SparcOpenFirmware::InitPostVM(struct kernel_args *kernelArgs)
{
	add_debugger_command("of_exit", &debug_command_of_exit,
		"Exit to the Open Firmware prompt. No way to get back into the OS!");
	add_debugger_command("of_enter", &debug_command_of_enter,
		"Enter a subordinate Open Firmware interpreter. Quitting it returns "
		"to KDL.");

	return B_OK;
}

// InitRTC
status_t
SparcOpenFirmware::InitRTC(struct kernel_args *kernelArgs,
	struct real_time_data *data)
{
	// open RTC
	fRTC = of_open(kernelArgs->platform_args.rtc_path);
	if (fRTC == OF_FAILED) {
		dprintf("SparcOpenFirmware::InitRTC(): Failed open RTC device!\n");
		return B_ERROR;
	}

	return B_OK;
}

// DebugSerialGetChar
char
SparcOpenFirmware::SerialDebugGetChar()
{
	// intptr_t, not int. of_interpret() returns values by writing through the
	// caller's pointer as a void**, which is an eight-byte store: passing the
	// address of an int both writes four bytes past it and, on a strict-alignment
	// architecture, raises mem_address_not_aligned whenever that int does not
	// happen to be eight-byte aligned.
	//
	// This is why the kernel debugger has never accepted a keypress on this
	// port. Typing anything at the "kdebug>" prompt faulted inside
	// of_interpret(), which panicked, which re-entered the debugger, which
	// prompted again.
	//
	// The same mistake as the of_open() handle truncation found in Phase 1, in
	// the same interface and for the same reason: Open Firmware deals in
	// pointer-width values, and int is not one on a 64-bit machine. PowerPC's
	// copy of this function has it too, and is harmless there only because int
	// and intptr_t coincide.
	intptr_t key = 0;
	if (of_interpret("key", 0, 1, &key) == OF_FAILED)
		return 0;
	return (char)key;
}

// DebugSerialPutChar
void
SparcOpenFirmware::SerialDebugPutChar(char c)
{
	if (fOutput == -1)
		return;

	if (c == '\n')
		of_write((uint32_t)fOutput, "\r\n", 2);
	else
		of_write((uint32_t)fOutput, &c, 1);
}

// SetHardwareRTC
void
SparcOpenFirmware::SetHardwareRTC(uint64 seconds)
{
	struct tm t;
	rtc_secs_to_tm(seconds, &t);

	t.tm_year += RTC_EPOCH_BASE_YEAR;
	t.tm_mon++;

	if (of_call_method((uint32_t)fRTC, "set-time", 6, 0, t.tm_year, t.tm_mon, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec) == OF_FAILED) {
		dprintf("SparcOpenFirmware::SetHardwareRTC(): Failed to set RTC!\n");
	}
}

// GetHardwareRTC
uint32
SparcOpenFirmware::GetHardwareRTC()
{
	struct tm t;
	if (of_call_method((uint32_t)fRTC, "get-time", 0, 6, &t.tm_year, &t.tm_mon,
			&t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == OF_FAILED) {
		dprintf("SparcOpenFirmware::GetHardwareRTC(): Failed to get RTC!\n");
		return 0;
	}

	t.tm_year -= RTC_EPOCH_BASE_YEAR;
	t.tm_mon--;

	return rtc_tm_to_secs(&t);
}

// ShutDown
void
SparcOpenFirmware::ShutDown(bool reboot)
{
	if (reboot) {
		of_interpret("reset-all", 0, 0);
	} else {
		// not standardized, so it might fail
		of_interpret("shut-down", 0, 0);
	}
}


// # pragma mark -


#define PLATFORM_BUFFER_SIZE sizeof(SparcOpenFirmware)
// static buffer for constructing the actual SparcPlatform
static char *sSparcPlatformBuffer[PLATFORM_BUFFER_SIZE];

status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	// only OpenFirmware supported for now
	sSparcPlatform = new(sSparcPlatformBuffer) SparcOpenFirmware;

	return sSparcPlatform->Init(kernelArgs);
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	return sSparcPlatform->InitPostVM(kernelArgs);
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	sparc_dump_openfirmware_devices();

	return B_OK;
}


/*!	Walks the Open Firmware device tree, reporting what the machine has.

	The first step of the device stack, and the reason it comes first: on sun4u
	the firmware has already probed everything and describes it in a tree, with
	the PCI configuration values the kernel would otherwise have to go and read
	itself. A bus manager built on that starts from a list of devices rather than
	from a scan.

	Printed depth-first with the properties that identify a device. "reg" on a PCI
	node begins with the configuration-space address of the function -- bus,
	device and function packed together -- which is what a config-space accessor
	needs as its handle.

	Bounded by depth as well as by the tree, because a malformed sibling chain
	would otherwise walk forever, and this runs before there is anything to catch
	that.
*/
static void
sparc_dump_device_tree(intptr_t node, int depth)
{
	const int kMaxDepth = 8;
	if (depth > kMaxDepth)
		return;

	for (; node != 0 && node != OF_FAILED; node = of_peer(node)) {
		char name[64];
		char type[32];
		uint32 reg[8];

		if (of_getprop(node, "name", name, sizeof(name)) == OF_FAILED)
			strlcpy(name, "?", sizeof(name));
		if (of_getprop(node, "device_type", type, sizeof(type)) == OF_FAILED)
			type[0] = '\0';

		// Vendor and device ids are separate properties on Open Firmware rather
		// than something to be read out of configuration space.
		uint32 vendor = 0;
		uint32 device = 0;
		bool isPci = of_getprop(node, "vendor-id", &vendor, sizeof(vendor))
				!= OF_FAILED
			&& of_getprop(node, "device-id", &device, sizeof(device))
				!= OF_FAILED;

		intptr_t regLength = of_getprop(node, "reg", reg, sizeof(reg));

		dprintf("%*s%s", depth * 2 + 1, " ", name);
		if (type[0] != '\0')
			dprintf(" (%s)", type);
		if (isPci)
			dprintf(" %04" B_PRIx32 ":%04" B_PRIx32, vendor, device);
		if (regLength != OF_FAILED && regLength >= (intptr_t)sizeof(uint32))
			dprintf(" reg %08" B_PRIx32, reg[0]);
		dprintf("\n");

		sparc_dump_device_tree(of_child(node), depth + 1);
	}
}


/*	Where sabre puts the PCI address spaces.
 *
 *	Read from the host bridge's "ranges" property rather than hardcoded, because
 *	that property is the machine describing itself. Each entry is seven cells: a
 *	three-cell PCI address, a two-cell physical address, and a two-cell size. The
 *	top two bits of the PCI address's first cell say which space it is -- 00
 *	configuration, 01 I/O, 10 thirty-two-bit memory -- which is the PCI Open
 *	Firmware binding, not something sabre invented.
 *
 *	On the machine this was written against those come out as configuration at
 *	physical 0x1fe.01000000 for 16 MB, I/O at 0x1fe.02000000, and memory at
 *	0x1ff.00000000, with buses 0 to 2. Which matches what the sabre documentation
 *	says, and is worth reading from the tree anyway: the Blade 150 is a different
 *	machine with the same bridge, and this is exactly the kind of constant that
 *	turns out to differ.
 */
#define PCI_RANGE_CELLS			7
#define PCI_SPACE_MASK			0x03000000
#define PCI_SPACE_CONFIGURATION		0x00000000
#define PCI_SPACE_IO			0x01000000
#define PCI_SPACE_MEMORY32		0x02000000

// Physical, non-cacheable, little endian. All three matter: configuration space
// is a device register block rather than memory, so it must not be cached, and
// PCI is little endian on a machine that is not.
#define ASI_PHYS_NON_CACHED_LITTLE	0x1d

static phys_addr_t sPciConfigurationBase;
static uint8 sPciLastBus;


static status_t
sparc_pci_read_ranges()
{
	intptr_t node = of_finddevice("/pci@1fe,0");
	if (node == OF_FAILED)
		return B_ENTRY_NOT_FOUND;

	uint32 ranges[PCI_RANGE_CELLS * 8];
	intptr_t length = of_getprop(node, "ranges", ranges, sizeof(ranges));
	if (length == OF_FAILED)
		return B_ERROR;

	uint32 cells = length / sizeof(uint32);
	for (uint32 i = 0; i + PCI_RANGE_CELLS <= cells; i += PCI_RANGE_CELLS) {
		uint32 space = ranges[i] & PCI_SPACE_MASK;
		phys_addr_t physical = ((phys_addr_t)ranges[i + 3] << 32)
			| ranges[i + 4];
		uint64 size = ((uint64)ranges[i + 5] << 32) | ranges[i + 6];

		const char *name = space == PCI_SPACE_CONFIGURATION ? "configuration"
			: space == PCI_SPACE_IO ? "I/O"
			: space == PCI_SPACE_MEMORY32 ? "memory" : "?";
		dprintf("arch_platform: pci %s space at %#" B_PRIxPHYSADDR ", %"
			B_PRIu64 " MB\n", name, physical, size / (1024 * 1024));

		if (space == PCI_SPACE_CONFIGURATION)
			sPciConfigurationBase = physical;
	}

	uint32 busRange[2];
	if (of_getprop(node, "bus-range", busRange, sizeof(busRange)) != OF_FAILED)
		sPciLastBus = (uint8)busRange[1];

	return sPciConfigurationBase != 0 ? B_OK : B_ERROR;
}


/*!	Reads a PCI configuration register.

	sabre maps configuration space linearly, so a register's physical address is
	the base plus the bus, device and function packed into the address the way the
	specification packs them. Sixteen megabytes is exactly enough for eight bits
	of bus.
*/
uint32
sparc_pci_read_config(uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size)
{
	if (sPciConfigurationBase == 0)
		return 0xffffffff;

	phys_addr_t address = sPciConfigurationBase + ((phys_addr_t)bus << 16)
		+ ((phys_addr_t)device << 11) + ((phys_addr_t)function << 8) + offset;

	uint64 value;
	switch (size) {
		case 1:
			asm volatile("lduba [%[a]] 0x1d, %[v]"
				: [v] "=r"(value) : [a] "r"(address));
			return (uint32)value;
		case 2:
			asm volatile("lduha [%[a]] 0x1d, %[v]"
				: [v] "=r"(value) : [a] "r"(address));
			return (uint32)value;
		default:
			asm volatile("lduwa [%[a]] 0x1d, %[v]"
				: [v] "=r"(value) : [a] "r"(address));
			return (uint32)value;
	}
}


void
sparc_pci_write_config(uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32 value)
{
	if (sPciConfigurationBase == 0)
		return;

	phys_addr_t address = sPciConfigurationBase + ((phys_addr_t)bus << 16)
		+ ((phys_addr_t)device << 11) + ((phys_addr_t)function << 8) + offset;

	switch (size) {
		case 1:
			asm volatile("stba %[v], [%[a]] 0x1d"
				: : [v] "r"(value), [a] "r"(address) : "memory");
			break;
		case 2:
			asm volatile("stha %[v], [%[a]] 0x1d"
				: : [v] "r"(value), [a] "r"(address) : "memory");
			break;
		default:
			asm volatile("stwa %[v], [%[a]] 0x1d"
				: : [v] "r"(value), [a] "r"(address) : "memory");
			break;
	}
}


/*!	Proves the configuration accessor against what the firmware already found.

	The device tree lists every device with its vendor and device id, because the
	firmware probed them itself. So there is a correct answer to compare against,
	which is worth having: an address formation that is wrong by a shift, or a
	byte order that is wrong, both produce plausible-looking numbers.
*/
static void
sparc_pci_scan()
{
	int found = 0;

	for (uint8 bus = 0; bus <= sPciLastBus; bus++) {
		for (uint8 device = 0; device < 32; device++) {
			for (uint8 function = 0; function < 8; function++) {
				uint32 id = sparc_pci_read_config(bus, device, function, 0, 4);
				if (id == 0xffffffff || id == 0)
					continue;

				uint32 classRevision = sparc_pci_read_config(bus, device,
					function, 0x08, 4);

				dprintf("arch_platform: pci %u:%u:%u %04x:%04x class %06x\n",
					bus, device, function, id & 0xffff, id >> 16,
					classRevision >> 8);
				found++;

				// A single-function device answers for function 0 only.
				if (function == 0) {
					uint32 header = sparc_pci_read_config(bus, device, 0, 0x0c,
						4);
					if ((header & 0x00800000) == 0)
						break;
				}
			}
		}
	}

	dprintf("arch_platform: %d pci functions found by configuration space\n",
		found);
}


void
sparc_dump_openfirmware_devices()
{
	if (sparc_pci_read_ranges() == B_OK)
		sparc_pci_scan();
	intptr_t root = of_finddevice("/");
	if (root == OF_FAILED) {
		dprintf("arch_platform: no Open Firmware root node\n");
		return;
	}

	dprintf("arch_platform: Open Firmware device tree:\n");
	sparc_dump_device_tree(of_child(root), 0);
}
