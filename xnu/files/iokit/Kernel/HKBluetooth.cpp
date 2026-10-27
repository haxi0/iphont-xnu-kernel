/*
 * HKBluetooth: the registry entry by which iOS finds its Bluetooth controller (bt=1, board=instantnoodle).
 *
 * On an iPhone X it is the device tree's /arm-io/uart1/bluetooth, which the UART's driver publishes as
 * a service. bluetoothd and BlueTool look it up by name -- IOServiceNameMatching("bluetooth"), after
 * "marconi-bt" -- and read three of its properties, searching the IODeviceTree plane from it (the
 * entry itself first):
 *   transport-encoding  how HCI goes over the wire: 0 USB, 1 H4, 2 H4DS, 3 H4BC (the iPhone X's,
 *                       Broadcom's sleep signalling), 4 H5, 6 AppleBT, 7 PCIe (bluetoothd's table);
 *                       here 1, plain H4, which is what the QCA6390 speaks with in-band sleep off;
 *   transport-speed     the UART's rate, 3000000 (the chip's, set by bsd/dev/hk_bt.c);
 *   local-mac-address   the device's address, six bytes, first shown first: the loader's
 *                       /chosen mac-address-bluetooth0, which hk_bt.c also gives the chip.
 * The service has nothing but properties: the UART itself is the kernel control socket of
 * bsd/dev/hk_bt.c, the power /dev/btpoweroff.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

#include "MiniDrivers.h"

extern "C" bool hk_bt_wanted(void);
extern "C" void hk_bt_set_address(const uint8_t address[6]);

class HKBluetooth : public IOService
{
	OSDeclareDefaultStructors(HKBluetooth);
};

OSDefineMetaClassAndStructors(HKBluetooth, IOService);

static void
set_data(IOService * service, const char * key, const void * bytes, unsigned length)
{
	OSData * data = OSData::withBytes(bytes, length);

	if (data != NULL) {
		service->setProperty(key, data);
		data->release();
	}
}

void
HKBluetoothStart(void)
{
	IOService * platform = (IOService *) IOService::getPlatform();
	IORegistryEntry * chosen;
	HKBluetooth * bt;
	uint8_t address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	uint32_t encoding = 1, speed = 3000000;

	if (!hk_bt_wanted()) {
		return;
	}
	chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (chosen != NULL) {
		OSData * mac = OSDynamicCast(OSData, chosen->getProperty("mac-address-bluetooth0"));

		if (mac != NULL && mac->getLength() == sizeof(address)) {
			memcpy(address, mac->getBytesNoCopy(), sizeof(address));
		}
		chosen->release();
	}
	hk_bt_set_address(address);

	bt = OSTypeAlloc(HKBluetooth);
	if (platform == NULL || bt == NULL || !bt->init() || !bt->attach(platform)) {
		OSSafeReleaseNULL(bt);
		IOLog("HKBluetooth: cannot create the service\n");
		return;
	}
	bt->setName("bluetooth");
	set_data(bt, "name", "bluetooth", sizeof("bluetooth"));
	set_data(bt, "device_type", "bluetooth", sizeof("bluetooth"));
	set_data(bt, "compatible", "bluetooth,qca6390", sizeof("bluetooth,qca6390"));
	set_data(bt, "transport-encoding", &encoding, sizeof(encoding));
	set_data(bt, "transport-speed", &speed, sizeof(speed));
	set_data(bt, "local-mac-address", address, sizeof(address));
	if (!bt->start(platform)) {
		bt->detach(platform);
	} else {
		bt->registerService();
	}
	bt->release();
}
