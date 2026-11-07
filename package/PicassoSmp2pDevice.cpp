// SPDX-License-Identifier: BSD-2-Clause-Patent
// lito: TCSR mutex 3 @ 0x01f43000, APPS owner=1; SMEM private APPS/ADSP;
// IPCC LPASS client 3 / SMP2P signal 2. No LPASS MMIO or shared IRQ clearing.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/cpu_data.h>
#include "PicassoSmp2p.h"
#include "PicassoSmemLock.h"
#include "PicassoRpmh.h"
#include "PicassoAdspBoot.h"
#include "PicassoAdsp.h"
#include "PicassoGlink.h"

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
    uint32_t reuseLogs = 0, remoteReuse = 0, localReuse = 0;
};
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
        auto &q=g->qrtr;
        if ((b->qrtrLogs<48 && (q.events!=b->qrtrEvents || q.received!=b->qrtrReceived)) || q.error!=b->qrtrError) {
            unsigned services=0,cached=0,busy=0;
            for (const auto &v:q.services) services+=v.used;
            for (const auto &v:g->qrtrRemote) { cached+=v.used; busy+=v.used && v.busy; }
            IOLog("PICASSO_ADSP qrtr hello=%u/%u peer=%u received=%u type=%u services=%u last=%x/%x/%u/%x overflow=%u malformed=%u ignored=%u data=%u resumes=%u queued=%u intents=%u/%u error=%d qmi=0 pcm=0\n",
                q.helloSent,q.helloReceived,q.peer,q.received,q.lastType,services,q.lastService,q.lastInstance,q.lastNode,q.lastPort,
                q.overflow,q.malformed,q.ignored,q.data,q.resumes,q.count,cached,busy,q.error);
            if (q.received) IOLog("PICASSO_ADSP qrtr wire version=%u bytes=%u source=%u/%x destination=%u/%x payload=%u words=%08x/%08x/%08x/%08x\n",
                q.wireVersion,q.wireBytes,q.sourceNode,q.sourcePort,q.destNode,q.destPort,q.payloadBytes,
                q.payloadWords[0],q.payloadWords[1],q.payloadWords[2],q.payloadWords[3]);
            b->qrtrEvents=q.events; b->qrtrReceived=q.received; b->qrtrError=q.error; ++b->qrtrLogs;
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
    IOLog("PICASSO_ADSP qrtr snapshot hello=%u/%u peer=%u received=%u malformed=%u ignored=%u overflow=%u data=%u resumes=%u queued=%u error=%d qmi=0 pcm=0\n",
        q.helloSent,q.helloReceived,q.peer,q.received,q.malformed,q.ignored,q.overflow,q.data,q.resumes,q.count,q.error);
    for (const auto &v:q.services) if (v.used)
        IOLog("PICASSO_ADSP qrtr service=%x instance=%x node=%u port=%x\n",v.service,v.instance,v.node,v.port);
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
    smpKick(&d);
    IOLog("PICASSO_ADSP glink stage=prepared descriptor=%x/%x tx=%x/%x remote_pid=2\n",
        glink.desc.offset,glink.desc.size,glink.tx.offset,glink.tx.size);
    BootEvents boot={&d,&io,&s,&glink,false,false,false};
    PicassoAdspEvents events={&boot,bootPoll,bootSnapshot,bootService};
    picasso_adsp_boot(&events);
    picasso_glink_stop(&glink);
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
