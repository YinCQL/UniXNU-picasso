// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off CPU frequency control for the SM7250 (lito) cpufreq-hw domains. `sheng-cpufreq=<n>`:
//   1  read-only: report each domain's enable bit, current/requested level and its highest LUT entry
//   2  additionally request that highest non-turbo level in domain 0 (the boot CPU's cluster)
//   3  the same in every domain once its LUT reads valid
//   4  mode 3, and a governor for the domains (below)
// Optional `sheng-cpufreq-cap=<MHz>`: modes 3/4 stay at or below it.
//
// Ported from UniXNU-sheng's Linux-independent CpuFreqProbe (BSD-2-Clause-Patent; see NOTICE in this directory). The
// register layout is NOT the SM8550 EPSS one: picasso's DT node is compatible "qcom,cpufreq-hw" (not "-epss"), and the
// downstream qcom-cpufreq-hw driver selects cpufreq_qcom_std_offsets for it: reg_enable 0x0, reg_freq_lut 0x110 with
// 32-byte rows, reg_volt_lut 0x114, reg_perf_state 0x920, reg_cycle_cntr 0x9c0. Mainline qcom-cpufreq-hw adds
// reg_dcvs_ctrl 0xbc (bit 0 = per-core perf-state votes) and reg_current_vote 0x704 (10-bit XO units) for the same
// qcom_soc_data; this probe reads the latter only as a diagnostic. The three windows, their CPU counts and each CPU's
// domain come from picasso.dtb: cpufreq_hw reg 0x18323000/0x18325800/0x18327800; cpus 0-5 -> domain 0 (6 cores),
// cpu 6 -> domain 1, cpu 7 -> domain 2; #freq-domain-cells = 2.
// LUT row: bits 31:30 source (0 = GPLL0/2, else XO), 18:16 core count (1 = turbo), 7:0 L value. Source 0 is the same
// fixed clock the downstream driver computes as clk_get_rate("alternate") / 2; lito's "alternate" is GCC GPLL0
// (clock id 0, not rpmhcc), which XBL programs to 600 MHz on this SoC, so 300 MHz.
// REG_ENABLE bit 0 is not a reliable gate on this board: both big-core domains read 0 while their cores run and their
// LUTs are complete, so the probe maps the windows once, judges each by its LUT (domainUsable) and sheng_cpufreq_poll(),
// called every boot-progress round, votes each usable domain the first time it sees it and refreshes the report row.
//
// Mode 4's governor runs every 16 ms: a CPU's busy share is 1 - its idle time over the window (sheng_cpu_idle_mach, a
// kernel helper over recount -- see patches/picasso-cpu-idle-xnu.patch), a domain's the share of its busiest CPU. The
// domain gets the lowest LUT level at or above 1.25 x busy x its current frequency (schedutil's margin), the highest
// allowed level once a CPU is 90 % busy (a hispeed jump); it goes up at once and down only after 4 windows in a row
// that asked for less. Levels stay between `sheng-cpufreq-min=<MHz>` (default 0) and mode 3's highest non-turbo level
// (or the cap). XNU's CPU numbers follow the DT cpu order.
// Touch boost: when the HID report count moves, the P domains (1, 2) hold their highest allowed level until
// `sheng-cpufreq-boost-ms=<ms>` (default 1000, 0 = off) after the last report.
// Frame floor: GPU submits (NativeAdreno's submit count) in 2 of the last 4 windows keep every domain at or above
// `sheng-cpufreq-frame-pct=<n>` (default 60) % of its highest level for `sheng-cpufreq-frame-ms=<ms>` (default 300).
// Per domain over the last 10 s the governor stores: level changes, mean MHz, mean busy % of the busiest CPU.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>

extern "C" {
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
// Mode 4, per domain over the last 10 s: level changes, mean MHz, mean busy % of the busiest CPU
volatile uint32_t native_cpufreq_gov[9];
// Mode 4: touch boosts started since boot
volatile uint32_t native_cpufreq_boosts;
// Mode 4: frame floors started, 16 ms windows spent in one
volatile uint32_t native_cpufreq_frame[2];
// Last 10 s, per CPU: busy share in per mille, or UINT32_MAX when the CPU has no processor (offline)
volatile uint32_t native_cpufreq_usage[8];
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
// Live touch-pipeline counters: the panel probe owns the frame count and the physical-contact state,
// the HID driver owns the reports and the edge counters, and the boot stubs define the rest weakly.
extern volatile uint32_t native_novatek_touch_frames, native_touch_hid_reports;
extern volatile uint32_t native_touch_count, native_touch_physical_active;
extern volatile uint32_t native_touch_edge_stats[4];
uint64_t sheng_cpu_idle_mach(int cpu);
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x18323000, 0x18325800, 0x18327800};
constexpr unsigned kDomainCpus[3] = {6, 1, 1};       // CPUs that can appear in each domain (DT cpu nodes)
constexpr unsigned kDomainMaxCores[3] = {6, 2, 2};   // qcom,freq-domain's max_cores cell (DT)
constexpr uint64_t kWindowSize = 0x1000;
constexpr uint32_t kRegEnable = 0x0, kRegDcvsCtrl = 0xbc, kRegFreqLut = 0x110, kRegPerfState = 0x920;
constexpr uint32_t kRegCurrentVote = 0x704, kRegCycleCounter = 0x9c0;
constexpr unsigned kLutEntries = 40, kLutRowSize = 32;
constexpr uint32_t kXoKHz = 19200;        // rpmhcc xo_clk_src (bi_tcxo / 2)
constexpr uint32_t kGpllHalfKHz = 300000; // GCC GPLL0 600 MHz / CLK_HW_DIV 2

struct Window {
    IOMemoryMap *map;
    uintptr_t va;
    int voted;          // level written, -1 none
    uint32_t votedKHz;
    uint32_t preKHz;    // current frequency the first time the domain read usable, before the vote
};
Window gDomain[3];
bool gUsable[3];    // the window's LUT describes this device, whatever REG_ENABLE bit 0 says
unsigned gMode;
uint32_t gCapKHz;
IOLock *gLock;

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t lutKHz(uint32_t row) {return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;}

// The downstream driver treats a row as boost unless its core count equals the domain's max_cores (the qcom,freq-domain
// second cell), except the max-1 rows it reserves for the temperature-driven core-count change; only max_cores rows are
// the normal operating points. If a LUT has no such row, fall back to the vendor's other rule (core count 1 = turbo).
// The table ends at two equal frequencies, whatever their core counts.
int pickLevel(unsigned d, uint32_t *khzOut, unsigned *rowsOut) {
    const uintptr_t va = gDomain[d].va;
    int best = -1; uint32_t bestKHz = 0, prev = 0; unsigned rows = 0;
    for (unsigned fallback = 0; fallback < 2 && best < 0; ++fallback) {
        prev = 0; rows = 0;
        for (unsigned i = 0; i < kLutEntries; ++i) {
            const uint32_t row = rd32(va, kRegFreqLut + i * kLutRowSize), khz = lutKHz(row);
            if (i > 0 && khz == prev) break;
            rows = i + 1;
            prev = khz;
            const unsigned cc = (row >> 16) & 7;
            if (fallback == 0 ? cc != kDomainMaxCores[d] : cc == 1) continue;
            if (gCapKHz && khz > gCapKHz) continue;
            if (best < 0 || khz >= bestKHz) {best = (int)i; bestKHz = khz;}
        }
    }
    if (khzOut) *khzOut = bestKHz;
    if (rowsOut) *rowsOut = rows;
    return best;
}

// REG_ENABLE bit 0 describes the state the boot firmware left the OSM in, and on this board it reads 0 for both
// big-core domains even though their cores run and their LUTs are complete (the vendor driver would skip them only
// when the DT says qcom,skip-enable-check; the vote register is REG_PERF_STATE either way). Judge a domain by its
// LUT instead: a rising, terminated table from a plausible idle frequency to a plausible top frequency.
bool domainUsable(unsigned d) {
    const uintptr_t va = gDomain[d].va;
    uint32_t first = 0, prev = 0, top = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        const uint32_t row = rd32(va, kRegFreqLut + i * kLutRowSize), khz = lutKHz(row);
        if (i > 0 && (khz == prev || khz < prev)) break;
        if (i == 0) first = khz;
        prev = khz;
        top = khz;
    }
    return first >= 100000 && first <= 1000000 && top > 1000000 && top <= 3500000 && top > first;
}

bool mapWindow(Window &w, uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, kWindowSize);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
    w.voted = -1;
    return w.va != 0;
}

// The frequency the hardware is running at, from the perf-state index the engine reports (the register the vendor
// driver writes and reads). 0 when the index is outside the LUT.
uint32_t currentKHz(uintptr_t va) {
    const uint32_t index = rd32(va, kRegPerfState);
    if (index >= kLutEntries) return 0;
    const uint32_t row = rd32(va, kRegFreqLut + index * kLutRowSize);
    return lutKHz(row);
}
uint32_t windowKHz(const Window &w) {return w.va ? currentKHz(w.va) : 0;}

void voteDomain(unsigned d) {
    Window &w = gDomain[d];
    uint32_t khz = 0;
    int level = pickLevel(d, &khz, nullptr);
    if (level < 0) return;
    w.preKHz = currentKHz(w.va);
    wr32(w.va, kRegPerfState, (uint32_t)level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, (uint32_t)level);
    w.voted = level;
    w.votedKHz = khz;
}

void refreshRow() {
    char *out = native_cpufreq_row;
    const size_t cap = sizeof(native_cpufreq_row);
    unsigned n = (unsigned)snprintf(out, cap, "CPUFREQ");
    for (unsigned d = 0; d < 3 && n < cap; ++d) {
        Window &w = gDomain[d];
        if (!w.va) {n += (unsigned)snprintf(out + n, cap - n, " d%u:nomap", d); continue;}
        uint32_t enable = rd32(w.va, kRegEnable) & 1, state = currentKHz(w.va) / 1000;
        if (w.voted >= 0)
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u v%d@%u pre%u", d, enable, state, w.voted, w.votedKHz / 1000,
                                    w.preKHz / 1000);
        else
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u p%u", d, enable, state, rd32(w.va, kRegPerfState));
    }
}

// Mode 4: the governor (see the header).
constexpr int kCpuDomain[] = {0, 0, 0, 0, 0, 0, 1, 2};
constexpr unsigned kCpus = sizeof kCpuDomain / sizeof kCpuDomain[0];
struct Levels {
    unsigned count;
    uint8_t index[kLutEntries];   // LUT rows in ascending frequency, non-turbo, within the floor and the cap
    uint32_t khz[kLutEntries];
};
Levels gLevels[3];
uint32_t gFloorKHz;
unsigned gBoostMs, gFrameMs, gFramePct;

void buildLevels(unsigned d) {
    Levels &l = gLevels[d];
    l.count = 0;
    uint32_t prev = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        const uint32_t row = rd32(gDomain[d].va, kRegFreqLut + i * kLutRowSize), khz = lutKHz(row);
        if (i > 0 && khz == prev) break;
        prev = khz;
        if (((row >> 16) & 7) != kDomainMaxCores[d] || (gCapKHz && khz > gCapKHz)) continue;
        unsigned at = l.count;
        while (at > 0 && l.khz[at - 1] > khz) {l.khz[at] = l.khz[at - 1]; l.index[at] = l.index[at - 1]; --at;}
        l.khz[at] = khz; l.index[at] = (uint8_t)i; ++l.count;
    }
    // Drop the levels under the floor, keeping at least the highest.
    unsigned first = 0;
    while (first + 1 < l.count && l.khz[first] < gFloorKHz) ++first;
    for (unsigned i = first; i < l.count; ++i) {l.khz[i - first] = l.khz[i]; l.index[i - first] = l.index[i];}
    l.count -= first;
}

void setLevel(unsigned d, unsigned at) {
    Window &w = gDomain[d];
    const uint32_t level = gLevels[d].index[at];
    wr32(w.va, kRegPerfState, level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, level);
    w.voted = (int)level;
    w.votedKHz = gLevels[d].khz[at];
}

void governorThread(void *, wait_result_t) {
    uint64_t lastIdle[kCpus] = {}, lastT = mach_absolute_time();
    for (unsigned c = 0; c < kCpus; ++c) lastIdle[c] = sheng_cpu_idle_mach((int)c);
    unsigned cur[3] = {}, down[3] = {}, quiet[3] = {};
    bool ready[3] = {};
    uint64_t boostAbs = 0, boostEnd = 0;
    nanoseconds_to_absolutetime((uint64_t)gBoostMs * 1000000, &boostAbs);
    uint32_t touches = &native_hid_input_events ? native_hid_input_events[6] : 0;
    uint64_t frameAbs = 0, frameEnd = 0;
    nanoseconds_to_absolutetime((uint64_t)gFrameMs * 1000000, &frameAbs);
    uint32_t submits = &native_adreno_method_stats ? native_adreno_method_stats[5][0] : 0, recent = 0;
    uint64_t winNs = 0, khzNs[3] = {}, busyNs[3] = {};
    uint32_t changes[3] = {};
    // 0x9c0 counts the domain's CPU cycles (the counter the vendor exposes to the scheduler); its delta over the window
    // is the only register-independent readout of the clock the OSM really applied.
    uint32_t cycPrev[3] = {};
    uint64_t cycDelta[3] = {};
    for (unsigned d = 0; d < 3; ++d) if (gUsable[d]) cycPrev[d] = rd32(gDomain[d].va, kRegCycleCounter);
    for (;;) {
        IOSleep(16);
        const uint64_t t = mach_absolute_time(), dt = t - lastT;
        lastT = t;
        if (!dt) continue;
        for (unsigned d = 0; d < 3; ++d) {
            if (!gUsable[d]) continue;
            const uint32_t now = rd32(gDomain[d].va, kRegCycleCounter);
            cycDelta[d] += (uint32_t)(now - cycPrev[d]);
            cycPrev[d] = now;
        }
        if (boostAbs && &native_hid_input_events && native_hid_input_events[6] != touches) {
            touches = native_hid_input_events[6];
            if (t >= boostEnd) native_cpufreq_boosts = native_cpufreq_boosts + 1;
            boostEnd = t + boostAbs;
        }
        if (frameAbs && &native_adreno_method_stats) {
            const uint32_t now = native_adreno_method_stats[5][0];
            recent = ((recent << 1) | (now != submits)) & 15;
            submits = now;
            if (__builtin_popcount(recent) >= 2) {
                if (t >= frameEnd) native_cpufreq_frame[0] = native_cpufreq_frame[0] + 1;
                frameEnd = t + frameAbs;
            }
        }
        const bool framing = t < frameEnd;
        if (framing) native_cpufreq_frame[1] = native_cpufreq_frame[1] + 1;
        const bool boost = t < boostEnd;
        uint64_t busy[3] = {};   // per mille of the busiest CPU
        for (unsigned c = 0; c < kCpus; ++c) {
            const uint64_t idle = sheng_cpu_idle_mach((int)c), di = idle - lastIdle[c];
            lastIdle[c] = idle;
            const uint64_t b = di >= dt ? 0 : (dt - di) * 1000 / dt;
            const int d = kCpuDomain[c];
            if (b > busy[d]) busy[d] = b;
        }
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(dt, &ns);
        winNs += ns;
        IOLockLock(gLock);
        for (unsigned d = 0; d < 3; ++d) {
            Window &w = gDomain[d];
            if (!w.va || !gUsable[d]) continue;
            Levels &l = gLevels[d];
            if (!ready[d]) {
                buildLevels(d);
                if (!l.count) continue;
                w.preKHz = currentKHz(w.va);
                cur[d] = l.count - 1;
                setLevel(d, cur[d]);
                ready[d] = true;
            }
            unsigned want = l.count - 1;
            if (busy[d] < 900) {
                const uint64_t need = l.khz[cur[d]] * busy[d] * 125 / 100000;
                want = 0;
                while (want + 1 < l.count && l.khz[want] < need) ++want;
            }
            if (framing)
                while (want + 1 < l.count && (uint64_t)l.khz[want] * 100 < (uint64_t)l.khz[l.count - 1] * gFramePct) ++want;
            if (d > 0) {
                quiet[d] = busy[d] < 100 ? quiet[d] + 1 : 0;
                if (boost) want = l.count - 1;
                else if (quiet[d] < 12)
                    while (want + 1 < l.count && l.khz[want] * 2 < l.khz[l.count - 1]) ++want;
            }
            if (want > cur[d]) {
                cur[d] = want; down[d] = 0; setLevel(d, want); ++changes[d];
            } else if (want < cur[d]) {
                if (++down[d] >= 4) {cur[d] = want; down[d] = 0; setLevel(d, want); ++changes[d];}
            } else {
                down[d] = 0;
            }
            khzNs[d] += (uint64_t)l.khz[cur[d]] / 1000 * ns;
            busyNs[d] += busy[d] * ns / 10;
        }
        IOLockUnlock(gLock);
        if (winNs >= 10000000000ull) {
            for (unsigned d = 0; d < 3; ++d) {
                native_cpufreq_gov[d * 3] = changes[d];
                native_cpufreq_gov[d * 3 + 1] = (uint32_t)(khzNs[d] / winNs);
                native_cpufreq_gov[d * 3 + 2] = (uint32_t)(busyNs[d] / winNs);
                changes[d] = 0; khzNs[d] = busyNs[d] = 0;
            }
            uint32_t cycMHz[3] = {};
            for (unsigned d = 0; d < 3; ++d) {
                cycMHz[d] = winNs ? (uint32_t)(cycDelta[d] * 1000 / winNs) : 0;
                cycDelta[d] = 0;
            }
            winNs = 0;
            refreshRow();
            // ps is the index the engine reads back, rd the frequency that row decodes to: a vote the OSM refused or an
            // LMh cap keeps ps/rd below the mean MHz above. cyc is the 0x9c0 cycle-counter delta over the same window,
            // i.e. the clock the cores actually ran at.
            printf("PICASSO_CPUFREQ gov d0 chg=%u mhz=%u busy=%u ps=%u rd=%u cyc=%u d1 chg=%u mhz=%u busy=%u ps=%u "
                   "rd=%u cyc=%u d2 chg=%u mhz=%u busy=%u ps=%u rd=%u cyc=%u boosts=%u\n",
                   native_cpufreq_gov[0], native_cpufreq_gov[1], native_cpufreq_gov[2],
                   gDomain[0].va ? rd32(gDomain[0].va, kRegPerfState) : 0, windowKHz(gDomain[0]) / 1000,
                   cycMHz[0],
                   native_cpufreq_gov[3], native_cpufreq_gov[4], native_cpufreq_gov[5],
                   gDomain[1].va ? rd32(gDomain[1].va, kRegPerfState) : 0, windowKHz(gDomain[1]) / 1000,
                   cycMHz[1],
                   native_cpufreq_gov[6], native_cpufreq_gov[7], native_cpufreq_gov[8],
                   gDomain[2].va ? rd32(gDomain[2].va, kRegPerfState) : 0, windowKHz(gDomain[2]) / 1000,
                   cycMHz[2],
                   native_cpufreq_boosts);
            // The TOUCH row rides this 10 s tick: CONFIG_XNU_BOOT_PROGRESS is off in this
            // build, so the shared boot monitor's rows never run and the drivers report here.
            {
                static uint32_t lf, lh, le1, le2, le3;
                const uint32_t f = native_novatek_touch_frames, h = native_touch_hid_reports;
                const uint32_t e1 = native_touch_edge_stats[1], e2 = native_touch_edge_stats[2];
                const uint32_t e3 = native_touch_edge_stats[3];
                printf("TOUCH d_frames=%u d_hid=%u d_begin=%u d_edge=%u d_skip=%u phys=%u count=%u\n",
                       f - lf, h - lh, e1 - le1, e2 - le2, e3 - le3,
                       (unsigned)native_touch_physical_active, (unsigned)native_touch_count);
                lf = f; lh = h; le1 = e1; le2 = e2; le3 = e3;
            }
        }
    }
}

// Read-only CPU usage (modes 1-3): per-CPU busy share over 1 s windows, stored and printed as a 10 s average. It is the
// measurement half of this probe and makes no votes, so a mode 1 boot shows the real load without touching a domain.
void usageThread(void *, wait_result_t) {
    uint64_t lastIdle[kCpus] = {}, lastT = mach_absolute_time();
    for (unsigned c = 0; c < kCpus; ++c) lastIdle[c] = sheng_cpu_idle_mach((int)c);
    uint64_t busySum[kCpus] = {};
    unsigned seen[kCpus] = {}, ticks = 0;
    for (;;) {
        IOSleep(1000);
        const uint64_t t = mach_absolute_time(), dt = t - lastT;
        if (!dt) continue;
        lastT = t;
        for (unsigned c = 0; c < kCpus; ++c) {
            const uint64_t idle = sheng_cpu_idle_mach((int)c);
            if (!idle) { lastIdle[c] = 0; continue; }   // no processor: this window has no sample
            const uint64_t di = idle - lastIdle[c];
            lastIdle[c] = idle;
            busySum[c] += di >= dt ? 0 : (dt - di) * 1000 / dt;
            ++seen[c];
        }
        if (++ticks < 10) continue;
        printf("PICASSO_CPUFREQ usage");
        for (unsigned c = 0; c < kCpus; ++c) {
            if (!seen[c]) {
                native_cpufreq_usage[c] = UINT32_MAX;
                printf(" c%u=-", c);
            } else {
                const uint32_t perMille = (uint32_t)(busySum[c] / seen[c]);
                native_cpufreq_usage[c] = perMille;
                printf(" c%u=%u.%u%%", c, perMille / 10, perMille % 10);
            }
            busySum[c] = 0; seen[c] = 0;
        }
        printf("\n");
        ticks = 0;
    }
}
}

extern "C" void sheng_cpufreq_poll(void) {
    if (!gMode || !gLock) return;
    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        bool wanted = (gMode == 2 && d == 0) || gMode == 3 || gMode == 4;
        if (w.va && wanted && w.voted < 0 && gUsable[d]) {
            voteDomain(d);
            // The index after the store is what the OSM accepted; the decoded frequency is what it means.
            printf("PICASSO_CPUFREQ vote d%u level=%d khz=%u pre=%u enable=%u ps=%u rd=%u\n", d, w.voted, w.votedKHz,
                   w.preKHz, rd32(w.va, kRegEnable), rd32(w.va, kRegPerfState), currentKHz(w.va));
        }
    }
    refreshRow();
    native_cpufreq_stage = 2;
    IOLockUnlock(gLock);
}

extern "C" void sheng_cpufreq_probe(void) {
    unsigned mode = 4, capMHz = 0, minMHz = 0, boostMs = 1000, frameMs = 300, framePct = 60;
    PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode));
    PE_parse_boot_argn("sheng-cpufreq-cap", &capMHz, sizeof(capMHz));
    PE_parse_boot_argn("sheng-cpufreq-min", &minMHz, sizeof(minMHz));
    PE_parse_boot_argn("sheng-cpufreq-boost-ms", &boostMs, sizeof(boostMs));
    PE_parse_boot_argn("sheng-cpufreq-frame-ms", &frameMs, sizeof(frameMs));
    gBoostMs = boostMs;
    PE_parse_boot_argn("sheng-cpufreq-frame-pct", &framePct, sizeof(framePct));
    gFrameMs = frameMs;
    gFramePct = framePct > 100 ? 100 : framePct;
    gFloorKHz = minMHz * 1000;
    if (!mode || gLock) return;
    gLock = IOLockAlloc();
    if (!gLock) return;
    native_cpufreq_stage = 1;
    gCapKHz = capMHz * 1000;
    for (unsigned d = 0; d < 3; ++d) mapWindow(gDomain[d], kDomainBase[d]);
    for (unsigned d = 0; d < 3; ++d) gUsable[d] = gDomain[d].va && domainUsable(d);
    for (unsigned c = 0; c < kCpus; ++c) native_cpufreq_usage[c] = UINT32_MAX;
    gMode = mode;
    printf("PICASSO_CPUFREQ probe mode=%u caps=%u min=%u boost=%u/%u frame=%u/%u maps=%u%u%u\n",
           mode, gCapKHz, gFloorKHz, gBoostMs, gFrameMs, gFramePct, gFrameMs,
           gDomain[0].va != 0, gDomain[1].va != 0, gDomain[2].va != 0);
    // The scheduler's view of the CPU clusters: one line is enough to tell a flat eight-core topology from an E/P one.
    {
        const ml_topology_info_t *topology = ml_get_topology_info();
        if (topology) {
            printf("PICASSO_CPUFREQ topo cpus=%u clusters=%u types=%x en=%u pn=%u cl-e=%u cl-p=%u\n",
                   topology->num_cpus, topology->num_clusters, topology->cluster_types,
                   ml_get_cpu_number_type(CLUSTER_TYPE_E, true, true),
                   ml_get_cpu_number_type(CLUSTER_TYPE_P, true, true),
                   ml_get_cluster_number_type(CLUSTER_TYPE_E),
                   ml_get_cluster_number_type(CLUSTER_TYPE_P));
            for (unsigned i = 0; i < topology->num_cpus; ++i)
                printf("PICASSO_CPUFREQ topo cpu=%u phys=%x cluster=%u type=%u\n",
                       topology->cpus[i].cpu_id, topology->cpus[i].phys_id,
                       topology->cpus[i].cluster_id, topology->cpus[i].cluster_type);
        }
        // Read the same property straight from the boot device tree: separates a missing property from a parser that
        // did not consume it.
        DTEntry entry = NULL;
        void const *prop = nullptr;
        unsigned int propSize = 0;
        if (SecureDTLookupEntry(NULL, "/cpus/cpu@600", &entry) == kSuccess &&
            SecureDTGetProperty(entry, "cluster-type", &prop, &propSize) == kSuccess)
            printf("PICASSO_CPUFREQ topo dt cpu600-cluster-type size=%u value=%x\n",
                   propSize, propSize ? *(const uint8_t *)prop : 0);
        else
            printf("PICASSO_CPUFREQ topo dt cpu600-cluster-type missing\n");
        if (SecureDTLookupEntry(NULL, "/", &entry) == kSuccess &&
            SecureDTGetProperty(entry, "model", &prop, &propSize) == kSuccess)
            printf("PICASSO_CPUFREQ topo dt model %.48s\n", (const char *)prop);
        const char *cpuPaths[2] = {"/cpus/cpu@0", "/cpus/cpu@600"};
        for (unsigned i = 0; i < 2; ++i) {
            if (SecureDTLookupEntry(NULL, cpuPaths[i], &entry) == kSuccess &&
                SecureDTGetProperty(entry, "capacity-dmips-mhz", &prop, &propSize) == kSuccess)
                printf("PICASSO_CPUFREQ topo dt %s capacity size=%u value=%u\n",
                       cpuPaths[i], propSize, propSize == 4 ? *(const uint32_t *)prop : 0);
            else
                printf("PICASSO_CPUFREQ topo dt %s capacity missing\n", cpuPaths[i]);
        }
    }
    // Read-only first look: enable, perf-state index, the engine's current frequency, the 0x704 vote (diagnostic) and
    // the highest LUT row. This line is what a mode 1 boot reports.
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        if (!w.va) {printf("PICASSO_CPUFREQ d%u nomap\n", d); continue;}
        uint32_t khz = 0; unsigned rows = 0;
        int level = pickLevel(d, &khz, &rows);
        printf("PICASSO_CPUFREQ d%u enable=%u raw=%08x usable=%u perf=%u cur=%ukHz vote704=%u best=%d@%ukHz rows=%u\n",
               d, rd32(w.va, kRegEnable) & 1, rd32(w.va, kRegEnable), gUsable[d], rd32(w.va, kRegPerfState),
               currentKHz(w.va), rd32(w.va, kRegCurrentVote) & 0x3ff, level, khz, rows);
        // Raw LUT rows (hex) up to the terminator: src/lval/core-count decode is in the file header, and a row that
        // does not match the expected max_cores is visible here.
        printf("PICASSO_CPUFREQ d%u lut=", d);
        uint32_t prev = 0;
        for (unsigned i = 0; i < kLutEntries; ++i) {
            const uint32_t row = rd32(w.va, kRegFreqLut + i * kLutRowSize), rowKHz = lutKHz(row);
            if (i > 0 && rowKHz == prev) break;
            prev = rowKHz;
            printf("%08x,", row);
        }
        printf("\n");
    }
    sheng_cpufreq_poll();
    thread_t thread = nullptr;
    if (mode >= 4) {
        if (kernel_thread_start(governorThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    } else {
        // Modes 1-3 make no periodic votes (mode 3 votes once per domain as it enables); the usage thread only measures.
        if (kernel_thread_start(usageThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}
