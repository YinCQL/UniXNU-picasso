// Host stand-ins for the extracted, display-only diagnostic.
#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#define __ATOMIC_RELAXED 0
#define __atomic_load_n(p,o) ((unsigned)_InterlockedCompareExchange((volatile long *)(p),0,0))
static bool host_compare(unsigned *p, unsigned *expected, unsigned desired) {
    unsigned actual = (unsigned)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)*expected);
    if (actual == *expected) return true;
    *expected = actual; return false;
}
#define __atomic_compare_exchange_n(p,e,d,w,s,f) host_compare(p,e,d)
#endif
#define DEVELOPMENT 1
#define DEBUG 0
using IOReturn = unsigned;
struct IOUserClient {
    bool display;
    void *metaCast(const char *name) {
        assert(!strcmp(name, "IOMobileFramebufferUserClient")); return display ? this : nullptr;
    }
};
struct Descriptor { uint64_t bytes; uint64_t getLength() const { return bytes; } };
struct IOExternalMethodArguments {
    Descriptor *structureInputDescriptor;
    uint32_t structureInputSize, scalarInputCount, structureOutputSize, scalarOutputCount;
};
static thread_local const char *processName = "backboardd";
static thread_local uint64_t threadId = 42;
static int proc_selfpid() { return 57; }
static void proc_name(int pid, char *name, int size) { assert(pid == 57); snprintf(name, size, "%s", processName); }
static void *current_thread() { return &threadId; }
static uint64_t thread_tid(void *p) { return *static_cast<uint64_t *>(p); }
static std::mutex outputLock;
static std::string output;
static unsigned lines;
static int host_log(const char *format, ...) {
    char text[256]; va_list args; va_start(args, format);
    int size = vsnprintf(text, sizeof(text), format, args); va_end(args);
    assert(size > 0 && size < static_cast<int>(sizeof(text)));
    std::lock_guard<std::mutex> lock(outputLock); output += text; ++lines; return size;
}
#define printf host_log
#include "fb-method-trace.inc"
#undef printf

int main(int argc, char **argv) {
    assert(argc == 2); int scenario = atoi(argv[1]);
    IOUserClient display{true}, other{false};
    IOExternalMethodArguments args{nullptr, 1132, 0, 0, 0};
    if (scenario == 0) {
        auto t = picasso_fb_method_begin(&display, 5, &args);
        assert(t.ticket == 1); picasso_fb_method_end(t, &args, 0xe00002c2U);
        assert(lines == 2 && args.structureInputSize == 1132);
        assert(output.find("sel=5 sample=1") != std::string::npos);
        assert(output.find("in_bytes=1132") != std::string::npos);
        assert(output.find("result=e00002c2") != std::string::npos);
    } else if (scenario == 1) {
        for (uint32_t selector : {0u, 6u, 71u, 73u, UINT32_MAX}) {
            assert(!picasso_fb_method_begin(&display, selector, &args).ticket);
        }
        assert(!picasso_fb_method_begin(&other, 5, &args).ticket);
        assert(!picasso_fb_method_begin(nullptr, 5, &args).ticket);
        assert(!picasso_fb_method_begin(&display, 5, nullptr).ticket);
        processName = "identityservices";
        assert(!picasso_fb_method_begin(&display, 5, &args).ticket);
        assert(lines == 0);
    } else if (scenario == 2) {
        Descriptor descriptor{0x123456789ULL}; args.structureInputDescriptor = &descriptor;
        args.scalarInputCount = 4; args.structureOutputSize = 9; args.scalarOutputCount = 2;
        auto t = picasso_fb_method_begin(&display, 72, &args);
        assert(t.ticket == 1); picasso_fb_method_end(t, &args, 0);
        assert(lines == 2 && output.find("in_bytes=4886718345") != std::string::npos);
        assert(output.find("in_scalar=4 out_bytes=9 out_scalar=2") != std::string::npos);
        assert(args.structureInputDescriptor == &descriptor && descriptor.bytes == 0x123456789ULL);
    } else if (scenario == 3) {
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 40; ++i) workers.emplace_back([i, &display, &args]() {
            processName = i & 1 ? "backboardd" : "SpringBoard"; threadId = i + 100;
            for (unsigned n = 0; n < 100; ++n) for (uint32_t selector : {4u, 5u, 72u}) {
                auto t = picasso_fb_method_begin(&display, selector, &args);
                picasso_fb_method_end(t, &args, 0);
            }
        });
        for (auto &worker : workers) worker.join();
        assert(lines == 36);
        for (uint32_t selector : {4u, 5u, 72u}) for (unsigned sample : {1u, 2u, 3u}) {
            std::string begin = "begin sel=" + std::to_string(selector) + " sample=" + std::to_string(sample);
            std::string end = "end sel=" + std::to_string(selector) + " sample=" + std::to_string(sample);
            size_t a = output.find(begin), b = output.find(end);
            assert(a != std::string::npos && b != std::string::npos);
            assert(output.find(begin, a + 1) != std::string::npos);
            assert(output.find(end, b + 1) != std::string::npos);
        }
    } else { assert(false); }
    std::printf("PASS scenario=%d\n", scenario);
}
