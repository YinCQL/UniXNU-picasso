// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire facts: Picasso stock qcom_glink_{smem,native}.c. Independent implementation.
#include "PicassoGlink.h"
#include "PicassoApr.h"
namespace {
int fail(PicassoGlink *s, int error) {
    s->failed = true; s->error = error; s->ready = false;
    s->apr.ready = false; s->apr.pending = false;
    s->apr.fw.pending=false;s->apr.fw.valid=false;s->apr.fw.error=error;
    s->apr.afe.pending=false;s->apr.afe.valid=false;s->apr.afe.verified=false;s->apr.afe.error=error;++s->apr.afe.events;
    s->apr.afe.readbackCompatible=false;picasso_clock_stop(&s->apr,error);
    if (!s->apr.error) s->apr.error = error;
    picasso_qrtr_invalidate(&s->qrtr, error);
    picasso_rpc_stop(&s->fastrpc,error);
    for (auto &c : s->channel) c.up = false;
    return error;
}
int rd(const PicassoSmemIo *io, uint32_t at, uint32_t *v) {
    uint8_t bytes[4]; int error = wlan_smread(&io->reader, at, bytes, 4);
    if (!error) *v = wlan_sm32(bytes);
    return error;
}
int wr(const PicassoSmemIo *io, PicassoGlink *s, uint32_t at, uint32_t v) {
    int error = io->write32(io->reader.context, at, v);
    return error ? fail(s, error) : 0;
}
void fence(const PicassoSmemIo *io) { io->barrier(io->reader.context); }
int transmit(const PicassoSmemIo *io, PicassoGlink *s, const uint32_t *words, uint32_t bytes, bool *kick) {
    uint32_t tail = 0, head = 0;
    if (rd(io, s->desc.offset, &tail) || rd(io, s->desc.offset + 4, &head)) return fail(s, -5);
    if (head != s->txHead || tail >= s->tx.size || (tail & 7)) return fail(s, -22);
    uint32_t free = tail > head ? tail - head : s->tx.size - head + tail;
    if (!bytes || (bytes & 7) || bytes > s->tx.size - 16) return fail(s, -22);
    bool notify = free < bytes + 16;
    // Use the reserved READ_NOTIF slot once. The remote rings us after draining
    // our TX FIFO. The worker itself never waits for that notification.
    const uint32_t notification[2] = {13, 0};
    if (notify) {
        if (s->readNotify || free < 16) return -11;
        words = notification; bytes = 8;
    }
    fence(io); // acquire peer's released slots before reusing their payload
    int error;
    for (uint32_t i = 0; i < bytes; i += 4)
        if ((error = wr(io, s, s->tx.offset + (head + i) % s->tx.size, words[i / 4]))) return error;
    fence(io); // publish payload before APPS TX head
    uint32_t next = (head + bytes) % s->tx.size;
    if ((error = wr(io, s, s->desc.offset + 4, next))) return error;
    fence(io); s->txHead = next; *kick = true;
    s->readNotify = notify;
    return notify ? -11 : 0;
}
int send(const PicassoSmemIo *io, PicassoGlink *s, unsigned command, bool *kick) {
    const uint32_t words[2] = {command | (1U << 16), 0};
    return transmit(io, s, words, sizeof(words), kick);
}
int peek(const PicassoSmemIo *io, PicassoGlink *s, uint32_t offset, void *out, uint32_t bytes) {
    uint32_t at = (s->rxTail + offset) % s->rx.size;
    uint32_t first = bytes < s->rx.size - at ? bytes : s->rx.size - at;
    if (wlan_smread(&io->reader, s->rx.offset + at, out, first) ||
        (bytes > first && wlan_smread(&io->reader, s->rx.offset,
          static_cast<uint8_t *>(out) + first, bytes - first))) return fail(s, -5);
    return 0;
}
bool sameName(const char *a, const char *b) {
    for (unsigned i = 0; i < 32; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    return false;
}
PicassoGlinkChannel *remoteChannel(PicassoGlink *s, unsigned id) {
    for (auto &c : s->channel) if (c.used && c.remote == id && id) return &c;
    return nullptr;
}
PicassoGlinkChannel *localChannel(PicassoGlink *s, unsigned id) {
    for (auto &c : s->channel) if (c.used && c.local == id && id) return &c;
    return nullptr;
}
void resetIntent(PicassoGlinkLocalIntent &i) {
    i.id=0; i.fill=0; i.left=0; i.advertised=false; i.active=false; i.done=false; i.delivered=false;
}
PicassoGlinkLocalIntent *locals(PicassoGlink *s, const PicassoGlinkChannel &c) {
    return c.local==1 ? s->localIntent : c.local==2 ? s->qrtrLocal : s->fastrpcLocal;
}
unsigned localCount(const PicassoGlinkChannel &c) { return c.local==1 ? PicassoGlinkLocalIntents : c.local==2 ? 10 : 64; }
PicassoGlinkRemoteIntent *remotes(PicassoGlink *s, const PicassoGlinkChannel &c) {
    return c.local==1 ? s->remoteIntent : c.local==2 ? s->qrtrRemote : s->fastrpcRemote;
}
void clearChannel(PicassoGlink *s, PicassoGlinkChannel &c) {
    if (c.local==1) {
        s->apr.ready=false; s->apr.pending=false; s->apr.error=-104;
        s->apr.fw.pending=false;s->apr.fw.valid=false;s->apr.fw.error=-104;
        s->apr.afe.pending=false;s->apr.afe.valid=false;s->apr.afe.verified=false;s->apr.afe.error=-104;++s->apr.afe.events;
        s->apr.afe.readbackCompatible=false;picasso_clock_stop(&s->apr,-104);
    }
    else if(c.local==2) picasso_qrtr_invalidate(&s->qrtr,-104);
    else picasso_rpc_stop(&s->fastrpc,-104);
    for (unsigned n=0;n<localCount(c);++n) resetIntent(locals(s,c)[n]);
    for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) remotes(s,c)[n]={};
}
void updateChannel(PicassoGlink *s, PicassoGlinkChannel *c) {
    bool up = c->supported && !c->closing && c->openSent && c->openAck && c->ackSent && c->remote;
    if (up != c->up) { c->up = up; ++s->channelEvents; }
}
int channelWork(const PicassoSmemIo *io, PicassoGlink *s, PicassoGlinkChannel &c, bool *kick) {
    if (!s->ready || !c.supported) return 0;
    if (c.closing) return 0;
    int error;
    if (!c.up && ++c.steps > 500) {
        if (c.local==1) s->apr.error = -60;
        else if(c.local==2) s->qrtr.error = -60;
        else s->fastrpc.error = -60;
        if (c.openSent) {
            const uint32_t words[2] = {3U | (uint32_t(c.local) << 16), 0};
            if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        }
        c.closing = true; ++s->channelEvents; return 0;
    }
    if (!c.openSent) {
        unsigned length=1; while (c.name[length-1]) ++length;
        uint32_t words[10] = {2U | (uint32_t(c.local) << 16), length};
        for (unsigned i = 0; i < length; ++i) words[2 + i / 4] |= uint32_t(uint8_t(c.name[i])) << ((i & 3) * 8);
        if ((error = transmit(io, s, words, 8+((length+7)&~7U), kick))) return error;
        c.openSent = true; ++s->channelEvents;
    }
    if (c.remote && !c.ackSent) {
        const uint32_t words[2] = {4U | (uint32_t(c.remote) << 16), 0};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        c.ackSent = true;
    }
    updateChannel(s, &c);
    if (!c.up) return 0;
    auto &a = s->apr;
    int qmiDue=c.local==2 ? picasso_qmi_due(&s->qrtr.qmi):-1;
    if (c.local==1) { ++a.ticks;picasso_afe_advance(&a,s->qrtr.qmi);picasso_clock_tick(&a,s->qrtr.qmi);picasso_stream_tick(&a,s->qrtr.qmi);picasso_pcm_tick(&a,s->qrtr.qmi); }
    // Worker cadence is 20 ms. Hold expiry while responses are backpressured.
    if(c.local==2 && !s->qrtr.count && s->qrtr.tftp.announced) {
        auto &q=s->qrtr;int retry=picasso_tftp_tick(&q.tftp);
        if(retry>=0) {
            auto &x=q.tftp.transfers[retry];
            picasso_qrtr_reply(&q,x.port,x.node,x.peer,x.reply,x.replyBytes);
        }
    }
    if(c.local==1 && a.fw.pending && ++a.fw.wait>100) {
        a.fw.pending=false;a.fw.error=-60;++a.fw.events;
    }
    if(c.local==1 && (a.afe.pending || picasso_afe_due(&a)) && ++a.afe.wait>(a.afe.pending?100U:250U)) {
        a.afe.pending=false;a.afe.error=-60;++a.afe.events;
    }
    // Count service calls even under TX backpressure. A timeout does not free
    // any remote intent; its ownership lasts until RX_DONE or disconnect.
    if (c.local==1 && !a.ready && !a.error) {
        if (a.pending && ++a.wait > 50) { a.pending = false; a.next = a.ticks + 5; }
        if (a.ticks > 250 || (!a.pending && a.attempts >= 3 && a.ticks >= a.next)) {
            a.pending = false; a.error = -60;
        }
    }
    // Stock lito uses reusable base intents without a negotiated feature bit.
    // Keep each real buffer unavailable until RX_DONE_W_REUSE is published.
    auto *local=locals(s,c); auto *remote=remotes(s,c);
    for (unsigned n=0;n<localCount(c);++n) {
        auto &i=local[n];
        if (!i.done) continue;
        if (!i.delivered) {
            if (c.local==2 && picasso_qrtr_receive(&s->qrtr,i.data,i.fill)==-11) break;
            i.delivered=true;
        }
        const uint32_t words[2] = {14U | (uint32_t(c.local) << 16), i.id};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        uint32_t id=i.id; resetIntent(i); i.id=id; i.advertised=true;
        ++s->localReuse; break;
    }
    for (unsigned n=0;n<localCount(c);++n) {
        auto &i=local[n]; if (i.id) continue;
        if (!s->nextIntent) return fail(s, -75);
        const uint32_t words[4] = {5U | (uint32_t(c.local) << 16), 1, i.capacity, s->nextIntent};
        if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
        i.id = s->nextIntent++; i.advertised = true; break;
    }
    if (c.local==2) {
        auto &q=s->qrtr;
        bool announce=q.helloSent && q.helloReceived && !q.locator.announced && !q.count;
        bool tftpAnnounce=q.helloSent && q.helloReceived && q.locator.announced && !q.tftp.announced && !q.count;
        if (q.error || (q.helloSent && !q.count && qmiDue<0 && !announce && !tftpAnnounce)) return 0;
        uint32_t words[4+((PicassoQrtrWords+1)&~1U)]={9U | (uint32_t(c.local)<<16),0,52,0};
        bool query=q.helloSent && !q.count && !announce && !tftpAnnounce && qmiDue>=0;
        if (!q.helloSent) picasso_qrtr_control(words+4,2,1,PicassoQrtrControl,
            q.helloReceived?q.peer:0xffffffffU,PicassoQrtrControl);
        else if(announce) {
            picasso_qrtr_control(words+4,4,1,PicassoQrtrControl,q.peer,PicassoQrtrControl,0x40,0x101);
            words[15]=1;words[16]=PicassoServlocPort;
        }
        else if(tftpAnnounce) {
            picasso_qrtr_control(words+4,4,1,PicassoQrtrControl,q.peer,PicassoQrtrControl,0x1000,2); // stock sensor TFTP address (b21365c8)
            words[15]=1;words[16]=PicassoTftpPort;
        }
        else if(query) words[2]=picasso_qmi_query(&q.qmi,unsigned(qmiDue),reinterpret_cast<uint8_t *>(words+4));
        else {
            words[2]=q.queueBytes[q.head];
            for(unsigned n=0;n<words[2]/4;++n)words[4+n]=q.queue[q.head][n];
        }
        PicassoGlinkRemoteIntent *slot=nullptr;
        for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n)
            if (remote[n].used && !remote[n].busy && remote[n].size>=words[2]) { slot=&remote[n]; break; }
        if (!slot) {
            if (!q.intentAsked && !q.intentGranted) {
                const uint32_t request[2]={7U | (uint32_t(c.local)<<16),words[2]};
                if ((error=transmit(io,s,request,sizeof(request),kick))) return error;
                q.intentAsked=true;
            }
            return 0;
        }
        words[1]=slot->id;
        if ((error=transmit(io,s,words,16+((words[2]+7)&~7U),kick))) return error;
        slot->busy=true; q.intentGranted=false;
        if (!q.helloSent) q.helloSent=true;
        else if(announce) { q.locator.announced=true;++q.locator.events; }
        else if(tftpAnnounce) { q.tftp.announced=true;++q.tftp.events; }
        else if(query) {
            auto &d=q.qmi.domains[qmiDue];d.sent=true;d.pending=true;d.ticks=0;++q.qmi.events;
        }
        else {
            if(q.queue[q.head][1]==7)++q.resumes;
            else if(q.queue[q.head][3]==PicassoServlocPort) { ++q.locator.replies;++q.locator.events; }
            else { ++q.tftp.replies;++q.tftp.events; }
            q.head=(q.head+1)%PicassoQrtrQueue; --q.count;
        }
        ++q.events; return 0;
    }
    if(c.local==3) {
        auto &f=s->fastrpc;picasso_rpc_tick(&f);
        uint32_t words[14]={9U|(uint32_t(c.local)<<16),0,40,0};
        int call=picasso_rpc_packet(&f,words+4);if(call<0)return 0;
        PicassoGlinkRemoteIntent *slot=nullptr;
        for(unsigned n=0;n<PicassoGlinkRemoteIntents;++n)
            if(remote[n].used && !remote[n].busy && remote[n].size>=40) { slot=&remote[n];break; }
        if(!slot) {
            if(!f.intentAsked && !f.intentGranted) {
                const uint32_t request[2]={7U|(uint32_t(c.local)<<16),40};
                if((error=transmit(io,s,request,sizeof(request),kick)))return error;
                f.intentAsked=true;
            }return 0;
        }
        words[1]=slot->id;
        if((error=transmit(io,s,words,sizeof(words),kick)))return error;
        slot->busy=true;f.intentGranted=false;picasso_rpc_sent(&f,unsigned(call));return 0;
    }
    // Stock q6core queries FWK_VERSION only after real CORE readiness. This
    // discovers AFE/ASM/ADM APIs without starting audio or assuming versions.
    bool version=a.ready && !a.fw.sent && !a.fw.error;
    bool afe=picasso_afe_due(&a);
    bool clock=picasso_clock_due(a);
    bool port=picasso_port_due(a);
    bool stream=picasso_stream_due(a);
    bool pcm=picasso_pcm_due(a);
    if ((a.ready && !version && !afe && !clock && !port && !stream && !pcm) || a.error) return 0;
    if (!version && !afe && !clock && !port && !stream && !pcm && (a.pending || a.ticks < a.next)) return 0;
    unsigned requestBytes=pcm?(a.pcm.phase?24:40):stream?(a.stream.phase==0?36:a.stream.phase==1?44:20):port?24:clock?68:afe?(a.afe.phase==1?68:56):20;
    PicassoGlinkRemoteIntent *slot = nullptr;
    for (auto &i : s->remoteIntent) if (i.used && !i.busy && i.size >= requestBytes) { slot = &i; break; }
    if (!slot) {
        if (!a.intentAsked && !a.intentGranted) {
            const uint32_t words[2] = {7U | (uint32_t(c.local) << 16), requestBytes};
            if ((error = transmit(io, s, words, sizeof(words), kick))) return error;
            a.intentAsked = true;
        }
        return 0;
    }
    uint32_t words[22] = {9U | (uint32_t(c.local) << 16), slot->id, requestBytes, 0};
    uint32_t query[5]; picasso_apr_query(version?0x18710001:a.token + 1, query);
    if(afe || clock || port || stream || pcm) { uint32_t request[18];if(pcm)picasso_pcm_request(a.pcm,request);else if(stream)picasso_stream_request(a.stream.phase,request);else if(port)picasso_port_request(a.port.phase,request);else if(clock)picasso_clock_request(a.clock.phase,request);else picasso_afe_request(a.afe.phase,request);for(unsigned i=0;i<requestBytes/4;++i)words[4+i]=request[i]; }
    else { if(version)query[4]=0x1292c;for (unsigned i = 0; i < 5; ++i) words[4 + i] = query[i]; }
    if ((error = transmit(io, s, words,16+((requestBytes+7)&~7U), kick))) {
        // I/O failure may leave publication ambiguous; lack of an ACK never
        // establishes that an enable vote was not applied. TX pressure did not
        // publish this request and must not acquire a vote or remote intent.
        if(clock && !a.clock.phase && s->failed)a.clock.uncertain=true;
        if(port && !a.port.phase && s->failed)a.port.uncertain=true;
        if(stream && !a.stream.phase && s->failed)a.stream.uncertain=true;
        if(pcm && !a.pcm.phase && s->failed)a.pcm.uncertain=true;
        return error;
    }
    slot->busy = true;
    if(pcm) { a.pcm.sent=a.pcm.pending=true;a.pcm.token=words[7];a.pcm.wait=0;if(!a.pcm.phase)a.pcm.uncertain=true;++a.pcm.events; }
    else if(stream) { a.stream.sent=a.stream.pending=true;a.stream.token=words[7];a.stream.wait=0;if(!a.stream.phase)a.stream.uncertain=true;++a.stream.events; }
    else if(port) { a.port.sent=a.port.pending=true;a.port.token=words[7];a.port.wait=0;if(!a.port.phase)a.port.uncertain=true;++a.port.events; }
    else if(clock) { a.clock.sent=a.clock.pending=true;a.clock.token=words[7];a.clock.wait=0;if(!a.clock.phase)a.clock.uncertain=true;++a.clock.events; }
    else if(afe) { a.afe.sent=a.afe.pending=true;a.afe.token=words[7];a.afe.wait=0;++a.afe.events; }
    else if(version) {
        a.fw.sent=a.fw.pending=true;a.fw.token=query[3];a.fw.wait=0;++a.fw.events;
    } else { ++a.token; ++a.attempts; a.wait = 0; a.pending = true; }
    a.intentGranted = false;
    return 0;
}
int receivePipe(const PicassoSmemIo *io, PicassoGlink *s) {
    struct wlan_smem_partition p = {}; wlan_smem_item desc = {}, tx = {}, rx = {};
    int error = wlan_smem_partition(&io->reader, 0, 2, &p);
    if (error) return error;
    if ((error = wlan_smem_find(&io->reader, &p, 478, &desc)) ||
        (error = wlan_smem_find(&io->reader, &p, 479, &tx))) return error;
    if (desc.offset != s->desc.offset || desc.size != s->desc.size ||
        tx.offset != s->tx.offset || tx.size != s->tx.size) return -22;
    if ((error = wlan_smem_find(&io->reader, &p, 480, &rx))) return error;
    // The remote owns FIFO_1 and declares its length (Linux does not fix it to
    // APPS' 16 KiB). Reject impossible or changing pipe geometry.
    if (rx.size < 16 || (rx.size & 7) || rx.size > 65536 ||
        (s->rx.size && (s->rx.offset != rx.offset || s->rx.size != rx.size))) return -22;
    s->rx = rx; return 0;
}
// A complete command is committed only after all required responses have room.
// Unsupported channel names are retained without OPEN_ACK or receive buffers;
// they do not prevent unrelated services sharing this FIFO from making progress.
int receive(const PicassoSmemIo *io, PicassoGlink *s, unsigned command, unsigned id,
            uint32_t data, uint32_t avail, uint32_t *length, unsigned *intentBudget, bool *kick) {
    if (!s->ready) return fail(s, -71);
    auto *c = remoteChannel(s, id);
    int error;
    if (command == 2) {
        unsigned nameBytes = data & 0xffff;
        if (!id || nameBytes < 2 || nameBytes > 32) return fail(s, -22);
        *length = 8 + ((nameBytes + 7) & ~7U);
        if (*length > s->rx.size - 8) return fail(s, -90);
        if (avail < *length) return -11;
        char name[32] = {};
        if ((error = peek(io, s, 8, name, nameBytes))) return error;
        if (name[nameBytes - 1]) return fail(s, -22);
        for (unsigned i = 0; i < nameBytes - 1; ++i)
            if (uint8_t(name[i]) < 0x21 || uint8_t(name[i]) > 0x7e) return fail(s, -22);
        if (c) return fail(s, -17); // an rcid cannot change owners mid-session
        for (auto &candidate : s->channel)
            if (candidate.used && sameName(candidate.name, name)) { c = &candidate; break; }
        if (c && (c->remote || c->closing)) return fail(s, -17);
        if (!c) {
            for (auto &candidate : s->channel) if (!candidate.used) { c = &candidate; break; }
            if (!c) return fail(s, -28);
            *c = {}; c->used = true;
            for (unsigned i = 0; i < nameBytes; ++i) c->name[i] = name[i];
            if (sameName(name,"IPCRTR")) { c->supported=true; c->local=2; }
            else if(sameName(name,"fastrpcglink-apps-dsp")) { c->supported=true; c->local=3; }
            else ++s->deferredOpens;
        }
        c->remote = uint16_t(id); c->priority = uint16_t(data >> 16); ++s->channelEvents;
        return 0;
    }
    if (command == 4) {
        c = localChannel(s, id);
        if (!c || !c->openSent || c->openAck || c->closing || data) return fail(s, -71);
        c->openAck = true; updateChannel(s, c); ++s->channelEvents; return 0;
    }
    if (command == 3) {
        if (!c || data) return fail(s, -71);
        uint32_t words[4] = {11U | (id << 16), 0, 3U | (uint32_t(c->local) << 16), 0};
        if ((error = transmit(io, s, words, c->openSent && !c->closing ? 16 : 8, kick))) return error;
        if (c->supported) { c->closing = true; c->remote = 0; c->up = false; clearChannel(s,*c); }
        else *c = {};
        ++s->channelEvents; return 0;
    }
    if (command == 11) {
        c = localChannel(s, id);
        if (!c || !c->closing || c->closeAck || data) return fail(s, -71);
        c->closeAck = true; ++s->channelEvents; return 0;
    }
    if (!c) return fail(s, -71);
    if (command == 15) { c->signals = data; return 0; }
    if (command == 5) {
        if (!c->supported || !c->ackSent || c->closing) {
            s->intentFault = 1;
            return fail(s, -22);
        }
        if (s->intentProgress && (s->intentCount != data || s->intentTail != s->rxTail || s->intentChannel != id)) {
            s->intentFault = 3; return fail(s, -71);
        }
        // Wire count is bounded by the FIFO, not our cache capacity. The DSP
        // may advertise more buffers than this bounded bring-up client needs.
        if (data > (s->rx.size - 16) / 8) { s->intentFault = 2; return fail(s, -90); }
        *length = 8 + data * 8;
        if (avail < *length) return -11;
        s->intentCount = data; s->intentTail = s->rxTail; s->intentChannel = id;
        // At most sixteen descriptors per service call. Keep RX tail until the
        // full command is parsed; the peer cannot reuse its payload meanwhile.
        while (s->intentProgress < data && *intentBudget) {
            uint8_t pair[8];
            if ((error = peek(io, s, 8 + s->intentProgress * 8, pair, sizeof(pair)))) return error;
            uint32_t size = wlan_sm32(pair), iid = wlan_sm32(pair + 4);
            s->lastIntentId = iid; s->lastIntentSize = size;
            // Local IDs start at one by policy. Remote IDs are opaque u32s:
            // stock idr_alloc(riids, ..., iid, iid+1) explicitly accepts zero.
            if (!size) { s->intentFault = 4; return fail(s, -22); }
            PicassoGlinkRemoteIntent *slot = nullptr;
            for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
                auto &i=remotes(s,*c)[n];
                if (i.used && i.id == iid) { s->intentFault = 5; return fail(s, -17); }
                if (!i.used && !slot) slot = &i;
            }
            if (!slot) {
                for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
                    auto &i=remotes(s,*c)[n];
                    if (!i.busy && i.size < size && (!slot || i.size < slot->size)) slot = &i;
                }
                if (slot) ++s->intentReplaced;
            }
            // Idle descriptors are remote-owned storage, not allocations held
            // by APPS. We may leave them unused. Never evict an in-flight ID or
            // send RX_DONE for an ignored advertisement. Keep a larger grant.
            if (slot) { slot->id = iid; slot->size = size; slot->used = true; }
            else ++s->intentSkipped;
            ++s->intentOffers; ++s->intentProgress; --*intentBudget;
        }
        if (s->intentProgress < data) return -11;
        s->intentProgress = 0; ++s->intentFrames;
        return 0;
    }
    if (command == 7) {
        if (!c->up || !data) return fail(s, -71);
        PicassoGlinkLocalIntent *slot = nullptr;
        // A reusable advertised buffer already belongs to this endpoint. A
        // request may refer to that pool; do not require another free slot or
        // re-advertise its ID. Busy/undelivered buffers cannot grant capacity.
        for (unsigned n=0;n<localCount(*c);++n) {
            const auto &i=locals(s,*c)[n];
            if (i.advertised && !i.active && !i.done && i.capacity>=data) {
                const uint32_t ack[2]={8U | (uint32_t(c->local)<<16),1};
                return transmit(io,s,ack,sizeof(ack),kick);
            }
        }
        for (unsigned n=0;n<localCount(*c);++n) {
            auto &i=locals(s,*c)[n]; if (!i.id && i.capacity>=data) { slot=&i; break; }
        }
        if (slot && !s->nextIntent) return fail(s, -75);
        uint32_t words[6] = {5U | (uint32_t(c->local) << 16), 1, slot ? slot->capacity : 0, s->nextIntent,
                            8U | (uint32_t(c->local) << 16), slot ? 1U : 0U};
        if ((error = transmit(io, s, slot ? words : words + 4, slot ? 24 : 8, kick))) return error;
        if (slot) { slot->id = s->nextIntent++; slot->advertised = true; }
        return 0;
    }
    if (command == 8) {
        auto &asked=c->local==1 ? s->apr.intentAsked : c->local==2 ? s->qrtr.intentAsked : s->fastrpc.intentAsked;
        auto &granted=c->local==1 ? s->apr.intentGranted : c->local==2 ? s->qrtr.intentGranted : s->fastrpc.intentGranted;
        if (!c->up || !asked || data > 1) return fail(s, -71);
        granted=data!=0 && (c->local!=1 || !s->apr.pending); asked=false;
        if (!data) {
            if (c->local==1 && !s->apr.pending) {
                auto &a=s->apr;
                if(a.ready && a.fw.valid) {
                    if(picasso_pcm_due(a)) { a.pcm.error=-28;++a.pcm.events; }
                    else if(picasso_stream_due(a)) { a.stream.error=-28;++a.stream.events; }
                    else if(picasso_port_due(a)) { a.port.error=-28;++a.port.events; }
                    else if(picasso_clock_due(a)) { a.clock.error=-28;++a.clock.events; }
                    else if(!a.afe.sent && !a.afe.error) { a.afe.error=-28;++a.afe.events; }
                } else if(a.ready) {
                    if(!a.fw.sent && !a.fw.error) { a.fw.error=-28;++a.fw.events; }
                } else a.error=-28;
            }
            if (c->local==2) s->qrtr.error=-28;
        }
        return 0;
    }
    if (command == 6 || command == 14) {
        if (!c->up) return fail(s, -71);
        for (unsigned n=0;n<PicassoGlinkRemoteIntents;++n) {
            auto &i=remotes(s,*c)[n];
            if (i.used && i.id == data && i.busy) {
                // RX_DONE retires the ID; RX_DONE_W_REUSE releases only its
                // in-flight ownership. Stock v1 handles both with features=0.
                if (command==14) { i.busy=false; ++s->remoteReuse; }
                else { i={}; ++s->remoteDone; }
                s->lastDoneId=data; s->lastDoneChannel=id; return 0;
            }
        }
        return fail(s, -71);
    }
    if (command == 9 || command == 12) {
        if (!c->up) return fail(s, -71);
        if (avail < 16) return -11;
        uint32_t sizes[2];
        if ((error = peek(io, s, 8, sizes, 8))) return error;
        uint32_t chunk = sizes[0], left = sizes[1];
        if (!chunk || chunk>0x4400 || left>0x4400-chunk) return fail(s,-90);
        *length = 16 + ((chunk + 7) & ~7U);
        if (*length > s->rx.size - 8) return fail(s, -90);
        if (avail < *length) return -11;
        PicassoGlinkLocalIntent *slot = nullptr;
        for (unsigned n=0;n<localCount(*c);++n) {
            auto &i=locals(s,*c)[n]; if (i.id==data && data) { slot=&i; break; }
        }
        if (!slot || !slot->advertised || slot->done ||
            (command == 9 ? slot->active : !slot->active)) return fail(s, -71);
        if (chunk>slot->capacity || left>slot->capacity-chunk ||
            slot->fill > slot->capacity - chunk - left ||
            (command == 12 && slot->left != chunk + left)) return fail(s, -90);
        if ((error = peek(io, s, 16, slot->data + slot->fill, chunk))) return error;
        slot->fill += chunk; slot->left = left; slot->active = left != 0;
        if (!left) {
            if (c->local==1) { picasso_apr_receive(&s->apr,slot->data,slot->fill); slot->delivered=true; }
            else if(c->local==2) slot->delivered=picasso_qrtr_receive(&s->qrtr,slot->data,slot->fill)!=-11;
            else {
                picasso_rpc_receive(&s->fastrpc,slot->data,slot->fill);
                slot->delivered=true;
            }
            slot->done = true; slot->advertised = false; ++s->dataPackets;
        }
        return 0;
    }
    // Unknown commands cannot be skipped without knowing their framing.
    return fail(s, -95);
}
}
int picasso_glink_prepare(const PicassoSmemIo *io, PicassoGlink *s, PicassoSmemAudit *audit) {
    if (!s) return -22;
    // Aggregate assignment creates a large temporary at -O0, overflowing a
    // small kernel stack despite the caller using static storage. Zero in place.
    auto *bytes = reinterpret_cast<uint8_t *>(s);
    for (unsigned i = 0; i < sizeof(*s); ++i) bytes[i] = 0;
    int error = picasso_smem_prepare_glink(io, &s->desc, &s->tx, audit);
    if (error) return fail(s, error);
    auto &c = s->channel[0]; c.used = true; c.supported = true; c.local = 1;
    const char name[] = "apr_audio_svc";
    for (unsigned i = 0; i < sizeof(name); ++i) c.name[i] = name[i];
    for (unsigned n=0;n<PicassoGlinkLocalIntents;++n) {
        s->localIntent[n].data=s->aprStorage[n]; s->localIntent[n].capacity=PicassoGlinkPacket;
    }
    unsigned offset=0;
    for (unsigned n=0;n<10;++n) {
        auto &i=s->qrtrLocal[n]; i.capacity=n<5 ? 0x800 : n<8 ? 0x2000 : 0x4400;
        i.data=s->qrtrStorage+offset; offset+=i.capacity;
    }
    for(unsigned n=0;n<64;++n) {
        s->fastrpcLocal[n].data=s->fastrpcStorage[n];s->fastrpcLocal[n].capacity=100;
    }
    s->nextIntent = 1; s->apr.token = 0x18300000;
    s->prepared = true; return 0;
}
int picasso_glink_service(const PicassoSmemIo *io, PicassoGlink *s, bool *kick) {
    if (!io || !io->reader.read || !io->write32 || !io->barrier || !s || !kick) return -22;
    *kick = false;
    if (!s->prepared || s->failed) return s->error ? s->error : -22;
    if (!s->ready && ++s->steps > 500) return fail(s, -60);
    int error = 0;
    if (!s->sent) {
        if ((error = send(io, s, 0, kick))) return error;
        s->sent = true; // stock version 1, features 0; explicit intents required
    }
    error = receivePipe(io, s);
    if (error == -2 || error == -11) return 0;
    if (error) return fail(s, error);
    // Only work already due at entry is sent here. Newly received OPENs are
    // acknowledged on the next service tick, after the command is committed.
    // Descriptors from a partially parsed command are not eligible for TX.
    if (!s->intentProgress) for (auto &c:s->channel) if (c.used && c.supported) {
        error=channelWork(io,s,c,kick); if (s->failed) return error;
    }
    unsigned intentBudget = 16;
    for (unsigned budget = 0; budget < 8; ++budget) {
        uint32_t head = 0, tail = 0, words[2] = {};
        if (rd(io, s->desc.offset + 12, &head) || rd(io, s->desc.offset + 8, &tail)) return fail(s, -5);
        if (head >= s->rx.size || (head & 7) || tail != s->rxTail) return fail(s, -22);
        if (head == tail) break;
        fence(io); // acquire peer's payload after its atomic RX head
        if (rd(io, s->rx.offset + tail, words) || rd(io, s->rx.offset + tail + 4, words + 1)) return fail(s, -5);
        unsigned command = words[0] & 0xffff, version = words[0] >> 16;
        uint32_t length = 8, avail = (head + s->rx.size - tail) % s->rx.size;
        if (s->intentProgress && command != 5) { s->intentFault = 3; return fail(s, -71); }
        if (command == 0 || command == 1) {
            if (!version) return fail(s, -93);
            if (s->retries >= 16) return fail(s, -71);
            if (command == 0) {
                if ((error = send(io, s, 1, kick))) return error;
                if (version == 1) s->peerVersion = true;
            } else if (version == 1 && words[1] == 0) s->peerAck = true;
            else if ((error = send(io, s, 0, kick))) return error;
            ++s->retries;
        } else if (command == 13) {
            *kick = true; // READ_NOTIF: notify peer after advancing our RX tail
        } else {
            s->pendingCommand = uint16_t(command);
            s->pendingChannel = uint16_t(version); s->pendingData = words[1];
            error = receive(io, s, command, version, words[1], avail, &length, &intentBudget, kick);
            if (error == -11) return 0; // incomplete frame or TX pressure; retry without consuming
            if (error) {
                // Read only the currently owned FIFO span, without turning a
                // failed diagnostic read into another protocol operation.
                s->faultBytes=0;
                for (unsigned at=0;at<32 && at+4<=avail;at+=4) {
                    uint8_t raw[4];
                    if (wlan_smread(&io->reader,s->rx.offset+(tail+at)%s->rx.size,raw,4)) break;
                    s->faultWords[at/4]=wlan_sm32(raw); s->faultBytes+=4;
                }
                return error;
            }
        }
        uint32_t next = (tail + length) % s->rx.size;
        fence(io);
        if ((error = wr(io, s, s->desc.offset + 8, next))) return error;
        fence(io); s->rxTail = next; ++s->commands;
        s->ready = s->peerVersion && s->peerAck;
        if (!intentBudget) break;
    }
    return 0;
}
void picasso_glink_stop(PicassoGlink *s) {
    if (!s) return;
    fail(s, -104);
    for (auto &c:s->channel) if (c.used && c.supported) clearChannel(s,c);
}
