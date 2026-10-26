#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
/* Registry state proves registration, not hardware operation. Keep those distinct on the host. */
extern "C" int iphont_registry(char *out, unsigned capacity)
{
    IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (!it) return -ENOMEM;
    unsigned used = 0, count = 0;
    int error = 0;
#define ADD(...) do { int n = snprintf(out + used, capacity - used, __VA_ARGS__); \
    if (n < 0 || (unsigned)n >= capacity - used) { error = ENOSPC; goto done; } used += n; } while (0)
    ADD("{\"entries\":[");
    while (IORegistryEntry *entry = it->getNextObject()) {
        if (count++) ADD(",");
        ADD("{\"id\":%llu,\"name\":\"", entry->getRegistryEntryID());
        const char *parts[2] = {entry->getName(), entry->getMetaClass()->getClassName()};
        for (unsigned i = 0; i < 2; i++) {
            for (const unsigned char *s = (const unsigned char *)parts[i]; s && *s; s++) {
                if (*s < 32 || *s >= 127 || *s == '"' || *s == '\\') ADD("\\u%04x", *s);
                else ADD("%c", *s);
            }
            if (!i) ADD("\",\"class\":\"");
        }
        ADD("\"");
        IOService *svc = OSDynamicCast(IOService, entry);
        if (svc) ADD(",\"inactive\":%s,\"busy\":%u", svc->isInactive() ? "true" : "false", svc->getBusyState());
        IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
        uint64_t providerID = parent ? parent->getRegistryEntryID() : 0;
        if (parent) parent->release();
        if (providerID) ADD(",\"provider_id\":%llu", providerID);
        ADD("}");
    }
    if (!it->isValid()) { error = EAGAIN; goto done; }
    ADD("]}");
done:
    it->release();
    return error ? -error : (int)used;
#undef ADD
}
