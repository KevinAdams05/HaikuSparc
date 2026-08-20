/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _SABRE_PCI_CONTROLLER_H_
#define _SABRE_PCI_CONTROLLER_H_


#include <bus/PCI.h>

#include <util/Vector.h>


#define SABRE_PCI_DRIVER_MODULE_NAME	"busses/pci/sabre/driver_v1"

// The Open Firmware phandle of the host bridge a node stands for, so a machine
// with more than one of them gets one driver instance per bridge rather than one
// instance that has to guess.
#define SABRE_PCI_NODE_ITEM				"sabre/of_node"


class SabrePCIController {
public:
	static	float			SupportsDevice(device_node* parent);
	static	status_t		RegisterDevice(device_node* parent);
	static	status_t		InitDriver(device_node* node,
								SabrePCIController*& _driver);
			void			UninitDriver();

			status_t		ReadConfig(uint8 bus, uint8 device, uint8 function,
								uint16 offset, uint8 size, uint32& value);
			status_t		WriteConfig(uint8 bus, uint8 device, uint8 function,
								uint16 offset, uint8 size, uint32 value);

			status_t		GetMaxBusDevices(int32& count);

			status_t		ReadIrq(uint8 bus, uint8 device, uint8 function,
								uint8 pin, uint8& irq);
			status_t		WriteIrq(uint8 bus, uint8 device, uint8 function,
								uint8 pin, uint8 irq);

			status_t		GetRange(uint32 index, pci_resource_range* range);

			status_t		Finalize();

private:
			status_t		_ReadRanges(intptr_t node);
			void			_FinalizeInterrupt(uint8 bus, uint8 device,
								uint8 function);
			phys_addr_t		_ConfigAddress(uint8 bus, uint8 device,
								uint8 function, uint16 offset);

			device_node*	fNode;
			intptr_t		fOpenFirmwareNode;

			phys_addr_t		fConfigurationBase;
			uint64			fConfigurationSize;
			uint8			fFirstBus;
			uint8			fLastBus;

			Vector<pci_resource_range> fResourceRanges;
};


extern device_manager_info* gDeviceManager;
extern pci_module_info* gPCI;


#endif	// _SABRE_PCI_CONTROLLER_H_
