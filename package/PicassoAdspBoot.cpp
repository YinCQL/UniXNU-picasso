// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito PIL order: AOP on, proxy rails/XO, INIT, MEM_SETUP, load, AUTH;
// shutdown before AOP off; retain shared ARC floors pending an aggregate owner.
// No automatic restart or fake PCM endpoint.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "PicassoAdspBoot.h"
#include "PicassoAdspLoadPlan.h"
#include "PicassoRpmh.h"
#include "PicassoPas.h"

extern "C" int picasso_pas_acquire(void) __attribute__((weak_import));
extern "C" void picasso_pas_release(bool) __attribute__((weak_import));
extern "C" int picasso_pas_adsp(unsigned,uint64_t,uint64_t) __attribute__((weak_import));
extern "C" int picasso_rpmh_adsp_resources(PicassoArcResource *) __attribute__((weak_import));
extern "C" int picasso_rpmh_crypto_ready(void) __attribute__((weak_import));
extern "C" int picasso_rpmh_active_write(uint32_t,uint32_t) __attribute__((weak_import));

namespace {
IOMemoryMap *bootMap(uint64_t base,uint64_t bytes) {
    IODeviceMemory *m=IODeviceMemory::withRange(base,bytes);
    if(!m)return nullptr;
    auto *map=m->map(kIOMapAnywhere|kIOMapInhibitCache);m->release();return map;
}
uint64_t bootDeadline(unsigned seconds) {
    uint64_t value;clock_interval_to_deadline(seconds,kSecondScale,&value);return value;
}
void bootFence() {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
struct Boot {
    const PicassoAdspEvents *events;
    PicassoAdspImage *image;
    IOMemoryMap *metadata,*memory;
    PicassoArcResource rails[3];
    unsigned voted;
    bool aop,pas,authenticated,ready,handed,quarantined,lease;
    unsigned missed;
    const char *phase;
};
void reportRetainedPower(const Boot &b) {
    // DRV2 inherits boot-loader votes. Without an aggregate vote owner/readback,
    // the stock regulator's init level is NOT the prior APPS request. Retain
    // the supported ceiling (and XO on) even after shutdown; never lower it to
    // a guessed retention value. Full power saving needs a resource owner.
    IOLog("PICASSO_ADSP boot stage=power-retained resources=%u pending=aggregate-owner\n",b.voted);
}
int sample(Boot &b,PicassoSmp2pEvent &e) {
    int error=b.events->poll(b.events->context,&e);
    if(error==-2 || error==-11) {
        if(b.ready && ++b.missed>=50)return -57;
        return 0;
    }
    if(error)return error;
    // Never hide a restart/fatal behind a temporarily absent slave entry.
    if(e.restart || (e.hasValue && (e.value&1))) {
        IOLog("PICASSO_ADSP boot event=fault restart=%u entry=%u value=%x flags=%x\n",
            e.restart,e.hasValue,e.value,e.flags);
        return -5;
    }
    if(!e.hasValue && b.ready) {
        if(++b.missed>=50)return -57;
        return 0;
    }
    b.missed=0;
    if(e.hasValue && b.ready && !(e.value&2))return -57;
    if(e.hasValue && e.changed)
        IOLog("PICASSO_ADSP boot event value=%x changed=%x ready=%u handover=%u stop_ack=%u\n",
            e.value,e.changed,!!(e.value&2),!!(e.value&4),!!(e.value&8));
    if(e.hasValue && (e.value&2))b.ready=true;
    if(e.hasValue && (e.value&4))b.handed=true;
    return 0;
}
int start(Boot &b) {
    b.phase="peer-preflight";
    PicassoSmp2pEvent e={};int error=b.events->poll(b.events->context,&e);
    // Existing slave-kernel state is not evidence that this image was booted.
    if(error!= -2)return error ? error : -16;
    b.phase="firmware-reopen";
    b.image=picasso_adsp_image_open(&error);if(!b.image)return error;
    b.phase="power-plan";
    if((error=picasso_rpmh_adsp_resources(b.rails)))return error;
    b.phase="memory-ownership";
    const auto *args=static_cast<const boot_args *>(PE_state.bootArgs);
    // All PIL ranges lie below 0x8c000000. Reject a changed loader memory
    // contract before mapping or touching firmware memory.
    if(!args || args->physBase<0x8c000000ULL || !args->memSize)return -16;
    b.phase="mappings";
    b.metadata=bootMap(0x8bc18000,0x3000);b.memory=bootMap(picassoAdspBase,picassoAdspBytes);
    if(!b.metadata || !b.memory || !b.metadata->getVirtualAddress() || !b.memory->getVirtualAddress())return -12;
    b.phase="crypto-path";if((error=picasso_rpmh_crypto_ready()))return error;
    // File validation above does not own the shared metadata or an EL3
    // session. Do not block GPU PIL while reading tens of MiB from UFS.
    b.phase="lease";if((error=picasso_pas_acquire()))return error;b.lease=true;
    b.phase="aop-on";if((error=picasso_aop_load_state(true))){b.quarantined=error==-115;return error;}b.aop=true;
    b.phase="proxy-vote";
    for(unsigned i=0;i<3;++i) {
        ++b.voted; // A failed command may already have reached the resource.
        if((error=picasso_rpmh_active_write(b.rails[i].address,b.rails[i].index))) {
            b.quarantined=true;return error;
        }
    }
    b.phase="metadata-copy";
    auto *to=reinterpret_cast<volatile uint8_t *>(b.metadata->getVirtualAddress());
    auto *from=static_cast<const uint8_t *>(picasso_adsp_image_metadata(b.image));
    if(!from)return -22;
    for(unsigned i=0;i<0x3000;++i)to[i]=i<picassoAdspMetadataBytes?from[i]:0;
    bootFence();
    b.phase="pas-init";b.pas=true;
    if((error=picasso_pas_adsp(1,0x8bc18000,0)))return error;
    b.phase="pas-memory";if((error=picasso_pas_adsp(2,picassoAdspBase,picassoAdspBytes)))return error;
    b.phase="segments";
    if((error=picasso_adsp_image_load(b.image,reinterpret_cast<volatile uint8_t *>(b.memory->getVirtualAddress()),picassoAdspBytes)))return error;
    // Recheck the initially absent peer before AUTH. No inherited ready counts.
    b.phase="peer-before-auth";e={};error=b.events->poll(b.events->context,&e);if(error!=-2)return error?error:-16;
    b.phase="pas-auth";if((error=picasso_pas_adsp(5,0,0)))return error;b.authenticated=true;
    picasso_adsp_image_close(b.image);b.image=nullptr;
    b.phase="ready";const uint64_t end=bootDeadline(10);
    do {
        e={};if((error=sample(b,e)))return error;
        if(b.ready)break;
        IOSleep(10);
    }while(mach_absolute_time()<end);
    if(!b.ready)return -60;
    IOLog("PICASSO_ADSP boot stage=ready authenticated=1 ready=1 handover=%u pcm=0\n",b.handed);
    picasso_pas_release(false);b.lease=false;
    b.events->snapshot(b.events->context);
    // Keep a bounded completion owner alive for the DSP. A missing handover
    // retains proxy votes; no speculative unvote after a fixed timeout.
    b.phase="service";unsigned snapshots=0;uint64_t next=bootDeadline(5);bool handoverLogged=false;
    for(;;) {
        e={};if((error=sample(b,e)))return error;
        if(e.hasValue && (e.value&2) && b.events->service)b.events->service(b.events->context);
        if(b.handed && b.voted && !handoverLogged) {
            reportRetainedPower(b);
            handoverLogged=true;
            IOLog("PICASSO_ADSP boot stage=handover proxy_released=0 xo_floor=3 authenticated=1\n");
        }
        if(snapshots<2 && mach_absolute_time()>=next) {
            b.events->snapshot(b.events->context);++snapshots;next=bootDeadline(25);
        }
        IOSleep(20);
    }
}
void stop(Boot &b,int error) {
    if(error==-115)b.quarantined=true;
    if(b.events)b.events->snapshot(b.events->context);
    if(b.pas && !b.quarantined) {
        if(!b.lease) {
            if(picasso_pas_acquire())b.quarantined=true;
            else b.lease=true;
        }
        // Reassert supplies before shutdown. An uncertain command retains all
        // resources for this boot; no guessed low-power rollback is allowed.
        if(!b.quarantined)for(unsigned i=0;i<3;++i) {
            if(b.voted<i+1)b.voted=i+1;
            if(picasso_rpmh_active_write(b.rails[i].address,b.rails[i].index)){b.quarantined=true;break;}
        }
        if(!b.quarantined && picasso_pas_adsp(6,0,0))b.quarantined=true;
    }
    if(b.voted)reportRetainedPower(b);
    if(!b.quarantined && b.aop && picasso_aop_load_state(false))b.quarantined=true;
    if(!b.quarantined && picasso_aop_finish())b.quarantined=true;
    if(b.image){picasso_adsp_image_close(b.image);b.image=nullptr;}
    if(!b.quarantined) {
        if(b.memory)b.memory->release();
        if(b.metadata)b.metadata->release();
    }
    IOLog("PICASSO_ADSP boot stage=stopped phase=%s error=%d authenticated=%u ready=%u quarantined=%u retained_votes=%u pcm=0\n",
        b.phase,error,b.authenticated,b.ready,b.quarantined,b.voted);
}
}
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *events) {
    static unsigned attempted;
    if(__atomic_exchange_n(&attempted,1U,__ATOMIC_ACQ_REL))return;
    if(!events || !events->poll || !events->snapshot || !picasso_pas_acquire || !picasso_pas_release ||
        !picasso_pas_adsp || !picasso_rpmh_adsp_resources || !picasso_rpmh_crypto_ready || !picasso_rpmh_active_write) {
        IOLog("PICASSO_ADSP boot stage=owner error=-6\n");return;
    }
    Boot b={};b.events=events;
    int error=start(b);stop(b,error);if(b.lease)picasso_pas_release(b.quarantined);
}
