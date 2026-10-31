// Compile the actual display policy functions with modeled IOKit objects.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct OSObject {
    unsigned refs = 1;
    virtual ~OSObject() = default;
    void retain() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
struct OSData : OSObject {
    std::vector<unsigned char> bytes;
    static OSData *withBytes(const void *value, unsigned length) {
        auto *data = new OSData;
        auto *begin = static_cast<const unsigned char *>(value);
        data->bytes.assign(begin, begin + length);
        return data;
    }
    unsigned getLength() const { return static_cast<unsigned>(bytes.size()); }
    const void *getBytesNoCopy() const { return bytes.data(); }
};
struct IORegistryEntry : OSObject {
    std::map<std::string, OSObject *> props;
    bool failSet = false;
    ~IORegistryEntry() override { for (auto &p : props) p.second->release(); }
    OSObject *getProperty(const char *key) {
        auto it = props.find(key); return it == props.end() ? nullptr : it->second;
    }
    bool setProperty(const char *key, OSObject *value) {
        if (failSet) return false;
        removeProperty(key); value->retain(); props[key] = value; return true;
    }
    void removeProperty(const char *key) {
        auto it = props.find(key);
        if (it != props.end()) { it->second->release(); props.erase(it); }
    }
    static IORegistryEntry *fromPath(const char *, void *);
};
static IORegistryEntry *root = nullptr;
IORegistryEntry *IORegistryEntry::fromPath(const char *, void *) {
    if (root) { root->retain(); }
    return root;
}
struct IOService {};
struct IOMemoryMap {
    uint64_t address = 0xa0000000, length = 4320 * 2400;
    uint64_t getPhysicalAddress() const { return address; }
    uint64_t getLength() const { return length; }
};
struct PE_Video {
    uint64_t v_baseAddr = 0xa0000000;
    unsigned long v_width = 1080, v_height = 2400, v_rowBytes = 4320, v_depth = 32, v_display = 0;
};
static PE_Video fixtureVideo;
static int consoleError = 0;
static int PE_current_console(PE_Video *out) { *out = fixtureVideo; return consoleError; }
static void IOLog(const char *, ...) {}
static void *gIODTPlane = nullptr;
static IORegistryEntry *displayChosen = nullptr;
static bool scaleAdded = false, rotationAdded = false;
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define OSSafeReleaseNULL(value) do { if (value) { (value)->release(); (value) = nullptr; } } while (0)

#include "display-policy.inc"

static void put(IORegistryEntry *entry, const char *key, uint32_t value) {
    OSData *data = OSData::withBytes(&value, sizeof(value));
    assert(entry->setProperty(key, data)); data->release();
}
static uint32_t word(IORegistryEntry *entry, const char *key) {
    auto *data = OSDynamicCast(OSData, entry->getProperty(key));
    assert(data && data->getLength() == sizeof(uint32_t));
    uint32_t value; std::memcpy(&value, data->getBytesNoCopy(), sizeof(value)); return value;
}
static void closeRoot() { assert(!displayChosen); root->release(); root = nullptr; }
static bool start(IOMemoryMap &map) { return native_legacy_gop_mapping_observed(nullptr, &map, 1080, 2400); }

int main(int argc, char **argv) {
    assert(argc == 2);
    uint32_t adtScale = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
    assert(adtScale == 2 || adtScale == 3);
    IOMemoryMap map;
    root = new IORegistryEntry;
    assert(start(map)); assert(word(root, "display-scale") == 2); assert(word(root, "display-rotation") == 0);
    finishDisplayPolicy(true); closeRoot();
    for (uint32_t scale : {2u, 3u, adtScale}) {
        root = new IORegistryEntry; put(root, "display-scale", scale);
        OSObject *original = root->getProperty("display-scale");
        assert(start(map)); assert(word(root, "display-scale") == scale); assert(word(root, "display-rotation") == 0);
        assert(root->getProperty("display-scale") == original);
        finishDisplayPolicy(true); closeRoot();
    }
    root = new IORegistryEntry;
    assert(start(map)); assert(!start(map)); finishDisplayPolicy(false);
    assert(root->props.empty()); closeRoot();
    root = new IORegistryEntry; put(root, "display-scale", adtScale);
    assert(start(map)); finishDisplayPolicy(false);
    assert(word(root, "display-scale") == adtScale && !root->getProperty("display-rotation")); closeRoot();
    root = new IORegistryEntry; put(root, "display-scale", 4);
    assert(!start(map)); assert(word(root, "display-scale") == 4); assert(!root->getProperty("display-rotation")); closeRoot();
    root = new IORegistryEntry; auto *bad = new OSObject;
    assert(root->setProperty("display-scale", bad)); bad->release();
    assert(!start(map)); assert(root->getProperty("display-scale")); closeRoot();
    root = new IORegistryEntry; unsigned char small = 3; auto *shortData = OSData::withBytes(&small, 1);
    assert(root->setProperty("display-scale", shortData)); shortData->release();
    assert(!start(map)); assert(root->getProperty("display-scale")); closeRoot();
    root = new IORegistryEntry; put(root, "display-rotation", 1);
    assert(!start(map)); assert(!root->getProperty("display-scale")); assert(word(root, "display-rotation") == 1); closeRoot();
    root = new IORegistryEntry; put(root, "display-scale", adtScale); put(root, "display-rotation", 1);
    assert(!start(map)); assert(word(root, "display-scale") == adtScale); assert(word(root, "display-rotation") == 1); closeRoot();
    root = new IORegistryEntry; root->failSet = true;
    assert(!start(map)); assert(root->props.empty()); closeRoot();
    root = new IORegistryEntry; map.address++;
    assert(!start(map)); assert(root->props.empty()); map.address--; closeRoot();
    root = new IORegistryEntry; consoleError = 1;
    assert(!start(map)); assert(root->props.empty()); consoleError = 0; closeRoot();
    assert(!start(map));
    std::puts("PASS display policy: scale and rollback scenarios");
}
