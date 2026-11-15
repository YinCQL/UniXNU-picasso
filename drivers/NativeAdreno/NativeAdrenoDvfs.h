/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_DVFS_H
#define NATIVE_ADRENO_DVFS_H
#include <stdint.h>

/* Board callbacks and the governor run under the publication/submit lock.
 * current is the last acknowledged HFI vote, not a measured clock rate. */
struct NativeAdrenoDvfsSample {
    uint64_t cycles;
    uint32_t busyTicks, count, current, hz[16];
};
struct NativeAdrenoDvfsPolicy {
    uint64_t sampled=0, lastWork=0, boostUntil=0, lastCycles=0;
    uint32_t lastBusy=0, low=0, busyPermille=0, elapsedUs=0;
    uint64_t cycleDelta=0;
    bool based=false, valid=false;
    static bool usable(const NativeAdrenoDvfsSample &s) {
        if(s.count<2 || s.count>16 || s.current<1 || s.current>=s.count || s.hz[0])return false;
        for(uint32_t i=1;i<s.count;++i)if(s.hz[i]<=s.hz[i-1])return false;
        return true;
    }
    void invalidate() {based=false;valid=false;low=0;}
    void baseline(uint64_t now,const NativeAdrenoDvfsSample &s) {
        sampled=now;lastBusy=s.busyTicks;lastCycles=s.cycles;based=true;
    }
    uint32_t work(uint64_t now,const NativeAdrenoDvfsSample &s,bool parked) {
        const bool wake=parked || (lastWork && now-lastWork>=250000);
        lastWork=now;
        if(!usable(s) || !wake)return s.current;
        boostUntil=now+100000;low=0;
        const uint64_t target=uint64_t(s.hz[1])*2;
        uint32_t level=1;
        while(level+1<s.count && s.hz[level]<target)++level;
        return level>s.current?level:s.current;
    }
    uint32_t sample(uint64_t now,const NativeAdrenoDvfsSample &s,bool idle) {
        valid=false;
        if(!usable(s)) {invalidate();return s.current;}
        if(!based || now<=sampled) {baseline(now,s);low=0;return s.current;}
        const uint64_t dt=now-sampled;
        if(dt<50000)return s.current;
        const uint32_t ticks=s.busyTicks-lastBusy; // modulo 32-bit XO rollover
        const uint64_t dc=s.cycles-lastCycles;
        baseline(now,s);
        /* A long scheduling gap or reset is not low utilization. The 5% margin
         * admits MMIO/time sampling skew; larger deltas discard this window. */
        if(dt>250000 || uint64_t(ticks)*10>dt*192*105/100 ||
+            dc>uint64_t(s.hz[s.count-1])*dt/1000000*105/100) {low=0;return s.current;}
        elapsedUs=uint32_t(dt);cycleDelta=dc;
        busyPermille=uint32_t(uint64_t(ticks)*10000/(dt*192));
        if(busyPermille>1000)busyPermille=1000;
        valid=true;
        if(now<boostUntil) {low=0;return s.current;}
        if(idle && now-lastWork>=250000 && !ticks) {low=0;return 1;}
        if(busyPermille>500) {
            low=0;
            const uint64_t target=uint64_t(s.hz[s.current])*busyPermille*2/1000;
            uint32_t level=s.current;
            while(level+1<s.count && s.hz[level]<target)++level;
            return level;
        }
        if(busyPermille<200) {
            if(++low>=2) {low=0;return s.current>1?s.current-1:1;}
        } else low=0;
        return s.current;
    }
    bool canPark(uint64_t now,const NativeAdrenoDvfsSample &s,bool idle) const {
        return valid && idle && s.current==1 && busyPermille==0 &&
            now-lastWork>=250000 && now>=boostUntil;
    }
};
#endif
