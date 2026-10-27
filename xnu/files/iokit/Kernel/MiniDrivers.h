/*
 * Stand-ins for Apple's closed kernel extensions: just enough of each for the stock iOS
 * user space to work on a machine without Apple hardware. The platform expert
 * (GenericARMPE) starts them once the registry is up.
 *
 *   MiniIOSurface.cpp              IOSurface: pixel buffers shared between processes
 *   MiniIOMobileFramebuffer.cpp    IOMobileFramebuffer: the display (the boot framebuffer)
  *   MiniAppleKeyStore.cpp          AppleKeyStore: the keybag of a device without a passcode
 *   MiniAppleARMBacklight.cpp      AppleARMBacklight: the service brightness requests go to
 *   MiniIONetworkStack.cpp         IONetworkStack: what configd's interface namer waits for
 *   HKBluetooth.cpp                the "bluetooth" service bluetoothd and BlueTool look up (bt=1)
 */
#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H

#include <IOKit/IOService.h>

void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniAppleARMBacklightStart(void);
void MiniIONetworkStackStart(void);
void HKBluetoothStart(void);

#endif /* _IOKIT_MINIDRIVERS_H */
