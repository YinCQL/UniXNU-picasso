/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PROFILE_H
#define NATIVE_ADRENO_PROFILE_H
#include <stdint.h>
/* Opt-in ngp=1 elapsed-time diagnostics. Nested phases must not be summed. */
enum NativeAdrenoProfilePhase {
    NativeProfileCall, NativeProfileLock, NativeProfileDispatch, NativeProfileBody,
    NativeProfileValidate, NativeProfileRetire, NativeProfileAudit, NativeProfilePublish,
    NativeProfileSwitch, NativeProfileDrain, NativeProfileRing, NativeProfileAcquire,
    NativeProfileIbPeek,
    NativeProfilePhaseCount
};
enum NativeAdrenoProfileCounter {
    NativeProfilePublishVisited, NativeProfilePublishSkipped, NativeProfilePublishCandidates, NativeProfilePublishBytes,
    NativeProfileAcquireVisited, NativeProfileAcquireSkipped, NativeProfileAcquireCandidates, NativeProfileAcquireBytes,
    NativeProfileCounterCount
};
extern "C" {
extern uint32_t native_adreno_profile_enabled;
extern uint64_t (*native_adreno_vm_now_us)(void);
extern volatile uint64_t native_adreno_profile_phases[NativeProfilePhaseCount][3];
extern volatile uint64_t native_adreno_profile_counters[NativeProfileCounterCount];
void native_adreno_profile_report(void);
}
static inline uint64_t nativeAdrenoProfileNow()
{
    return native_adreno_profile_enabled && native_adreno_vm_now_us ? native_adreno_vm_now_us() : 0;
}
static inline void nativeAdrenoProfileAdd(unsigned slot,uint64_t value)
{
    if(native_adreno_profile_enabled)
        __atomic_fetch_add(&native_adreno_profile_counters[slot],value,__ATOMIC_RELAXED);
}
static inline void nativeAdrenoProfileElapsed(unsigned phase,uint64_t began)
{
    if(!began)return;
    const uint64_t now=nativeAdrenoProfileNow();
    if(now<began)return;
    const uint64_t elapsed=now-began;
    auto *row=native_adreno_profile_phases[phase];
    __atomic_fetch_add(&row[0],1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&row[1],elapsed,__ATOMIC_RELAXED);
    uint64_t maximum=__atomic_load_n(&row[2],__ATOMIC_RELAXED);
    while(elapsed>maximum && !__atomic_compare_exchange_n(&row[2],&maximum,elapsed,true,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
class NativeAdrenoProfileTimer {
    unsigned phase_;
    uint64_t began_;
public:
    explicit NativeAdrenoProfileTimer(unsigned phase,bool active=true):phase_(phase),began_(active?nativeAdrenoProfileNow():0) {}
    ~NativeAdrenoProfileTimer() {finish();}
    void finish() {nativeAdrenoProfileElapsed(phase_,began_);began_=0;}
};
#endif
