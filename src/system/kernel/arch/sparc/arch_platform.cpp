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


void
sparc_dump_openfirmware_devices()
{
	intptr_t root = of_finddevice("/");
	if (root == OF_FAILED) {
		dprintf("arch_platform: no Open Firmware root node\n");
		return;
	}

	dprintf("arch_platform: Open Firmware device tree:\n");
	sparc_dump_device_tree(of_child(root), 0);
}
