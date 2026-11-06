// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso: PM8150 SID0 PON 0x08 (power bit0, volume-down bit1),
// PM8150L SID4 GPIO5 0xc4 (volume-up active low). See saved stock DT and
// vendor lito.dtsi, lito-mtp.dtsi, pm8150.dtsi, spmi-pmic-arb.c.
// Poll real RT_STS through EE0 observer channels at 10 ms. No PMIC register,
// PDC/arbiter interrupt, GPIO mux, PON reset timer or reboot reason is written.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "PicassoButtonsCore.h"
#include "PicassoSpmi.h"

extern "C" bool picasso_buttons_deliver(uint32_t);
extern "C" bool picasso_buttons_available(void);
extern "C" void picasso_hid_physical_button(void);
namespace {
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t base, uint64_t bytes) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, bytes);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    ~Window() { OSSafeReleaseNULL(map); }
};
uint64_t milliseconds() { uint64_t n = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &n); return n / 1000000; }
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "picasso-buttons");
    if (!picasso_spmi_init()) { IOLog("PICASSO_BUTTONS refused reason=spmi-lock\n"); return; }
    Window core, owners, observer;
    if (!core.open(0xc440000, 0x2000) || !owners.open(0xc40a000, 0x1000)) {
        IOLog("PICASSO_BUTTONS refused reason=map\n"); return;
    }
    uint32_t version = core.va[0];
    if (!picasso_buttons::version_supported(version)) {
        IOLog("PICASSO_BUTTONS refused reason=version value=%08x\n", version); return;
    }
    auto read = [&](bool owner, uint32_t offset) { return (owner ? owners.va : core.va)[offset / 4]; };
    int pon = picasso_buttons::find_apid(read, picasso_buttons::pon_ppid);
    int up = picasso_buttons::find_apid(read, picasso_buttons::up_ppid);
    if (pon < 0 || up < 0 || !observer.open(0xe600000, 0x10000)) {
        IOLog("PICASSO_BUTTONS refused reason=owner-or-map pon=%d up=%d\n", pon, up); return;
    }
    auto *p=observer.va + picasso_buttons::observer_offset(unsigned(pon)) / 4;
    auto *g=observer.va + picasso_buttons::observer_offset(unsigned(up)) / 4;
    uint8_t type = 0, mode = 0, enable = 0, subtype = 0;
    int error = picasso_spmi_read8(g, 4, type);
    if (!error) error = picasso_spmi_read8(g, 5, subtype);
    if (!error) error = picasso_spmi_read8(g, 0x40, mode);
    if (!error) error = picasso_spmi_read8(g, 0x46, enable);
    // Adopt only an enabled GPIO input; expose incompatible firmware instead
    // of driving a button pin or reporting all keys permanently released.
    if (error || !picasso_buttons::gpio_input(type, subtype, mode, enable)) {
        IOLog("PICASSO_BUTTONS refused reason=gpio-input error=%d type=%x subtype=%x mode=%x enable=%x\n", error, type, subtype, mode, enable); return;
    }
    IOLog("PICASSO_BUTTONS ready version=%08x pon_apid=%d up_apid=%d poll_ms=10 debounce_ms=20\n", version, pon, up);
    picasso_buttons::Reports state;
    unsigned reports = 0;
    for (;;) {
        if (!picasso_buttons_available()) break;
        uint8_t pv = 0, gv = 0;
        int failedApid=pon;
        error = picasso_spmi_read8(p, 0x10, pv);
        if (!error) { failedApid=up; error = picasso_spmi_read8(g, 0x10, gv); }
        if (error) {
            // Stop after a failed/unfinished command. Do not reuse a timed-out
            // channel or guess its state. Cancel our logical HID hold explicitly:
            // this is transport teardown, not an observed physical release.
            bool cancelled = picasso_buttons_deliver(0x80000000u);
            IOLog("PICASSO_BUTTONS stopped error=%d apid=%d last_mask=%x cancel_ok=%u\n", error, failedApid, state.delivered, cancelled); break;
        }
        bool changed = state.sample(picasso_buttons::mask(pv, gv), milliseconds(), [](uint32_t mask) {
            if (mask) picasso_hid_physical_button();
            return picasso_buttons_deliver(mask);
        });
        if (changed && ++reports <= 64)
            IOLog("PICASSO_BUTTONS report=%u mask=%x pon=%02x gpio=%02x\n", reports, state.delivered, pv, gv);
        IOSleep(10);
    }
}
}
extern "C" void picasso_buttons_start(void) {
    static bool started;
    unsigned enabled = 0;
    PE_parse_boot_argn("picasso-buttons", &enabled, sizeof(enabled));
    if (enabled != 1 || started || !picasso_buttons_available()) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) { started = true; thread_deallocate(thread); }
    else IOLog("PICASSO_BUTTONS refused reason=thread\n");
}
