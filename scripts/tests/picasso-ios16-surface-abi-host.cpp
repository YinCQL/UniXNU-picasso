#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#define DEVELOPMENT 1
#define DEBUG 0
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnUnsupported = 1;
static bool failAllocation, refuseOff;
#if defined(_MSC_VER) && !defined(__clang__)
#define __ATOMIC_RELAXED 0
template<class T, class U> static T __atomic_fetch_add(T *p, U amount, int) {
    T old = *p; *p += static_cast<T>(amount); return old;
}
#endif
static unsigned snapshots, calls, locks;
static std::unordered_map<void *, size_t> allocations;
static int diagnostic(const char *, ...) { return 0; }
#define printf diagnostic
#define bzero(p, n) memset(p, 0, n)
static void *IOMallocZeroData(size_t n) {
    if (failAllocation) return nullptr;
    void *p = calloc(1, n); assert(p); allocations[p] = n; return p;
}
static void IOFreeData(void *p, size_t n) {
    assert(allocations.at(p) == n); allocations.erase(p); free(p);
}
static int proc_selfpid() { return 42; }
static void proc_name(int, char *p, size_t n) { snprintf(p, n, "backboardd"); }
static bool PE_parse_boot_argn(const char *name, void *p, size_t n) {
    assert(!strcmp(name, "picasso-fb-reject-off") && n == sizeof(unsigned));
    *static_cast<unsigned *>(p) = refuseOff; return true;
}
extern "C" void picasso_notifyd_snapshot_later() { ++snapshots; }
struct IOExternalMethodArguments {
    uint32_t scalarInputCount = 0, scalarOutputCount = 0;
    const uint64_t *scalarInput = nullptr;
    const void *structureInput = nullptr;
    void *structureOutput = nullptr;
    uint32_t structureInputSize = 0, structureOutputSize = 0;
    void *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
};
using IOExternalMethodArgumentsOpaque = IOExternalMethodArguments;
struct MetaClass { const char *name; const char *getClassName() { return name; } };
constexpr int kIPCLockWrite = 1, kIPCLockRead = 2, kIPCLockNone = 0;
static std::function<IOReturn(uint32_t, IOExternalMethodArguments *)> handler;
class IOUserClient {
public:
    MetaClass meta{"OtherClient"};
    bool defaultLocking = true, defaultLockingSingleThreadExternalMethod = false, uc2022 = false;
    MetaClass *getMetaClass() { return &meta; }
    void ipcEnter(int) { assert(locks++ == 0); }
    void ipcExit(int) { assert(--locks == 0); }
    virtual IOReturn externalMethod(uint32_t s, IOExternalMethodArguments *a) {
        assert(locks == 1); ++calls; return handler(s, a);
    }
    IOReturn callExternalMethod(uint32_t, IOExternalMethodArguments *);
};
class IOUserClient2022 : public IOUserClient {};
#include "ios16-surface-abi.inc"
#undef printf

static uint32_t word(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
static void wide(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }
int main() {
    unsigned scenarios = 0;
    std::vector<uint8_t> oldSwap(0x46c), expected(0x508);
    for (unsigned i = 0; i < oldSwap.size(); ++i) oldSwap[i] = static_cast<uint8_t>(i % 251 + 1);
    const unsigned runs[][3] = {{0,0x50,0},{0x50,0x260,0x48},{0x260,0x2e6,0x6c},{0x2e6,0x2e8,0x6d},
        {0x2e8,0x30c,0x70},{0x30c,0x37b,0x78},{0x37b,0x467,0x7c},{0x467,0x46c,0x9c}};
    for (auto &r : runs) memcpy(expected.data()+r[0]+r[2], oldSwap.data()+r[0], r[1]-r[0]);
    void *swap = picasso_ios16_swap(oldSwap.data());
    assert(!memcmp(swap, expected.data(), expected.size())); IOFreeData(swap, expected.size()); ++scenarios;
    std::vector<uint8_t> limits(0x28), lock(0xc68), converted(0xa68);
    for (unsigned i = 0; i < limits.size(); ++i) limits[i] = static_cast<uint8_t>(i+1);
    uint8_t small[0x18]; picasso_ios16_limits(small, limits.data());
    assert(!memcmp(small, limits.data(), 0x14) && small[0x14] == limits[0x20] && small[0x15] == 0);
    assert(!memcmp(small+0x16, limits.data()+0x22, 2)); ++scenarios;
    for (unsigned i = 0; i < lock.size(); ++i) lock[i] = static_cast<uint8_t>(i % 251);
    const unsigned header[][2] = {{0x1c,0x20},{0x20,0x28},{0x24,0x30},{0x28,0x38},{0x2c,0x40}};
    for (unsigned i = 0; i < 5; ++i) wide(lock.data()+header[i][1], 100+i);
    for (unsigned p = 0; p < 20; ++p)
        for (unsigned i = 0; i < 6; ++i) wide(lock.data()+0x88+p*0x98+i*8, 1000+p*10+i);
    wide(lock.data()+0x88+19*0x98+5*8, UINT64_MAX);
    picasso_ios16_lock(converted.data(), lock.data());
    assert(!memcmp(converted.data(), lock.data(), 0x1c));
    for (unsigned i = 0; i < 5; ++i) assert(word(converted.data()+header[i][0]) == 100+i);
    assert(!memcmp(converted.data()+0x30, lock.data()+0x48, 8));
    assert(!memcmp(converted.data()+0x38, lock.data()+0x58, 0x30));
    for (unsigned p = 0; p < 20; ++p) {
        for (unsigned i = 0; i < 6; ++i)
            assert(word(converted.data()+0x68+p*0x80+i*4) == (p == 19 && i == 5 ? UINT32_MAX : 1000+p*10+i));
        assert(!memcmp(converted.data()+0x68+p*0x80+0x18, lock.data()+0x88+p*0x98+0x30, 0x68));
    }
    ++scenarios;
    IOUserClient client; client.meta.name = "IOMobileFramebufferUserClient";
    IOExternalMethodArguments args; args.structureInput = oldSwap.data(); args.structureInputSize = 0x46c;
    handler = [&](uint32_t s, auto *a) { assert(s == 5 && a->structureInputSize == 0x508);
        assert(!memcmp(a->structureInput, expected.data(), expected.size())); return 7; };
    assert(client.callExternalMethod(5, &args) == 7 && args.structureInput == oldSwap.data() && args.structureInputSize == 0x46c);
    assert(allocations.empty()); ++scenarios;
    handler = [&](uint32_t, auto *a) { assert(a->structureInput == oldSwap.data() && a->structureInputSize == 0x46c); return 9; };
    for (unsigned mode = 0; mode < 4; ++mode) {
        failAllocation = mode == 0;
        args.structureInputDescriptor = mode == 1 ? &client : nullptr;
        client.meta.name = mode == 2 ? "OtherClient" : "IOMobileFramebufferUserClient";
        assert(client.callExternalMethod(mode == 3 ? 4 : 5, &args) == 9); ++scenarios;
    }
    failAllocation = false; args.structureInputDescriptor = nullptr; client.meta.name = "IOSurfaceRootUserClient";
    for (uint32_t s : {0U,2U,4U,6U,7U,34U,54U,56U,58U,60U,61U,88U,13U,67U}) {
        bool isLimits = s % 54 == 13;
        std::vector<uint8_t> out(isLimits ? 0x18 : 0xa68, 0xcd);
        args.structureOutput = out.data(); args.structureOutputSize = static_cast<uint32_t>(out.size());
        handler = [&](uint32_t selected, auto *a) { assert(selected == s);
            assert(a->structureOutput != out.data() && a->structureOutputSize == (isLimits ? 0x28U : 0xc68U));
            memcpy(a->structureOutput, isLimits ? limits.data() : lock.data(), a->structureOutputSize); return 0; };
        assert(client.callExternalMethod(s, &args) == 0 && args.structureOutput == out.data() && args.structureOutputSize == out.size());
        assert(!memcmp(out.data(), isLimits ? small : converted.data(), out.size()));
        assert(allocations.empty()); ++scenarios;
    }
    std::vector<uint8_t> untouched(0xa68, 0xcd); args.structureOutput = untouched.data(); args.structureOutputSize = 0xa68;
    handler = [](uint32_t, auto *a) { memset(a->structureOutput, 1, a->structureOutputSize); return 8; };
    assert(client.callExternalMethod(0, &args) == 8 && args.structureOutput == untouched.data() && args.structureOutputSize == 0xa68);
    for (auto byte : untouched) assert(byte == 0xcd); assert(allocations.empty()); ++scenarios;
    args.structureOutputDescriptor = &client;
    handler = [&](uint32_t, auto *a) { assert(a->structureOutput == untouched.data()); return 0; };
    assert(client.callExternalMethod(0, &args) == 0); ++scenarios;
    args = {}; client.meta.name = "IOMobileFramebufferUserClient"; uint64_t power = 0;
    args.scalarInput = &power; args.scalarInputCount = 1;
    handler = [](uint32_t, auto *) { return kIOReturnUnsupported; };
    refuseOff = true; assert(client.callExternalMethod(12, &args) == kIOReturnUnsupported); ++scenarios;
    power = 1; assert(client.callExternalMethod(12, &args) == kIOReturnSuccess); ++scenarios;
    power = 0; refuseOff = false; assert(client.callExternalMethod(12, &args) == kIOReturnSuccess); ++scenarios;
    IOUserClient2022 modern; modern.uc2022 = true;
    handler = [](uint32_t s, auto *) { assert(s == 99); return 6; };
    assert(modern.callExternalMethod(99, &args) == 6); ++scenarios;
    assert(allocations.empty() && !locks && calls > 0 && snapshots == 1);
    std::printf("PASS iOS16 ABI: %u scenarios\n", scenarios);
}
