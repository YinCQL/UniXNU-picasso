// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito: TCSR mutex 3 @ 0x01f43000, APPS owner=1; SMEM private APPS/ADSP;
// IPCC LPASS client 3 / SMP2P signal 2. No LPASS MMIO or shared IRQ clearing.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/cpu_data.h>
#include <pexpert/pexpert.h>
#include "PicassoSmp2p.h"
#include "PicassoSmemLock.h"
#include "PicassoRpmh.h"
#include "PicassoAdspBoot.h"
#include "PicassoAdsp.h"
#include "PicassoGlink.h"
#include "PicassoRpcDma.h"
extern "C" void picasso_audio_service_tick(PicassoAprProbe &,bool);

// Display also builds without the optional Adreno archive. A missing shared
// owner means no power plan; never create an independent RSC writer.
extern "C" int picasso_rpmh_adsp_plan(void) __attribute__((weak_import));

namespace {
struct SmemDevice { uintptr_t ram, mutex, ipcc; bool held; };
static unsigned allocatorGate, started;
void smemBarrier(void *) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
uint32_t lockRead(void *ctx) { return *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->mutex); }
void lockWrite(void *ctx, uint32_t value) { *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->mutex) = value; }
bool lockHeld(void *ctx) { return static_cast<SmemDevice *>(ctx)->held && lockRead(ctx) == 1; }
int smemRead(void *ctx, uint32_t at, void *out, uint32_t n) {
    if (at > WLAN_SMEM_SIZE || n > WLAN_SMEM_SIZE - at) return -22;
    // Preserve atomic remote 32-bit publication/state words; byte loads could
    // tear a free pointer or ready/fatal value while the peer updates it.
    auto base = static_cast<SmemDevice *>(ctx)->ram;
    for (unsigned i = 0; i < n;) {
        uint32_t address = at + i, shift = address & 3;
        uint32_t word = *reinterpret_cast<volatile uint32_t *>(base + (address & ~3U));
        for (; shift < 4 && i < n; ++shift, ++i)
            static_cast<uint8_t *>(out)[i] = uint8_t(word >> (8 * shift));
    }
    smemBarrier(ctx); return 0;
}
int smemWrite(void *ctx, uint32_t at, uint32_t value) {
    if ((at & 3) || at > WLAN_SMEM_SIZE - 4) return -22;
    *reinterpret_cast<volatile uint32_t *>(static_cast<SmemDevice *>(ctx)->ram + at) = value;
    return 0;
}
void smpKick(SmemDevice *d) {
    smemBarrier(d);
    *reinterpret_cast<volatile uint32_t *>(d->ipcc + 0x0c) = (3U << 16) | 2;
    smemBarrier(d);
}
IOMemoryMap *smemMap(uint64_t base, uint64_t bytes) {
    IODeviceMemory *m = IODeviceMemory::withRange(base, bytes);
    if (!m) return nullptr;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release(); return map;
}
// One-shot audio pin transaction: map only the four documented GPIO pages.
// No global TLMM sweep, interrupt clearing, reset or amplifier writes.
int audioPinAccess(unsigned index,unsigned reg,uint32_t *value,bool write) {
    if(index>=4 || (reg!=0 && reg!=4) || !value)return -22;
    OSSharedPtr<IOMemoryMap> map(smemMap(picasso_audio_pin_pa(index),0x1000),OSNoRetain);
    if(!map || !map->getVirtualAddress())return -12;
    auto *p=reinterpret_cast<volatile uint32_t *>(uintptr_t(map->getVirtualAddress())+reg);
    if(write)*p=*value;else *value=*p;
    smemBarrier(nullptr);return 0;
}
int audioPinRead(void *,unsigned i,unsigned reg,uint32_t *v) { return audioPinAccess(i,reg,v,false); }
int audioPinWrite(void *,unsigned i,unsigned reg,uint32_t v) { return audioPinAccess(i,reg,&v,true); }
// Touch publishes stage >= 2 only after its one-time QUP vote update.
// Its worker never changes that vote again. Gate on this before sharing GCC.
extern "C" volatile uint32_t native_novatek_touch_stage __attribute__((weak_import));
struct TfaMaps { uintptr_t address[7]; };
int tfaRead(void *ctx,unsigned bank,unsigned reg,uint32_t *v) {
    auto *m=static_cast<TfaMaps *>(ctx);
    if(bank>=7 || reg&3 || reg>=(bank?0x1000U:0x4000U) || !m->address[bank] || !v)return -22;
    *v=*reinterpret_cast<volatile uint32_t *>(m->address[bank]+reg);smemBarrier(nullptr);return 0;
}
int tfaWrite(void *ctx,unsigned bank,unsigned reg,uint32_t v) {
    auto *m=static_cast<TfaMaps *>(ctx);
    if(bank>=7 || reg&3 || reg>=(bank?0x1000U:0x4000U) || !m->address[bank])return -22;
    *reinterpret_cast<volatile uint32_t *>(m->address[bank]+reg)=v;smemBarrier(nullptr);return 0;
}
void tfaDelay(void *,unsigned us) { if(us>=1000)IOSleep((us+999)/1000);else IODelay(us); }
void tfaProbe(PicassoTfa &out) {
    if(!&native_novatek_touch_stage || native_novatek_touch_stage<2) {
        out.attempted=true;out.error=-16;out.stage=0;++out.events;return;
    }
    IOLog("PICASSO_ADSP tfa stage=map se=988000 address=34 reset=58 id_reg=3 pcm=0\n");
    IOMemoryMap *maps[7]={};TfaMaps context={};
    bool ok=true;
    for(unsigned i=0;i<7 && ok;++i) {
        maps[i]=smemMap(picasso_tfa_base(i),i?0x1000:0x4000);
        context.address[i]=maps[i]?uintptr_t(maps[i]->getVirtualAddress()):0;
        ok=!!context.address[i];
    }
    if(ok) {
        PicassoTfaIo io={&context,tfaRead,tfaWrite,tfaDelay};picasso_tfa_run(io,out);
    } else { out.attempted=true;out.error=-12;++out.events; }
    for(auto *m:maps)if(m)m->release();
}
// Single ADSP worker, one bounded playback per boot. Preserve mappings while
// the lease owns SE8; stack-backed state would disappear between APR replies.
static TfaMaps playTfaContext={};
static IOMemoryMap *playTfaMaps[7]={};
static PicassoTfa playTfa={};
static const PicassoTfaIo playTfaIo={&playTfaContext,tfaRead,tfaWrite,tfaDelay};
static PicassoTfaSession playTfaSession={playTfaIo,playTfa,{},{},0,0,false,false,false,false};
static bool playTfaMapped=false;
int tfaPlayback(PicassoPlayback &v,unsigned action) {
    auto &a=v.amp;int e=0;
    if(action==0) {
        if(playTfaMapped || a.attempted)return -16;
        if(playTfa.attempted) {
            if(!playTfa.restored || playTfa.dirty || playTfa.uncertain)return -16;
            playTfa={};playTfaSession.pinDirty=playTfaSession.configDirty=0;
            playTfaSession.voteDirty=playTfaSession.command=playTfaSession.stretch=playTfaSession.prepared=false;
        }
        if(!&native_novatek_touch_stage || native_novatek_touch_stage<2)return a.error=-16;
        for(unsigned i=0;i<7 && !e;++i) {
            playTfaMaps[i]=smemMap(picasso_tfa_base(i),i?0x1000:0x4000);
            playTfaContext.address[i]=playTfaMaps[i]?uintptr_t(playTfaMaps[i]->getVirtualAddress()):0;
            if(!playTfaContext.address[i])e=-12;
        }
        if(!e) {playTfaMapped=true;e=picasso_tfa_play_prepare(playTfaSession,a);}
        else {
            a.error=e;
            for(auto *&m:playTfaMaps)if(m) {m->release();m=nullptr;}
        }
    } else if(action==1)e=playTfaMapped?picasso_tfa_play_enable(playTfaSession,a):-22;
    else if(action==3) {
        e=playTfaMapped?picasso_tfa_play_status(playTfaSession,a,true):-22;
        if(!e)e=picasso_tfa_play_serial(playTfaSession,a);
        if(e && !a.error)a.error=e;
    }
    else if(action==2) {
        if(playTfaMapped) {
            e=picasso_tfa_play_finish(playTfaSession,a);
            // Keep uncertain engine mappings/clock owned for diagnostics.
            if(playTfa.restored) {
                for(auto *&m:playTfaMaps)if(m) {m->release();m=nullptr;}
                playTfaMapped=false;
            }
        }
    } else e=-22;
    return e;
}
void logTfa(const PicassoTfa &t) {
    IOLog("PICASSO_ADSP tfa stage=%u error=%d cleanup_error=%d id=%x identified=%u reads=%u complete=%u restored=%u dirty=%x uncertain=%u pcm=0\n",
        t.stage,t.error,t.cleanupError,t.revision,t.identified,t.reads,t.complete,t.restored,t.dirty,t.uncertain);
    IOLog("PICASSO_ADSP tfa-bus fw=%x if=%x hw=%x/%x level=%u cfg=%x irq=%x rx=%x ios=%x commands=%u reg=%x abort=%x\n",
        t.fw,t.ifDisable,t.hw[0],t.hw[1],t.clockLevel,t.clockCfg,t.irq,t.rxStatus,t.ios,t.commands,t.reg,t.abortStatus);
    IOLog("PICASSO_ADSP tfa-pins before=%x/%x/%x active=%x/%x/%x reset_high_ms=10 reset_low_ms=10\n",
        t.pinBefore[0],t.pinBefore[1],t.pinBefore[2],t.pinAfter[0],t.pinAfter[1],t.pinAfter[2]);
    if(t.complete)IOLog("PICASSO_ADSP tfa-config regs=00/01/02/04/20/21/22/23/26 values=%x/%x/%x/%x/%x/%x/%x/%x/%x snapshot_before=1 pcm=0\n",
        t.values[0],t.values[1],t.values[2],t.values[3],t.values[4],t.values[5],t.values[6],t.values[7],t.values[8]);
    IOLog("PICASSO_ADSP tfa-digital attempted=%u applied=%u error=%d restored=%u restore_error=%d dirty=%x writes=%u regs20_22=%x/%x/%x amp_enable_write=0 pcm=0\n",
        t.digitalAttempted,t.digitalApplied,t.digitalError,t.digitalRestored,t.digitalRestoreError,t.digitalDirty,t.registerWrites,t.digital[0],t.digital[1],t.digital[2]);
}
void logStream(const PicassoAprProbe &a) {
    const auto &v=a.stream;
    IOLog("PICASSO_ADSP asm phase=%u token=%x api=%u sent=%u pending=%u responded=%u status=%u error=%d session=1 stream=1\n",
        v.phase,v.token,v.api,v.sent,v.pending,v.responded,v.status,v.error);
    IOLog("PICASSO_ADSP asm-state open=%u format=%u close=%u uncertain=%u done=%u complete=%u rate=48000 bits=16 channels=2 run=0 buffers=0 pcm=0\n",
        v.openAccepted,v.formatAccepted,v.closeAccepted,v.uncertain,v.done,v.completed);
    IOLog("PICASSO_ADSP asm-result open_error=%d format_error=%d status=%u/%u/%u\n",
        v.openError,v.formatError,v.statuses[0],v.statuses[1],v.statuses[2]);
}
struct PlayLogState {
    uint32_t phase = ~0U, tokenBase = 0, completed = 0;
};
enum PlayLogKind { PlayLogNone, PlayLogFull, PlayLogProgress };
PlayLogKind playLogKind(PlayLogState &last, const PicassoAprProbe &a) {
    const auto &v = a.play;
    bool transition = last.phase != v.phase || last.tokenBase != v.tokenBase;
    bool failed = v.status || v.error || v.firstError || v.cleanupError || v.dmaError ||
        v.writeStatus || v.replyReject || v.rejected || v.pins.error || v.pins.restoreError ||
        v.amp.error || v.amp.stopError || a.pcm.error || a.pcm.dmaError || (a.pcm.fsr & 0xc00001feU);
    last.phase = v.phase;
    last.tokenBase = v.tokenBase;
    // Preserve setup, teardown and fault details. A healthy stream needs only
    // a progress sample every 128 completions, about eleven seconds at 48 kHz.
    if (transition || v.phase != PicassoPlayWrite || failed || v.done || v.complete) {
        last.completed = v.writeDone;
        return PlayLogFull;
    }
    if (uint32_t(v.writeDone - last.completed) >= 128) {
        last.completed = v.writeDone;
        return PlayLogProgress;
    }
    return PlayLogNone;
}
void logPlayProgress(const PicassoAprProbe &a) {
    const auto &v = a.play;
    IOLog("PICASSO_ADSP play-queue sent=%u returned=%u inflight=%x dma_busy=%x bytes=%u/%u ticks=%u source=%u slot_bytes=16384\n",
        v.writeSent,v.writeDone,v.inflight,a.pcm.busyMask,v.bytesSent,v.bytesDone,v.streamTicks,!!v.source);
}
void logPlay(const PicassoAprProbe &a) {
    const auto &v=a.play;
    IOLog("PICASSO_ADSP play phase=%u token=%x pending=%u opcode=%x status=%u error=%d first_error=%d cleanup_error=%d dma_error=%d\n",
        v.phase,v.token,v.pending,v.opcode,v.status,v.error,v.firstError,v.cleanupError,v.dmaError);
    IOLog("PICASSO_ADSP play-state accepted=%x owned=%x copp=%u handle=%x consumed=%u done=%u complete=%u bytes=%u silence=%u amp_enable=%u\n",
        v.accepted,v.owned,v.copp,v.handle,v.consumed,v.done,v.complete,picasso_play_queued(v)?v.bytesSent:4096,!picasso_play_queued(v),v.amp.wasEnabled);
    IOLog("PICASSO_ADSP play-queue sent=%u returned=%u inflight=%x dma_busy=%x bytes=%u/%u ticks=%u source=%u slot_bytes=16384\n",
        v.writeSent,v.writeDone,v.inflight,a.pcm.busyMask,v.bytesSent,v.bytesDone,v.streamTicks,!!v.source);
    IOLog("PICASSO_ADSP play-serial regs20_21_22_23_26_6e=%x/%x/%x/%x/%x/%x enable_requested=%u\n",
        v.amp.serial[0],v.amp.serial[1],v.amp.serial[2],v.amp.serial[3],v.amp.serial[4],v.amp.serial[5],v.amp.enableRequested);
    IOLog("PICASSO_ADSP play-pins active=%u restored=%u dirty=%x error=%d restore_error=%d\n",
        v.pins.active,v.pins.restored,v.pins.dirty,v.pins.error,v.pins.restoreError);
    IOLog("PICASSO_ADSP play-write address=%08x%08x handle=%x status=%u fsr=%x fault=%x\n",
        v.writeAddressHigh,v.writeAddressLow,v.writeHandle,v.writeStatus,a.pcm.fsr,a.pcm.fsr&0xc00001feU);
    IOLog("PICASSO_ADSP play-rx phase=%u opcode=%x type=%u bytes=%u source=%x destination=%x reject=%u rejected=%u\n",
        v.replyPhase,v.replyOpcode,v.replyType,v.replyBytes,v.replySource,v.replyDestination,v.replyReject,v.rejected);
    IOLog("PICASSO_ADSP play-raw words=%08x/%08x/%08x/%08x\n",
        v.replyRaw[0],v.replyRaw[1],v.replyRaw[2],v.replyRaw[3]);
    IOLog("PICASSO_ADSP play-amp stage=%u configured=%u clks=%u enabled=%u stopped=%u reset=%u error=%d stop_error=%d regs=%u polls=%u drained=%u\n",
        v.amp.stage,v.amp.configured,v.amp.clockReady,v.amp.enabled,v.amp.stopped,v.amp.resetSafe,v.amp.error,v.amp.stopError,v.amp.registers,v.amp.polls,v.drained);
    if(v.amplifier)IOLog("PICASSO_ADSP play-amp-bus reg=%x irq=%x commands=%u writes=%u dirty=%x uncertain=%u cleanup_error=%d\n",
        playTfa.reg,playTfa.irq,playTfa.commands,playTfa.registerWrites,playTfa.dirty,playTfa.uncertain,playTfa.cleanupError);
    IOLog("PICASSO_ADSP play-amp-reg status10_11_13_14=%x/%x/%x/%x control00_01_02=%x/%x/%x\n",
        v.amp.status[0],v.amp.status[1],v.amp.status[2],v.amp.status[3],v.amp.control[0],v.amp.control[1],v.amp.control[2]);
}
void logPcm(const PicassoAprProbe &a) {
    const auto &v=a.pcm;
    IOLog("PICASSO_ADSP pcm-map phase=%u token=%x sent=%u pending=%u responded=%u opcode=%x status=%u error=%d map_error=%d dma_error=%d\n",
        v.phase,v.token,v.sent,v.pending,v.responded,v.opcode,v.status,v.error,v.mapError,v.dmaError);
    IOLog("PICASSO_ADSP pcm-state prepared=%u handle=%x mapped=%u unmapped=%u uncertain=%u done=%u complete=%u retained=%u status=%u/%u run=0 write=0 pcm=0\n",
        v.prepared,v.handle,v.mapped,v.unmapped,v.uncertain,v.done,v.complete,v.retained,v.statuses[0],v.statuses[1]);
    IOLog("PICASSO_ADSP pcm-buffer sid=%x address=%08x%08x bytes=%u bank=%u asid=%u fsr=%x far=%08x%08x syndrome=%x fault=%x\n",
        v.sid,uint32_t(v.address>>32),uint32_t(v.address),v.bytes,v.bank,v.asid,v.fsr,v.farHigh,v.farLow,v.syndrome,v.fsr&0xc00001feU);
}
int prepare(SmemDevice *d, const PicassoSmemIo *io, PicassoSmp2p *s, PicassoGlink *glink) {
    s->audit.phase = "lock";
    if (__atomic_exchange_n(&allocatorGate, 1U, __ATOMIC_ACQ_REL)) return -16;
    PicassoSmemLockIo lock = {d, lockRead, lockWrite, smemBarrier};
    int error = -60;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        // No scheduling while APPS owns the cross-processor allocator lock.
        disable_preemption();
        int acquired = picasso_smem_trylock(&lock);
        if (!acquired) {
            d->held = true;
            error = picasso_smp2p_prepare(io, s);
            if (!error) error = picasso_glink_prepare(io, glink, &s->audit);
            int released = picasso_smem_unlock(&lock);
            d->held = false;
            enable_preemption();
            if (released) error = released;
            break;
        }
        enable_preemption();
        if (acquired != -11) { error = acquired; break; }
        IOSleep(1);
    }
    __atomic_store_n(&allocatorGate, 0U, __ATOMIC_RELEASE);
    return error;
}
struct BootEvents {
    SmemDevice *device; const PicassoSmemIo *io; PicassoSmp2p *state; PicassoGlink *glink;
    bool sentLogged, readyLogged, failedLogged;
    uint32_t channelEvents = 0, channelLogs = 0, aprAttempts = 0, aprReceived = 0;
    uint32_t intentOffers = 0, intentLogs = 0, intentFault = 0;
    int aprError = 0;
    uint32_t qrtrEvents = 0, qrtrReceived = 0, qrtrLogs = 0;
    int qrtrError = 0;
    PicassoRpcTrace rpcTrace[2]={};
    PicassoRpcIdle rpcIdle[2]={};
    uint32_t fastrpcReceived = 0, fastrpcLogs = 0, fastrpcEvents = 0;
    int fastrpcError = 0;
    uint32_t reuseLogs = 0, remoteReuse = 0, localReuse = 0;
    uint32_t qmiEvents = 0, qmiLogs = 0;
    uint32_t locatorEvents = 0, locatorLogs = 0, fwEvents = 0, afeEvents = 0;
    uint32_t tftpEvents = 0, tftpLogs = 0;
    uint32_t clockEvents = 0, portEvents = 0, pinEvents = 0, tfaEvents = 0, streamEvents = 0, pcmEvents = 0, playEvents = 0;
    PlayLogState playLog = {};
};
void logAudioPins(const PicassoAudioPins &p) {
IOLog("PICASSO_ADSP afe-pins attempted=%u active=%u restored=%u dirty=%x error=%d restore_error=%d ctl=%x/%x/%x/%x amplifier=0 pcm=0\n",
                p.attempted,p.active,p.restored,p.dirty,p.error,p.restoreError,p.observed[0],p.observed[1],p.observed[2],p.observed[3]);
}
void logAfe(const PicassoAprProbe &a) {
    const auto &v=a.afe;
    IOLog("PICASSO_ADSP afe sent=%u pending=%u responded=%u valid=%u api=%u token=%x opcode=%x status=%u error=%d port=1002 pcm=0\n",
        v.sent,v.pending,v.responded,v.valid,v.api,v.token,v.opcode,v.status,v.error);
    IOLog("PICASSO_ADSP afe-stage phase=%u set_accepted=%u verified=%u port=1002 started=0 pcm=0\n",
        v.phase,v.setAccepted,v.verified);
    if(v.phase==2 && v.responded)IOLog("PICASSO_ADSP afe-readback compatible=%u stereo_unreported=%u requested_stereo=1 started=0 pcm=0\n",
        v.readbackCompatible,v.stereoUnreported);
    if(v.payloadBytes) {
        IOLog("PICASSO_ADSP afe-wire header_bytes=%u payload_bytes=%u rejected=%u status=%u module=%x param=%x size_reserved=%x\n",
            v.headerBytes,v.payloadBytes,v.rejected,v.raw[0],v.raw[1],v.raw[2],v.raw[3]);
        IOLog("PICASSO_ADSP afe-data words=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x pcm=0\n",
            v.raw[4],v.raw[5],v.raw[6],v.raw[7],v.raw[8],v.raw[9],v.raw[10],v.raw[11]);
    }
    if(v.valid)IOLog("PICASSO_ADSP afe-config port=1002 minor=%u bits=%u channel=%u mono_stereo=%u ws=%u rate=%u format=%u pcm=0\n",
        v.config[0],v.config[1]&65535,v.config[1]>>16,v.config[2]&65535,v.config[2]>>16,v.config[3],v.config[4]&65535);
}
void logRpcRequest(const PicassoRpcDomain &d,uint32_t repeated) {
    // Serial IOLog truncates long records; correlate short lines by PD/count.
    IOLog("PICASSO_ADSP rpc-request pd=%u count=%u handle=%x sc=%x bytes=%u decode=%u result=%u repeat=%u\n",
        d.pd,d.requests,d.lastHandle,d.lastScalar,d.wireBytes,d.decodeResult,d.previousResult,repeated);
    IOLog("PICASSO_ADSP rpc-path pd=%u count=%u path=%.127s\n",d.pd,d.requests,d.name);
    if(d.fs.target[0])IOLog("PICASSO_ADSP rpc-target pd=%u count=%u target=%.127s\n",d.pd,d.requests,d.fs.target);
    IOLog("PICASSO_ADSP rpc-fs pd=%u count=%u nodes=%u/%u tombstones=%u data=%u blocks=%u/%u exhausted=%u volatile=1\n",
        d.pd,d.requests,d.fs.nodes,d.fs.nodeLimit,d.fs.tombstones,d.fs.bytes,d.fs.blocks,d.fs.blockLimit,d.fs.exhaustion);
}
void logFramework(const PicassoAprProbe &a) {
    const auto &v=a.fw;
    IOLog("PICASSO_ADSP avcs sent=%u pending=%u valid=%u token=%x status=%u error=%d build=%u.%u.%u.%u services=%u pcm=0\n",
        v.sent,v.pending,v.valid,v.token,v.status,v.error,v.build[0],v.build[1],v.build[2],v.build[3],v.count);
    if(v.valid)for(unsigned n=0;n<v.count;++n)
        IOLog("PICASSO_ADSP avcs service=%x api=%u branch=%u\n",v.services[n][0],v.services[n][1],v.services[n][2]);
}
void logFastrpc(const PicassoGlink &g) {
    bool up=false;unsigned advertised=0,active=0,done=0;
    for(const auto &c:g.channel) if(c.used && c.local==3) up=c.up;
    for(const auto &i:g.fastrpcLocal) { advertised+=i.advertised;active+=i.active;done+=i.done; }
    const auto &f=g.fastrpc;
    IOLog("PICASSO_ADSP fastrpc up=%u buffers=%u/%u/%u received=%u unclaimed=%u bytes=%u words=%08x/%08x/%08x/%08x/%08x/%08x error=%d pcm=0\n",
        up,advertised,active,done,f.received,f.unclaimed,f.bytes,f.words[0],f.words[1],f.words[2],
        f.words[3],f.words[4],f.words[5],f.error);
    IOLog("PICASSO_ADSP rpc bound=%u stopped=%u sent=%u completed=%u malformed=%u\n",
        f.bound,f.stopped,f.sent,f.completed,f.malformed);
    if(f.bound)for(const auto &d:f.domain)
        IOLog("PICASSO_ADSP rpc pd=%u stage=%u init=%u default=%u handle=%x queued=%u/%u pending=%u/%u requests=%u reverse=%x/%x result=%x name=%.127s error=%d\n",
            d.pd,d.stage,d.initialized,d.registered,d.defaultHandle,d.call[0].queued,d.call[1].queued,
            d.call[0].pending,d.call[1].pending,d.requests,d.lastHandle,d.lastScalar,d.previousResult,d.name,d.error);
}
void logTftp(const PicassoTftp &t) {
    unsigned files=0,bytes=0,active=0;
    for(const auto &f:t.files)if(f.used) { ++files;bytes+=f.size; }
    for(const auto &x:t.transfers)active+=x.used && !x.done;
    IOLog("PICASSO_ADSP tftp announced=%u requests=%u replies=%u errors=%u expired=%u retries=%u reads=%u writes=%u files=%u bytes=%u active=%u source=%u/%x destination=%x op=%u size=%u name=%.127s error=%u volatile=1 pcm=0\n",
        t.announced,t.requests,t.replies,t.errors,t.expired,t.retries,t.reads,t.writes,files,bytes,active,
        t.lastNode,t.lastPort,t.lastDestination,t.lastOpcode,t.lastBytes,t.lastName,t.lastError);
}
void logLocator(const PicassoServloc &l) {
    IOLog("PICASSO_ADSP servloc announced=%u requests=%u replies=%u malformed=%u unsupported=%u registrations=%u source=%u/%x msg=%x txn=%x name=%.64s offset=%u total=%u error=%u\n",
        l.announced,l.requests,l.replies,l.malformed,l.unsupported,l.registrations,l.lastNode,l.lastPort,
        l.lastMessage,l.lastTransaction,l.lastName,l.lastOffset,l.lastTotal,l.lastError);
}
int bootPoll(void *context, PicassoSmp2pEvent *event) {
    auto *b=static_cast<BootEvents *>(context);
    int error=picasso_smp2p_service(b->io,b->state,event);
    if(!error && event->kick)smpKick(b->device);
    return error;
}
void bootService(void *context) {
    auto *b=static_cast<BootEvents *>(context);
    // The coordinator calls here only after its own PAS AUTH and fresh ready;
    // neither a preflight sample nor the stopped observer can transmit GLINK.
    {
        bool kick=false;auto *g=b->glink;
        for(const auto &c:g->channel)if(c.used && c.local==3 && c.up && !g->fastrpc.bound && !g->fastrpc.stopped)
            picasso_rpc_device_prepare(&g->fastrpc);
        int transport=picasso_glink_service(b->io,g,&kick);
        if(kick) {
            smemBarrier(b->device);
            *reinterpret_cast<volatile uint32_t *>(b->device->ipcc+0x0c)=3U<<16;
            smemBarrier(b->device);
        }
        if(g->sent && !b->sentLogged) {
            b->sentLogged=true;
            IOLog("PICASSO_ADSP glink stage=version-sent version=1 features=0 signal=3/0\n");
        }
        if(g->ready && !b->readyLogged) {
            b->readyLogged=true;
            IOLog("PICASSO_ADSP glink stage=negotiated version=1 features=0 rx_bytes=%u channel_open=0 pcm=0\n",g->rx.size);
        }
        if(transport && transport!=-11 && !b->failedLogged) {
            b->failedLogged=true;
            IOLog("PICASSO_ADSP glink stage=stopped error=%d tx=%u rx=%u commands=%u last_command=%u channel=%u data=%x channel_open=0 pcm=0\n",
                transport,g->txHead,g->rxTail,g->commands,g->pendingCommand,g->pendingChannel,g->pendingData);
            IOLog("PICASSO_ADSP glink fault bytes=%u words=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
                g->faultBytes,g->faultWords[0],g->faultWords[1],g->faultWords[2],g->faultWords[3],
                g->faultWords[4],g->faultWords[5],g->faultWords[6],g->faultWords[7]);
        }
        if (g->channelEvents != b->channelEvents && b->channelLogs < 32) {
            b->channelEvents = g->channelEvents; ++b->channelLogs;
            for (const auto &c : g->channel) if (c.used && (c.remote || c.supported))
                IOLog("PICASSO_ADSP glink channel=%.31s local=%u remote=%u priority=%u supported=%u open_sent=%u open_ack=%u ack_sent=%u up=%u closing=%u close_ack=%u pcm=0\n",
                    c.name,c.local,c.remote,c.priority,c.supported,c.openSent,c.openAck,c.ackSent,c.up,c.closing,c.closeAck);
        }
        auto &a = g->apr;
        if (b->reuseLogs<16 && (g->remoteReuse!=b->remoteReuse || g->localReuse!=b->localReuse)) {
            IOLog("PICASSO_ADSP glink completions done=%u remote_reuse=%u local_reuse=%u last_channel=%u last_id=%x\n",
                g->remoteDone,g->remoteReuse,g->localReuse,g->lastDoneChannel,g->lastDoneId);
            b->remoteReuse=g->remoteReuse; b->localReuse=g->localReuse; ++b->reuseLogs;
        }
        if ((g->intentOffers != b->intentOffers && b->intentLogs < 16) || g->intentFault != b->intentFault) {
            unsigned cached = 0, busy = 0;
            for (const auto &i : g->remoteIntent) { cached += i.used; busy += i.used && i.busy; }
            IOLog("PICASSO_ADSP glink intents frames=%u offers=%u cached=%u busy=%u skipped=%u replaced=%u progress=%u/%u last_id=%x last_size=%u fault=%u\n",
                g->intentFrames,g->intentOffers,cached,busy,g->intentSkipped,g->intentReplaced,
                g->intentProgress,g->intentCount,g->lastIntentId,g->lastIntentSize,g->intentFault);
            ++b->intentLogs; b->intentOffers=g->intentOffers; b->intentFault=g->intentFault;
        }
        if (a.attempts != b->aprAttempts || a.received != b->aprReceived || a.error != b->aprError) {
            if (a.received <= 32 || a.error != b->aprError) {
                IOLog("PICASSO_ADSP apr query=1290c attempts=%u token=%x pending=%u received=%u opcode=%x response_token=%x state=%x status=%x ready=%u ignored=%u malformed=%u error=%d pcm=0\n",
                    a.attempts,a.token,a.pending,a.received,a.opcode,a.receivedToken,a.state,a.status,a.ready,a.ignored,a.malformed,a.error);
                IOLog("PICASSO_ADSP apr wire bytes=%u header=%x source=%x destination=%x\n",a.bytes,a.header,a.source,a.destination);
            }
            b->aprAttempts=a.attempts; b->aprReceived=a.received; b->aprError=a.error;
        }
        if(((g->fastrpc.received!=b->fastrpcReceived || g->fastrpc.events!=b->fastrpcEvents) && b->fastrpcLogs<64) || g->fastrpc.error!=b->fastrpcError) {
            logFastrpc(*g);b->fastrpcReceived=g->fastrpc.received;
            b->fastrpcEvents=g->fastrpc.events;
            b->fastrpcError=g->fastrpc.error;++b->fastrpcLogs;
        }
        for(unsigned n=0;n<2;++n) {
            const auto &d=g->fastrpc.domain[n];
            bool trace=picasso_rpc_trace(b->rpcTrace[n],d);
            bool idle=picasso_rpc_idle(b->rpcIdle[n],d.requests);
            if(trace || idle)logRpcRequest(d,b->rpcTrace[n].repeated);
            if(idle) {
                if(n==1 && d.stage==5 && !d.error && !g->fastrpc.stopped)
                    picasso_qmi_registry_idle(&g->qrtr.qmi,d.requests);
                IOLog("PICASSO_ADSP rpc-idle pd=%u count=%u ticks=100 stage=%u queued=%u/%u pending=%u/%u complete=%u/%u error=%d sensor_init=unknown\n",
                    d.pd,d.requests,d.stage,d.call[0].queued,d.call[1].queued,d.call[0].pending,d.call[1].pending,
                    d.call[0].complete,d.call[1].complete,d.error);
                IOLog("PICASSO_ADSP rpc-idle-transport pd=%u count=%u received=%u sent=%u completed=%u malformed=%u stopped=%u error=%d\n",
                    d.pd,d.requests,g->fastrpc.received,g->fastrpc.sent,g->fastrpc.completed,g->fastrpc.malformed,g->fastrpc.stopped,g->fastrpc.error);
            }
        }
        auto &q=g->qrtr;
        if(q.tftp.events!=b->tftpEvents && b->tftpLogs<48) {
            logTftp(q.tftp);b->tftpEvents=q.tftp.events;++b->tftpLogs;
        }
        if(a.fw.events!=b->fwEvents) { logFramework(a);b->fwEvents=a.fw.events; }
        if(a.afe.events!=b->afeEvents) { logAfe(a);b->afeEvents=a.afe.events; }
        if(a.clock.events!=b->clockEvents) {
            const auto &c=a.clock;b->clockEvents=c.events;
            IOLog("PICASSO_ADSP afe-clock phase=%u token=%x sent=%u pending=%u responded=%u status=%u error=%d enable_error=%d id=102 hz=1536000\n",
                c.phase,c.token,c.sent,c.pending,c.responded,c.status,c.error,c.enableError);
            IOLog("PICASSO_ADSP afe-clock-vote enable_accepted=%u disable_accepted=%u uncertain=%u complete=%u measured=0 pcm=0\n",
                c.enableAccepted,c.disableAccepted,c.uncertain,c.completed);
        }
        if(a.port.events!=b->portEvents) {
            const auto &v=a.port;b->portEvents=v.events;
            IOLog("PICASSO_ADSP afe-port phase=%u token=%x sent=%u pending=%u responded=%u status=%u error=%d start_error=%d port=1002\n",
                v.phase,v.token,v.sent,v.pending,v.responded,v.status,v.error,v.startError);
            IOLog("PICASSO_ADSP afe-port-state start_accepted=%u stop_accepted=%u uncertain=%u done=%u complete=%u pins=%u amplifier=0 pcm=0\n",
                v.startAccepted,v.stopAccepted,v.uncertain,v.done,v.completed,a.pins.active);
        }
        if(a.pins.events!=b->pinEvents) {
            const auto &p=a.pins;b->pinEvents=p.events;
            logAudioPins(p);
        }
        if(a.tfa.events!=b->tfaEvents) { b->tfaEvents=a.tfa.events;logTfa(a.tfa); }
        if(a.play.events!=b->playEvents) {
            b->playEvents=a.play.events;
            PlayLogKind kind=playLogKind(b->playLog,a);
            if(kind==PlayLogFull)logPlay(a);
            else if(kind==PlayLogProgress)logPlayProgress(a);
        }
        picasso_audio_service_tick(a,g->failed);
        if(a.pcm.events!=b->pcmEvents) { b->pcmEvents=a.pcm.events;logPcm(a); }
        if(a.stream.events!=b->streamEvents) { b->streamEvents=a.stream.events;logStream(a); }
        if(q.locator.events!=b->locatorEvents && b->locatorLogs<32) {
            logLocator(q.locator);b->locatorEvents=q.locator.events;++b->locatorLogs;
        }
        if ((b->qrtrLogs<48 && (q.events!=b->qrtrEvents || q.received!=b->qrtrReceived)) || q.error!=b->qrtrError) {
            unsigned services=0,cached=0,busy=0;
            for (const auto &v:q.services) services+=v.used;
            for (const auto &v:g->qrtrRemote) { cached+=v.used; busy+=v.used && v.busy; }
            IOLog("PICASSO_ADSP qrtr hello=%u/%u peer=%u received=%u type=%u services=%u last=%x/%x/%u/%x overflow=%u malformed=%u ignored=%u data=%u resumes=%u queued=%u intents=%u/%u error=%d qmi_query=1 pcm=0\n",
                q.helloSent,q.helloReceived,q.peer,q.received,q.lastType,services,q.lastService,q.lastInstance,q.lastNode,q.lastPort,
                q.overflow,q.malformed,q.ignored,q.data,q.resumes,q.count,cached,busy,q.error);
            if (q.received) IOLog("PICASSO_ADSP qrtr wire version=%u bytes=%u source=%u/%x destination=%u/%x payload=%u words=%08x/%08x/%08x/%08x\n",
                q.wireVersion,q.wireBytes,q.sourceNode,q.sourcePort,q.destNode,q.destPort,q.payloadBytes,
                q.payloadWords[0],q.payloadWords[1],q.payloadWords[2],q.payloadWords[3]);
            b->qrtrEvents=q.events; b->qrtrReceived=q.received; b->qrtrError=q.error; ++b->qrtrLogs;
        }
        if(q.qmi.events!=b->qmiEvents && b->qmiLogs<32) {
            b->qmiEvents=q.qmi.events;++b->qmiLogs;
            IOLog("PICASSO_ADSP qmi-phase round=%u registry_requests=%u refresh_requested=%u\n",
                q.qmi.round,q.qmi.registryRequests,q.qmi.refreshRequested);
            for(unsigned n=0;n<PicassoQmiDomains;++n) {
                const auto &d=q.qmi.domains[n];
                IOLog("PICASSO_ADSP qmi domain=%s server=%u/%x bound=%u txn=%u sent=%u pending=%u complete=%u state_valid=%u state=%x result=%u remote_error=%u error=%d\n",
                    picasso_qmi_domain(n),q.qmi.node,q.qmi.port,q.qmi.bound,picasso_qmi_transaction(&q.qmi,n),d.sent,d.pending,d.completed,d.stateValid,d.state,d.result,d.remoteError,d.error);
            }
        }
    }
}
int bootSnapshot(void *context) {
    auto *b=static_cast<BootEvents *>(context);
    picasso_adsp_snapshot s={};picasso_adsp_survey(&b->io->reader,&s);
    IOLog("PICASSO_ADSP boot snapshot crash_error=%d reason=%.255s glink_error=%d fifo=%x/%x/%x/%x\n",
        s.crash_error,s.crash,s.glink_error,s.fifo[0],s.fifo[1],s.fifo[2],s.fifo[3]);
    const auto &g = *b->glink;
    IOLog("PICASSO_ADSP glink snapshot negotiated=%u failed=%u error=%d commands=%u tx=%u rx=%u last_command=%u channel=%u data=%x deferred=%u apr_up=%u apr_ready=%u apr_error=%d intent_offers=%u intent_skipped=%u intent_id=%x intent_size=%u intent_fault=%u pcm=0\n",
        g.ready,g.failed,g.error,g.commands,g.txHead,g.rxTail,g.pendingCommand,g.pendingChannel,g.pendingData,
        g.deferredOpens,g.channel[0].up,g.apr.ready,g.apr.error,g.intentOffers,g.intentSkipped,
        g.lastIntentId,g.lastIntentSize,g.intentFault);
    IOLog("PICASSO_ADSP glink completions snapshot done=%u remote_reuse=%u local_reuse=%u last_channel=%u last_id=%x fault_bytes=%u\n",
        g.remoteDone,g.remoteReuse,g.localReuse,g.lastDoneChannel,g.lastDoneId,g.faultBytes);
    const auto &q=g.qrtr;
    logFramework(g.apr);logLocator(q.locator);logTftp(q.tftp);logFastrpc(g);
    IOLog("PICASSO_ADSP qrtr snapshot hello=%u/%u peer=%u received=%u malformed=%u ignored=%u overflow=%u data=%u resumes=%u queued=%u error=%d qmi_query=1 pcm=0\n",
        q.helloSent,q.helloReceived,q.peer,q.received,q.malformed,q.ignored,q.overflow,q.data,q.resumes,q.count,q.error);
    for (const auto &v:q.services) if (v.used)
        IOLog("PICASSO_ADSP qrtr service=%x instance=%x node=%u port=%x\n",v.service,v.instance,v.node,v.port);
    IOLog("PICASSO_ADSP qmi snapshot bound=%u lost=%u received=%u malformed=%u ignored=%u\n",
        q.qmi.bound,q.qmi.lost,q.qmi.received,q.qmi.malformed,q.qmi.ignored);
    if(q.qmi.bound || q.qmi.lost) for(unsigned n=0;n<PicassoQmiDomains;++n) {
        const auto &d=q.qmi.domains[n];
        IOLog("PICASSO_ADSP qmi snapshot domain=%s sent=%u pending=%u complete=%u state_valid=%u state=%x result=%u remote_error=%u error=%d\n",
            picasso_qmi_domain(n),d.sent,d.pending,d.completed,d.stateValid,d.state,d.result,d.remoteError,d.error);
    }
    return s.glink_error;
}
void logAudit(const PicassoSmp2p &s) {
    const auto &a = s.audit;
    IOLog("PICASSO_ADSP smem phase=%s entry=%u/%u offset=%x size=%x flags=%x hosts=%08x line=%x reserved=%x free=%x/%x siii=%x region=%x base=%08x%08x items=%u\n",
        a.phase ? a.phase : "none", a.index, a.count, a.offset, a.size, a.flags, a.hosts,
        a.cacheline, a.reserved, s.partition.uncached, s.partition.cached,
        a.infoMagic, a.infoSize, a.infoBaseHigh, a.infoBaseLow, a.items);
}
}

extern "C" void picasso_smp2p_start(void) {
    // The existing firmware worker becomes the sole SMP2P service thread.
    if (__atomic_exchange_n(&started, 1U, __ATOMIC_ACQ_REL)) return;
    // Independent, read-only command DB discovery, including SMEM failures.
    // This does not acquire the allocator lock or send DSP power votes.
    if (picasso_rpmh_adsp_plan) (void)picasso_rpmh_adsp_plan();
    else IOLog("PICASSO_ADSP rpmh stage=plan error=-6 vote_sent=0 pending=shared-owner\n");
    OSSharedPtr<IOMemoryMap> ram(smemMap(WLAN_SMEM_BASE, WLAN_SMEM_SIZE), OSNoRetain);
    OSSharedPtr<IOMemoryMap> mutex(smemMap(0x01f43000, 0x1000), OSNoRetain);
    OSSharedPtr<IOMemoryMap> ipcc(smemMap(0x00408000, 0x1000), OSNoRetain);
    SmemDevice d = {ram ? uintptr_t(ram->getVirtualAddress()) : 0,
                   mutex ? uintptr_t(mutex->getVirtualAddress()) : 0,
                   ipcc ? uintptr_t(ipcc->getVirtualAddress()) : 0, false};
    if (!d.ram || !d.mutex || !d.ipcc) {
        IOLog("PICASSO_ADSP smp2p stage=map error=-12\n"); return;
    }
    PicassoSmemIo io = {{&d, smemRead}, smemWrite, smemBarrier, lockHeld};
    PicassoSmp2p s = {};
    // The one-shot worker owns these 20x512-byte RX buffers. Keep them off the
    // kernel thread stack and retain storage until the PAS lifecycle has ended.
    static PicassoGlink glink={};
    int error = prepare(&d, &io, &s, &glink);
    IOLog("PICASSO_ADSP smp2p stage=prepare error=%d created=%u partition=%x/%x out=%x/%x owner=%x irq=0 authenticated=0\n",
        error, s.created, s.partition.offset, s.partition.size, s.out.offset, s.out.size, lockRead(&d));
    logAudit(s);
    if (error) return;
    glink.apr.pins.read=audioPinRead;glink.apr.pins.write=audioPinWrite;glink.apr.tfa.probe=tfaProbe;glink.apr.pcm.prepare=picasso_pcm_device_prepare;uint32_t audioTest=0;PE_parse_boot_argn("picasso-audio-test",&audioTest,sizeof audioTest);glink.apr.play.enabled=audioTest==1;glink.apr.play.amplifier=tfaPlayback;
    smpKick(&d);
    IOLog("PICASSO_ADSP glink stage=prepared descriptor=%x/%x tx=%x/%x remote_pid=2\n",
        glink.desc.offset,glink.desc.size,glink.tx.offset,glink.tx.size);
    picasso_rpc_fs_reset(); // worker startup, before PAS and any listener DMA
    uint32_t soc[6]={};int socError=picasso_rpc_fs_socinfo(&io.reader,soc);
    IOLog("PICASSO_ADSP rpc-socinfo error=%d format=%u id=%u revision=%x platform=%u version=%u subtype=%u files=%u\n",
        socError,soc[0],soc[1],soc[2],soc[3],soc[4],soc[5],socError?0:6);
    BootEvents boot={&d,&io,&s,&glink,false,false,false};
    PicassoAdspEvents events={&boot,bootPoll,bootSnapshot,bootService};
    picasso_adsp_boot(&events);
    picasso_glink_stop(&glink);
    picasso_audio_service_tick(glink.apr,true);
    unsigned missing = 0, logs = 0;
    bool wasOpen = false, hadValue = false;
    for (;;) {
        PicassoSmp2pEvent event = {};
        error = picasso_smp2p_service(&io, &s, &event);
        if (error && error != -2 && error != -11) {
            IOLog("PICASSO_ADSP smp2p stage=peer error=%d stopped=1 observer=1\n", error);
            logAudit(s);
            return; // Preserve the published item; no blind teardown or reuse.
        }
        if (!error) {
            if (event.kick) smpKick(&d);
            if ((!wasOpen || event.hasValue != hadValue || event.changed || event.restart) && logs < 64) {
                ++logs;
                IOLog("PICASSO_ADSP smp2p peer=1 entry=%u value=%x changed=%x features=%x restart=%u ready=%u fatal=%u handover=%u stop_ack=%u observer=1 pcm=0\n",
                    event.hasValue, event.value, event.changed, event.features, event.restart,
                    !!(event.value & 2), !!(event.value & 1), !!(event.value & 4), !!(event.value & 8));
            }
            wasOpen = true;
            hadValue = event.hasValue;
        } else if (missing == 0 || missing == 250 || missing == 1500) {
            IOLog("PICASSO_ADSP smp2p peer=0 error=%d elapsed_ms=%u observer=1 retry=0\n", error, missing * 20);
        }
        if (missing < 1501) ++missing;
        IOSleep(20);
    }
}
