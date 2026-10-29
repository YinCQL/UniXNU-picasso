// Host stand-ins; the actual diagnostic helper is included without edits.
#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
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
    *expected = actual;
    return false;
}
#define __atomic_compare_exchange_n(p,e,d,w,s,f) host_compare(p,e,d)
#endif
#define DEVELOPMENT 1
#define DEBUG 0
using IOReturn = unsigned;
struct OSObject { virtual ~OSObject() = default; };
struct OSString : OSObject {
    const char *value;
    explicit OSString(const char *p) : value(p) {}
    const char *getCStringNoCopy() const { return value; }
};
struct OSDictionary {
    OSObject *provider, *match, *resource;
    unsigned reads = 0;
    OSObject *getObject(const char *key) {
        ++reads;
        if (!strcmp(key, "IOProviderClass")) return provider;
        if (!strcmp(key, "IONameMatch")) return match;
        assert(!strcmp(key, "IOResourceMatch")); return resource;
    }
};
#define OSDynamicCast(T,v) dynamic_cast<T *>(v)
static thread_local const char *process_name = "backboardd";
static thread_local uint64_t thread_id = 42;
static int proc_selfpid() { return 115; }
static void proc_name(int pid, char *name, int size) { assert(pid == 115); snprintf(name, size, "%s", process_name); }
static void *current_thread() { return &thread_id; }
static uint64_t thread_tid(void *t) { return *static_cast<uint64_t *>(t); }
static std::mutex output_lock;
static std::string output;
static unsigned lines;
static int host_log(const char *format, ...) {
    char text[1024]; va_list args; va_start(args, format);
    int count = vsnprintf(text, sizeof(text), format, args); va_end(args);
    assert(count > 0 && size_t(count) < sizeof(text));
    std::lock_guard<std::mutex> lock(output_lock); output += text; ++lines;
    return count;
}
#define printf host_log
#include "service-trace.inc"
#undef printf
static void contains(const char *s) { assert(output.find(s) != std::string::npos); }
int main(int argc, char **argv) {
    assert(argc == 2); int scenario = atoi(argv[1]);
    OSString provider("IOSurfaceRoot"), match("display"), resource("IOBSD");
    OSDictionary dict{&provider, &match, &resource};
    if (scenario == 0) {
        auto trace = picasso_service_begin(0, &dict, nullptr, 0);
        picasso_service_end(trace, 0, 0, true);
        assert(lines == 2 && dict.reads == 3);
        contains("provider=IOSurfaceRoot match=display resource=IOBSD"); contains("rpc=0 result=0 object=1");
    } else if (scenario == 1) {
        for (const char *name : {"watchdogd", "backboardd-helper", "SpringBoardX", "springboard", ""}) {
            process_name = name; auto trace = picasso_service_begin(0, &dict, nullptr, 0);
            picasso_service_end(trace, 0, 0, true); assert(!trace.ticket);
        }
        assert(!lines && !dict.reads);
    } else if (scenario == 2) {
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 32; ++t) workers.emplace_back([t]() {
            process_name = (t & 1) ? "backboardd" : "SpringBoard"; thread_id = t + 100;
            for (unsigned i = 0; i < 100; ++i) for (unsigned op = 0; op < 4; ++op) {
                auto trace = picasso_service_begin(op, nullptr, nullptr, 0);
                picasso_service_end(trace, 0, 0, true);
            }
        });
        for (auto &t : workers) t.join();
        assert(lines == 192); contains("sample=12"); assert(output.find("sample=13") == std::string::npos);
        for (const char *name : {"backboardd", "SpringBoard"}) for (unsigned op = 0; op < 4; ++op) {
            for (unsigned sample = 1; sample <= 12; ++sample) {
                std::string key = "op=" + std::to_string(op) + " sample=" + std::to_string(sample) + " pid=115 name=" + name + " ";
                auto a = output.find(key); assert(a != std::string::npos);
                auto b = output.find(key, a + 1); assert(b != std::string::npos);
                assert(output.find(key, b + 1) == std::string::npos);
            }
        }
    } else if (scenario == 3) {
        std::string long_text(150, 'A'); long_text[2] = '\n'; long_text[3] = '\x80';
        OSString long_string(long_text.c_str()); OSObject non_string;
        dict = {&long_string, &non_string, nullptr};
        auto trace = picasso_service_begin(2, &dict, "notify\nvalue", 0);
        picasso_service_end(trace, 0, 0, true);
        contains("provider=AA??");contains("match=<non-string> resource=<absent> detail=notify?value");
        auto at = output.find("provider=") + 9; assert(output.find(' ', at) - at == 63);
        assert(lines == 2);
    } else if (scenario == 4) {
        auto trace = picasso_service_begin(3, nullptr, "IOSurfaceRoot", 7);
        picasso_service_end(trace, 0, 0xe00002e2U, false);
        contains("detail=IOSurfaceRoot type=7");contains("rpc=0 result=e00002e2 object=0");
        auto denied = picasso_service_begin(3, nullptr, "IOSurfaceRoot", 7);
        picasso_service_end(denied, 0xe00002e2U, 0xe00002e2U, false);
        contains("rpc=e00002e2 result=e00002e2 object=0");assert(lines == 4);
    } else if (scenario == 5) {
        for (unsigned op : {4U, 99U, UINT32_MAX}) {
            auto trace = picasso_service_begin(op, &dict, nullptr, 0);
            picasso_service_end(trace, 0, 0, true); assert(!trace.ticket);
        }
        assert(!lines && !dict.reads);
    } else if (scenario == 6) {
        // The diagnostic must bound source reads as well as output length.
        char bounded_source[63]; memset(bounded_source, 'A', sizeof(bounded_source));
        char text[64]; picasso_trace_text(text, sizeof(text), bounded_source);
        assert(strlen(text) == 63 && !memcmp(text, bounded_source, 63));
        picasso_trace_text(nullptr, 0, bounded_source);
        text[0] = 'X'; picasso_trace_text(text, 1, bounded_source); assert(!text[0]);
        assert(!lines && !dict.reads);
    } else assert(false);
    unsigned newlines = 0; for (char c : output) if (c == '\n') ++newlines;
    assert(newlines == lines);
    fprintf(stdout, "PASS scenario=%d lines=%u\n", scenario, lines);
}
