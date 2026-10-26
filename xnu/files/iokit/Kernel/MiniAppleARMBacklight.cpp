/*
 * AppleARMBacklight: the service iOS's brightness code (CoreBrightness in backboardd, UserEventAgent) looks for. It is
 * found by class (IOServiceMatching) or by the name "backlight". What it asks of it is written down (every property set,
 * every user client opened, every call) so that the way from the brightness setting to the panel can be built on what is
 * really asked: bsd/dev -> MiniIOMobileFramebuffer.cpp (hk_display_set_backlight).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

extern "C" void hk_display_set_backlight(uint32_t level);

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

bool
AppleARMBacklight::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setName("backlight");
	/* what the power logger (UserEventAgent's PerfPowerServices plugin) matches on, with IOPropertyMatch, and reads back */
	setProperty("backlight-control", kOSBooleanTrue);
	{
		OSDictionary * brightness = OSDictionary::withCapacity(3);
		OSDictionary * parameters = OSDictionary::withCapacity(1);

		if (brightness != NULL && parameters != NULL) {
			brightness->setObject("min", OSNumber::withNumber((unsigned long long) 0, 32));
			brightness->setObject("max", OSNumber::withNumber((unsigned long long) 65536, 32));
			brightness->setObject("value", OSNumber::withNumber((unsigned long long) 32768, 32));
			parameters->setObject("brightness", brightness);
			setProperty("IODisplayParameters", parameters);
		}
		OSSafeReleaseNULL(brightness);
		OSSafeReleaseNULL(parameters);
	}
	registerService();
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	char procname[32] = "?";
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);

	proc_selfname(procname, sizeof(procname));
	if (dict == NULL) {
		IOLog("MiniAppleARMBacklight: %s[%d] set properties of a kind that is not a dictionary\n", procname, proc_selfpid());
		return kIOReturnBadArgument;
	}
	OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
	const OSSymbol * key;

	while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL) {
		OSObject * value = dict->getObject(key);
		OSNumber * number = OSDynamicCast(OSNumber, value);

		OSSerialize * text = OSSerialize::withCapacity(256);

		if (text != NULL && value != NULL && value->serialize(text)) {
			IOLog("MiniAppleARMBacklight: %s[%d] sets %s = %.300s\n", procname, proc_selfpid(), key->getCStringNoCopy(),
			    text->text());
		} else {
			IOLog("MiniAppleARMBacklight: %s[%d] sets %s = %s %llu\n", procname, proc_selfpid(), key->getCStringNoCopy(),
			    value ? value->getMetaClass()->getClassName() : "?", number ? number->unsigned64BitValue() : 0);
		}
		OSSafeReleaseNULL(text);
		if (value != NULL) {
			setProperty(key, value);
			/* "backlight-level" and "brightness" arrive together: 0..65536 (16.16: 0x8000 is half); one is enough */
			if (number != NULL && key->isEqualTo("backlight-level")) {
				hk_display_set_backlight((uint32_t) number->unsigned64BitValue());
			}
		}
	}
	OSSafeReleaseNULL(keys);
	return kIOReturnSuccess;
}

IOReturn
AppleARMBacklight::newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient ** handler)
{
	char procname[32] = "?";

	proc_selfname(procname, sizeof(procname));
	IOLog("MiniAppleARMBacklight: %s[%d] opens a user client of type %u\n", procname, proc_selfpid(), (unsigned) type);
	*handler = NULL;
	return kIOReturnUnsupported;
}

void
MiniAppleARMBacklightStart(void)
{
	IOService * platform = (IOService *) IOService::getPlatform();
	AppleARMBacklight * light = OSTypeAlloc(AppleARMBacklight);

	if (platform == NULL || light == NULL || !light->init() || !light->attach(platform)) {
		OSSafeReleaseNULL(light);
		IOLog("MiniAppleARMBacklight: cannot create the service\n");
		return;
	}
	if (!light->start(platform)) {
		light->detach(platform);
	}
	light->release();
}
