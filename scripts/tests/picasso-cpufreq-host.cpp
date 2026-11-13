// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
struct Levels { unsigned count; uint32_t khz[9]; };
// Non-turbo rows read from this board's hardware LUT, in ascending order.
static Levels levels[3] = {
    {9,{300000,576000,614400,864000,902400,1363200,1516800,1651200,1804800}},
    {8,{652800,940800,1094400,1478400,1728000,1900800,2092800,2208000}},
    {8,{806400,1094400,1401600,1766400,1996800,2188800,2304000,2400000}}
};
static unsigned cur[3], down[3], changes[3], gFramePct = 60;
[[maybe_unused]] static unsigned quiet[3]; // Also permits testing the prior source.
static uint64_t busy[3];
static bool boost, framing;
static void setLevel(unsigned d, unsigned at) { assert(at < levels[d].count); }
static void step(unsigned d, unsigned load) {
    busy[d] = load; Levels &l = levels[d];
#include "decision.inc"
}
static void reset(unsigned d) {
    cur[d] = levels[d].count-1; down[d] = changes[d] = quiet[d] = 0;
    boost = framing = false;
}
int main() {
    for (unsigned d=0; d<3; ++d) {
        reset(d);
        for (unsigned i=0; i<80; ++i) step(d, i%4==0 ? 120 : 50);
        // Short background bursts must not impose a half-maximum floor.
        assert(cur[d]==0);
        reset(d);
        for (unsigned i=0; i<3; ++i) step(d, 0);
        assert(cur[d]==levels[d].count-1);
        step(d, 0); assert(cur[d]==0);
        step(d, 900); assert(cur[d]==levels[d].count-1);
        for (unsigned i=0; i<3; ++i) step(d, 0);
        step(d, 1000); step(d, 0);
        assert(cur[d]==levels[d].count-1); // Interrupted descent restarts.
        for (unsigned i=0; i<20; ++i) step(d, 0);
        boost=true; step(d,0);
        assert(cur[d]==(d ? levels[d].count-1 : 0));
        boost=false;
        for (unsigned i=0; i<4; ++i) step(d,0);
        assert(cur[d]==0);
        framing=true; step(d,0);
        const uint64_t need=uint64_t(levels[d].khz[levels[d].count-1])*60;
        assert(uint64_t(levels[d].khz[cur[d]])*100>=need);
        assert(!cur[d] || uint64_t(levels[d].khz[cur[d]-1])*100<need);
        framing=false;
        for (unsigned i=0; i<4; ++i) step(d,0);
        assert(cur[d]==0);
        // A single allowed OPP (floor/cap) stays valid under all inputs.
        const unsigned count=levels[d].count; levels[d].count=1; reset(d);
        for (unsigned load=0; load<=1000; ++load) {
            boost=load%2; framing=load%3; step(d,load); assert(cur[d]==0);
        }
        levels[d].count=count;
    }
    puts("PASS low-load descent, four-window hysteresis, high load, touch, frame floor and OPP bounds");
}
