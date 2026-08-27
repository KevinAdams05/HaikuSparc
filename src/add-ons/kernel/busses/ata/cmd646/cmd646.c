/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

/*!	CMD Technology PCI0646 -- the Ultra 5/10's IDE controller.

	`generic_ide_pci` already drives this part correctly, and everything below is
	a forwarding shim to the same shared code, with one exception: the interrupt
	handler clears a latch that only this chip has.

	**What it is for.** The PCI0646 keeps its own per-channel interrupt status
	bit, separate from the bus master status bit that `ata_adapter_inthand()`
	checks and clears. The primary channel's is CFR bit 2 at configuration offset
	0x50, the secondary's is ARTTIM23 bit 4 at 0x57, and both are cleared by being
	*read* -- "Read CFR will clear this bit", and the same sentence for ARTTIM23,
	in the PCI0646 specification's IDE Timing Control Registers. Reading is the
	whole operation; there is nothing to write back. Linux's `pata_cmd64x` says so
	twice in comments and does exactly this, which is the second opinion that
	settled it against a note in this port's own log claiming the bits were
	"written back to clear".

	**It cannot be verified in emulation, and it fixes nothing there.** QEMU's
	`cmd646-ide` sets CFR bit 2 and does not implement read-to-clear: two
	consecutive reads from an interrupt handler return 0x4 and 0x4. Nothing in
	QEMU acts on the bit either, so on the emulator this read is inert.

	It is here anyway, for the reason the porting plan's section 5.5 gives about
	this whole category: the datasheet documents the latch, Linux clears it, and
	the failure mode on silicon that does implement it is an interrupt that will
	not go away -- which is a miserable thing to diagnose on a machine whose only
	channel is a serial cable. Written from the datasheet while the reasoning is
	fresh, to be confirmed when an Ultra 10 is on the bench.

	**What it does not fix, despite the note that asked for it.** This port
	recorded a defect of "one extra interrupt per transfer that returns
	B_UNHANDLED_INTERRUPT", and attributed it to this latch holding the PCI line
	asserted. Measured per channel, that is not what is happening:

	    ch0 0/90 unhandled, ch1 90/102 unhandled

	Every interrupt on the active channel is handled. The unhandled ones are all
	on the *idle* channel, whose handler is registered on the same vector -- both
	channels are multiplexed onto INTA -- and which correctly reports that the
	interrupt was not its own. That is not a defect and there is nothing to fix;
	it is what sharing a vector between two channel handlers looks like. Reading
	the latch changes the ratio not at all, which is how it was ruled out.

	**Why it does not decide whether the interrupt was ours.** The specification
	describes polling both registers to find which channel interrupted. This
	handler reads the latch and then delegates unconditionally rather than using
	it as a gate: the shared handler already decides that question from the bus
	master status, which is the register the rest of the ATA stack is written
	against, and having two sources of that answer is how a real interrupt gets
	dropped and a transfer hangs. Clearing the latch is the whole job here.
*/

#include <KernelExport.h>
#include <ata_adapter.h>
#include <bus/PCI.h>

#include <stdlib.h>
#include <string.h>


#define CMD646_MODULE_NAME "busses/ata/cmd646/driver_v1"
#define CMD646_CHANNEL_MODULE_NAME "busses/ata/cmd646/channel/v1"

// CMD Technology, which Silicon Image later bought -- the vendor id is shared
// with the SiI parts that silicon_image_3112 drives.
#define PCI_VENDOR_CMD			0x1095
#define PCI_DEVICE_CMD_646		0x0646

// Configuration (R), offset 0x50. Bit 2 is the drive 0/1 interrupt status, and
// reading the register clears it.
#define CMD646_CFR				0x50
#define CMD646_CFR_INTR_CH0		0x04

// Drive 2/3 Control/Status (RW), offset 0x57. Bit 4 is the drive 2/3 interrupt
// status, cleared the same way.
#define CMD646_ARTTIM23			0x57
#define CMD646_ARTTIM23_INTR_CH1	0x10


/*!	A channel, plus the one thing the shared structure does not carry.

	Which of the two channels this is decides which register holds its interrupt
	latch, and `ata_adapter_channel_info` has no index -- it is identified by its
	register bases, not by a number. The device node does carry one, so it is read
	once at init rather than derived from an address every interrupt.

	The shared structure has to come first: the adapter code is handed this
	pointer and reads it as its own type.
*/
typedef struct {
	ata_adapter_channel_info	ataAdapter;
	uint8						index;
} cmd646_channel_info;


static ata_for_controller_interface* sATA;
static ata_adapter_interface* sATAAdapter;
static device_manager_info* sDeviceManager;


static void
set_channel(void* cookie, ata_channel channel)
{
	sATAAdapter->set_channel((ata_adapter_channel_info*)cookie, channel);
}


static status_t
write_command_block_regs(void* channel_cookie, ata_task_file* tf,
	ata_reg_mask mask)
{
	return sATAAdapter->write_command_block_regs(
		(ata_adapter_channel_info*)channel_cookie, tf, mask);
}


static status_t
read_command_block_regs(void* channel_cookie, ata_task_file* tf,
	ata_reg_mask mask)
{
	return sATAAdapter->read_command_block_regs(
		(ata_adapter_channel_info*)channel_cookie, tf, mask);
}


static uint8
get_altstatus(void* channel_cookie)
{
	return sATAAdapter->get_altstatus(
		(ata_adapter_channel_info*)channel_cookie);
}


static status_t
write_device_control(void* channel_cookie, uint8 val)
{
	return sATAAdapter->write_device_control(
		(ata_adapter_channel_info*)channel_cookie, val);
}


static status_t
write_pio(void* channel_cookie, uint16* data, int count, bool force_16bit)
{
	return sATAAdapter->write_pio((ata_adapter_channel_info*)channel_cookie,
		data, count, force_16bit);
}


static status_t
read_pio(void* channel_cookie, uint16* data, int count, bool force_16bit)
{
	return sATAAdapter->read_pio((ata_adapter_channel_info*)channel_cookie,
		data, count, force_16bit);
}


static status_t
prepare_dma(void* channel_cookie, const physical_entry* sg_list,
	size_t sg_list_count, bool write)
{
	return sATAAdapter->prepare_dma((ata_adapter_channel_info*)channel_cookie,
		sg_list, sg_list_count, write);
}


static status_t
start_dma(void* channel_cookie)
{
	return sATAAdapter->start_dma((ata_adapter_channel_info*)channel_cookie);
}


static status_t
finish_dma(void* channel_cookie)
{
	return sATAAdapter->finish_dma((ata_adapter_channel_info*)channel_cookie);
}


/*!	Clears this chip's own interrupt latch, then hands over to the shared code.

	See the note at the top of the file for why reading is the whole operation
	and why the result is not used to decide anything.
*/
static int32
cmd646_inthand(void* arg)
{
	cmd646_channel_info* channel = (cmd646_channel_info*)arg;

	channel->ataAdapter.pci->read_pci_config(channel->ataAdapter.device,
		channel->index == 0 ? CMD646_CFR : CMD646_ARTTIM23, 1);

	return sATAAdapter->inthand(arg);
}


static status_t
init_channel(device_node* node, void** channel_cookie)
{
	cmd646_channel_info* channel;
	uint8 index;
	status_t status;

	if (sDeviceManager->get_attr_uint8(node, ATA_ADAPTER_CHANNEL_INDEX, &index,
			false) != B_OK) {
		return B_ERROR;
	}

	status = sATAAdapter->init_channel(node,
		(ata_adapter_channel_info**)channel_cookie,
		sizeof(cmd646_channel_info), cmd646_inthand);
	if (status != B_OK)
		return status;

	// After init_channel, not before: it is what allocates the structure.
	channel = (cmd646_channel_info*)*channel_cookie;
	channel->index = index;

	// Said once per channel, because otherwise nothing in a boot log
	// distinguishes this driver from generic_ide_pci -- both reach the same
	// shared code, which is what prints everything else about the controller.
	dprintf("cmd646: channel %" B_PRIu8 ", interrupt latch at %#x\n", index,
		index == 0 ? CMD646_CFR : CMD646_ARTTIM23);

	return B_OK;
}


static void
uninit_channel(void* channel_cookie)
{
	sATAAdapter->uninit_channel((ata_adapter_channel_info*)channel_cookie);
}


static void
channel_removed(void* channel_cookie)
{
	sATAAdapter->channel_removed((ata_adapter_channel_info*)channel_cookie);
}


static status_t
init_controller(device_node* node, ata_adapter_controller_info** cookie)
{
	return sATAAdapter->init_controller(node, cookie,
		sizeof(ata_adapter_controller_info));
}


static void
uninit_controller(ata_adapter_controller_info* controller)
{
	sATAAdapter->uninit_controller(controller);
}


static void
controller_removed(ata_adapter_controller_info* controller)
{
	sATAAdapter->controller_removed(controller);
}


/*!	The same parameters generic_ide_pci uses, and for the same reasons.

	Nothing about this part's DMA engine differs from the specification's
	defaults: scatter/gather blocks up to 64 KB that may not cross a 64 KB
	boundary, and 16-bit alignment. It is a conventional PCI IDE controller
	everywhere except its interrupt latch.
*/
static status_t
probe_controller(device_node* parent)
{
	return sATAAdapter->probe_controller(parent, CMD646_MODULE_NAME, "cmd646",
		"CMD PCI0646 IDE Controller", CMD646_CHANNEL_MODULE_NAME,
		true,
		true,					// assume that command queuing works
		1,						// assume 16 bit alignment is enough
		0xffff,					// boundary is on 64k according to spec
		0x10000,				// up to 64k per S/G block according to spec
		true);					// by default, compatibility mode is used
}


static float
supports_device(device_node* parent)
{
	const char* bus;
	uint16 vendorID;
	uint16 deviceID;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
			&vendorID, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &deviceID,
			false) != B_OK) {
		return -1.0f;
	}

	if (strcmp(bus, "pci") != 0 || vendorID != PCI_VENDOR_CMD
		|| deviceID != PCI_DEVICE_CMD_646) {
		return 0.0f;
	}

	// Above generic_ide_pci's 0.3, which also matches this device and drives it
	// correctly apart from the latch.
	return 1.0f;
}


module_dependency module_dependencies[] = {
	{ ATA_FOR_CONTROLLER_MODULE_NAME, (module_info**)&sATA },
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ ATA_ADAPTER_MODULE_NAME, (module_info**)&sATAAdapter },
	{}
};


static ata_controller_interface channel_interface = {
	{
		{
			CMD646_CHANNEL_MODULE_NAME,
			0,
			NULL
		},

		NULL,	// supports device
		NULL,	// register device
		init_channel,
		uninit_channel,
		NULL,	// register child devices
		NULL,	// rescan
		channel_removed,
	},

	&set_channel,

	&write_command_block_regs,
	&read_command_block_regs,

	&get_altstatus,
	&write_device_control,

	&write_pio,
	&read_pio,

	&prepare_dma,
	&start_dma,
	&finish_dma,
};


static driver_module_info controller_interface = {
	{
		CMD646_MODULE_NAME,
		0,
		NULL
	},

	supports_device,
	probe_controller,
	(status_t (*)(device_node *, void**))	init_controller,
	(void (*)(void*))						uninit_controller,
	NULL,	// register child devices
	NULL,	// rescan
	(void (*)(void*))						controller_removed,
};

module_info* modules[] = {
	(module_info*)&controller_interface,
	(module_info*)&channel_interface,
	NULL
};
