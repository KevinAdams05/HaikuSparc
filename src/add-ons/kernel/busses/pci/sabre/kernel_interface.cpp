/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */


#include "SabrePCIController.h"


device_manager_info* gDeviceManager;


static pci_controller_module_info sSabrePCIController = {
	.info = {
		.info = {
			.name = SABRE_PCI_DRIVER_MODULE_NAME,
		},
		.supports_device = SabrePCIController::SupportsDevice,
		.register_device = SabrePCIController::RegisterDevice,
		.init_driver = [](device_node* node, void** driverCookie) {
			return SabrePCIController::InitDriver(node,
				*(SabrePCIController**)driverCookie);
		},
		.uninit_driver = [](void* driverCookie) {
			return static_cast<SabrePCIController*>(driverCookie)
				->UninitDriver();
		},
	},

	.read_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32* value) {
		return static_cast<SabrePCIController*>(cookie)
			->ReadConfig(bus, device, function, offset, size, *value);
	},
	.write_pci_config = [](void* cookie, uint8 bus, uint8 device,
		uint8 function, uint16 offset, uint8 size, uint32 value) {
		return static_cast<SabrePCIController*>(cookie)
			->WriteConfig(bus, device, function, offset, size, value);
	},
	.get_max_bus_devices = [](void* cookie, int32* count) {
		return static_cast<SabrePCIController*>(cookie)
			->GetMaxBusDevices(*count);
	},
	.read_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8* irq) {
		return static_cast<SabrePCIController*>(cookie)
			->ReadIrq(bus, device, function, pin, *irq);
	},
	.write_pci_irq = [](void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint8 irq) {
		return static_cast<SabrePCIController*>(cookie)
			->WriteIrq(bus, device, function, pin, irq);
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		return static_cast<SabrePCIController*>(cookie)
			->GetRange(index, range);
	},
	.finalize = [](void* cookie) {
		return static_cast<SabrePCIController*>(cookie)->Finalize();
	}
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};

_EXPORT module_info* modules[] = {
	(module_info*)&sSabrePCIController,
	NULL
};
