// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent bounded SMQ/listener implementation. Wire: stock adsprpc_shared.h,
// quic/fastrpc adsp_listener.idl, remotectl.idl and listener_buf.h.
#ifndef PICASSO_FASTRPC_H
#define PICASSO_FASTRPC_H
#include <stdint.h>
#include "PicassoRpcFs.h"
enum { PicassoRpcSlotBytes=16384, PicassoRpcDomainBytes=32768, PicassoRpcBlob=6144 };
struct PicassoRpcIo {
    void *context;
    int (*sync)(void *,unsigned,unsigned,bool); // whole owned slot: publish / acquire
    int (*healthy)(void *);
};
struct PicassoRpcCall {
    uint64_t ctx;
    uint32_t handle,sc,offset[4],length[4],ticks,retval;
    bool queued,pending,complete,early;
};
struct PicassoRpcDomain {
    uint8_t *memory;
    uint64_t address;
    uint32_t pd,pid,stage,defaultHandle,sequence,requests,lastHandle,lastScalar;
    uint32_t previousCtx,previousResult,replyBytes,opens,decodeResult,wireBytes;
    int error;
    bool registered,initialized,stdOpen;
    char name[128];
    uint8_t reply[PicassoRpcBlob];
    PicassoRpcCall call[2];
};
struct PicassoFastrpc {
    uint32_t received,bytes,words[6],unclaimed,malformed,completed,sent,events;
    int error;
    bool bound,intentAsked,intentGranted,stopped;
    PicassoRpcIo io;
    PicassoRpcDomain domain[2];
};
static inline uint32_t rpc32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static inline uint64_t rpc64(const uint8_t *p) { return rpc32(p)|(uint64_t(rpc32(p+4))<<32); }
static inline void rpcPut32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
static inline void rpcPut64(uint8_t *p,uint64_t v) { rpcPut32(p,uint32_t(v));rpcPut32(p+4,uint32_t(v>>32)); }
static inline void rpcZero(uint8_t *p,unsigned n) { for(unsigned i=0;i<n;++i)p[i]=0; }
static inline bool rpcName(const char *a,const char *b) {
    for(unsigned i=0;i<128;++i) { if(a[i]!=b[i])return false;if(!a[i])return true; }return false;
}
static inline void picasso_rpc_stop(PicassoFastrpc *f,int error) {
    f->error=error;f->stopped=true;++f->events;
    for(auto &d:f->domain) { d.error=error;d.registered=d.initialized=d.stdOpen=false;
        for(auto &c:d.call)c.queued=c.pending=c.complete=false; }
    // No memory recycling: remote ownership may outlive transport or timeout.
}
static inline int picasso_rpc_bind(PicassoFastrpc *f,const PicassoRpcIo &io,uint8_t *root,uint8_t *sensor) {
    if(!f || f->bound || f->stopped || !io.sync || !io.healthy || !root || !sensor ||
        (uintptr_t(root)&4095) || (uintptr_t(sensor)&4095) ||
        (uintptr_t(root)<uintptr_t(sensor)+PicassoRpcDomainBytes && uintptr_t(sensor)<uintptr_t(root)+PicassoRpcDomainBytes))return -22;
    f->io=io;f->bound=true;
    for(unsigned n=0;n<2;++n) { auto &d=f->domain[n];d.memory=n?sensor:root;
        d.address=(uint64_t(3+n)<<32)|0x80000000U;d.pd=n?2:0;d.pid=0x188+n;
        d.previousResult=0xffffffffU; }
    ++f->events;return 0;
}
// At most four copied buffers. CPU pointers are never put on the wire: the
// descriptor pointer contributes only the within-page offset to DSP mapping.
static inline int rpcBuild(PicassoFastrpc *f,unsigned domain,unsigned slot,uint32_t handle,uint32_t sc,
    const uint8_t *const *inputs,const uint32_t *lengths) {
    auto &d=f->domain[domain];auto &c=d.call[slot];unsigned in=(sc>>16)&255,out=(sc>>8)&255,n=in+out;
    if(c.pending || c.queued || c.complete || n>4 || (sc&255) || (n && !lengths) || d.sequence>=0xffffff)return -22;
    unsigned cursor=(n*40+388+127)&~127U;
    for(unsigned i=0;i<n;++i) { if(lengths[i]>PicassoRpcBlob || (i<in && lengths[i] && (!inputs || !inputs[i])))return -22;
        if(lengths[i])cursor=((cursor+127)&~127U)+lengths[i]; }
    if(cursor>PicassoRpcSlotBytes)return -90;
    c={};c.ctx=(uint64_t(++d.sequence)<<12)|(uint64_t(domain)<<8)|(uint64_t(slot)<<4)|d.pd;c.handle=handle;c.sc=sc;
    auto *p=d.memory+slot*PicassoRpcSlotBytes;rpcZero(p,PicassoRpcSlotBytes);cursor=(n*40+388+127)&~127U;
    for(unsigned i=0;i<n;++i) {
        c.length[i]=lengths[i];rpcPut64(p+i*16+8,lengths[i]);
        rpcPut32(p+n*16+i*8,lengths[i]?1:0);rpcPut32(p+n*16+i*8+4,i);
        if(!lengths[i])continue;
        cursor=(cursor+127)&~127U;c.offset[i]=cursor;
        uint64_t address=d.address+slot*PicassoRpcSlotBytes+cursor;
        rpcPut64(p+i*16,0x10000000U+cursor);
        rpcPut64(p+n*24+i*16,address&~uint64_t(4095));
        rpcPut64(p+n*24+i*16+8,((cursor&4095)+lengths[i]+4095)&~4095U);
        if(i<in)for(unsigned j=0;j<lengths[i];++j)p[cursor+j]=inputs[i][j];
        cursor+=lengths[i];
    }
    int error=f->io.sync(f->io.context,domain,slot,true);if(error)return error;
    c.queued=true;++f->events;return 0;
}
static inline void picasso_rpc_receive(PicassoFastrpc *f,const uint8_t *p,unsigned bytes) {
    ++f->received;f->bytes=bytes;for(auto &w:f->words)w=0;
    for(unsigned n=0;n<bytes && n<24;++n)f->words[n/4]|=uint32_t(p[n])<<((n&3)*8);
    if(bytes!=12 && bytes!=16 && bytes!=24) { ++f->malformed;return; }
    unsigned flags=bytes==24?rpc32(p+12):0;
    if(bytes==24 && (rpc32(p+20)!=2 || flags>3)) { ++f->malformed;return; }
    uint64_t ctx=rpc64(p);
    for(auto &d:f->domain)for(auto &c:d.call)if(c.pending && (c.ctx&~3ULL)==(ctx&~3ULL) &&
        (!(ctx&3) || (ctx&3)==d.pd) && !f->stopped) {
        if(flags==2)return; // advisory early wakeup is not completion
        if(flags==1) { c.retval=rpc32(p+8);c.early=true;return; }
        if(flags==3 && !c.early) { ++f->malformed;return; }
        if(!flags)c.retval=rpc32(p+8);
        c.pending=false;c.complete=true;++f->completed;++f->events;return;
    }
    ++f->unclaimed;
}
struct PicassoRpcArg { uint8_t *data;uint32_t bytes; };
// next2 packs input length/data (8-byte alignment), then output lengths.
// Bound both lists before invoking anything, including module opens/closes.
static inline int rpcUnpack(uint8_t *in,unsigned bytes,uint32_t sc,uint8_t *out,
    PicassoRpcArg *args,unsigned *outBytes) {
    unsigned ni=(sc>>16)&255,no=(sc>>8)&255,cursor=0,w=0;
    *outBytes=0;rpcZero(out,PicassoRpcBlob);
    if((sc&255) || ni+no>8)return 14;
    for(unsigned i=0;i<ni+no;++i) {
        if(cursor>bytes || bytes-cursor<4)return 14;
        unsigned len=rpc32(in+cursor);cursor+=4;args[i]={nullptr,len};
        if(i<ni) { if(len) { cursor=(cursor+7)&~7U;if(cursor>bytes || len>bytes-cursor)return 14;
            args[i].data=in+cursor;cursor+=len; } }
        else { if(w>PicassoRpcBlob-4)return 77;rpcPut32(out+w,len);w+=4;
            if(len) { w=(w+7)&~7U;if(w>PicassoRpcBlob || len>PicassoRpcBlob-w)return 77;
                args[i].data=out+w;rpcZero(out+w,len);w+=len; } }
    }
    if(cursor!=bytes)return 14;
    *outBytes=w;return 0;
}
static inline bool rpcString(const PicassoRpcArg &a,unsigned len,char *out) {
    if(!len || len>128 || len>a.bytes || !a.data || a.data[len-1])return false;
    for(unsigned i=0;i<len-1;++i)if(a.data[i]<32 || a.data[i]>126)return false;
    for(unsigned i=0;i<len;++i)out[i]=char(a.data[i]);return true;
}
static inline int rpcReverse(PicassoRpcDomain &d,PicassoRpcArg *a,uint32_t sc,uint32_t handle) {
    // Only a real locally implemented apps_std module can receive a handle.
    if(handle==0 && sc==0x00020200) {
        if(a[0].bytes<8 || a[2].bytes<8 || rpc32(a[0].data+4)>a[3].bytes ||
            !rpcString(a[1],rpc32(a[0].data),d.name))return 14;
        if(!rpcName(d.name,"apps_std"))return 62;
        d.stdOpen=true;++d.opens;rpcPut32(a[2].data,0x100);rpcPut32(a[2].data+4,0);return 0;
    }
    if(handle==0 && sc==0x01010200) {
        if(a[0].bytes<8 || a[1].bytes<4 || rpc32(a[0].data+4)>a[2].bytes)return 14;
        if(rpc32(a[0].data)!=0x100 || !d.opens)return 44;
        --d.opens;d.stdOpen=d.opens!=0;rpcPut32(a[1].data,0);return 0;
    }
    if(handle!=0x100 || !d.stdOpen)return 44;
    return picasso_rpc_fs_invoke(d.pd,sc,a,d.name);
}
static inline int rpcNext(PicassoFastrpc *f,unsigned n) {
    auto &d=f->domain[n];uint8_t prim[16];rpcPut32(prim,d.previousCtx);rpcPut32(prim+4,d.previousResult);
    rpcPut32(prim+8,d.replyBytes);rpcPut32(prim+12,PicassoRpcBlob);
    const uint8_t *in[2]={prim,d.reply};const uint32_t lengths[4]={16,d.replyBytes,16,PicassoRpcBlob};
    return rpcBuild(f,n,1,3,0x04020200,in,lengths);
}
static inline void picasso_rpc_tick(PicassoFastrpc *f) {
    if(!f->bound || f->stopped)return;
    int error=f->io.healthy(f->io.context);if(error) { picasso_rpc_stop(f,error);return; }
    for(unsigned n=0;n<2;++n) {
        auto &d=f->domain[n];if(d.error)continue;
        for(unsigned slot=0;slot<2;++slot) { auto &c=d.call[slot];
            // next2 is a blocking listener and may remain idle indefinitely.
            if((c.queued || (c.pending && slot==0)) && ++c.ticks>250) { d.error=-60;++f->events;break; }
            if(!c.complete)continue;
            error=f->io.sync(f->io.context,n,slot,false);
            if(error) { picasso_rpc_stop(f,error);return; }
            c.complete=false;
            if(c.retval) { d.error=int(c.retval);++f->events;break; }
            auto *p=d.memory+slot*PicassoRpcSlotBytes;
            if(slot==0) {
                if(d.stage==3) { d.defaultHandle=rpc32(p+c.offset[2]);
                    if(rpc32(p+c.offset[2]+4) || d.defaultHandle<4 || d.defaultHandle==0xffffffffU) { d.error=-71;++f->events;break; } }
                if(d.stage==2)d.initialized=true;
                if(d.stage==4)d.registered=true;
                ++d.stage;
            } else {
                auto *prim=p+c.offset[2];d.previousCtx=rpc32(prim);d.lastHandle=rpc32(prim+4);d.lastScalar=rpc32(prim+8);
                unsigned len=rpc32(prim+12);d.wireBytes=len;++d.requests;d.name[0]=0;
                if(!d.previousCtx) { d.error=-71;++f->events;break; }
                PicassoRpcArg args[8]={};d.replyBytes=0;
                d.previousResult=len>PicassoRpcBlob?77:rpcUnpack(p+c.offset[3],len,d.lastScalar,d.reply,args,&d.replyBytes);
                d.decodeResult=d.previousResult;
                if(!d.previousResult)d.previousResult=rpcReverse(d,args,d.lastScalar,d.lastHandle);
                if(d.previousResult)d.replyBytes=0;
                ++f->events;
            }
        }
        if(d.error)continue;
        auto &c=d.call[0];
        if(!c.pending && !c.queued && !c.complete && d.stage<5) {
            if(d.stage==0) { uint8_t pid[4];rpcPut32(pid,d.pid);const uint8_t *in[1]={pid};const uint32_t len[1]={4};
                error=rpcBuild(f,n,0,1,0x10000,in,len);if(!error)d.stage=1; }
            else if(d.stage==2)error=rpcBuild(f,n,0,3,0x03000000,nullptr,nullptr);
            else if(d.stage==3) { const char name[]="adsp_default_listener";uint8_t prim[8];rpcPut32(prim,sizeof(name));rpcPut32(prim+4,128);
                const uint8_t *in[2]={prim,reinterpret_cast<const uint8_t *>(name)};const uint32_t len[4]={8,sizeof(name),8,128};
                error=rpcBuild(f,n,0,0,0x20200,in,len); }
            else if(d.stage==4)error=rpcBuild(f,n,0,d.defaultHandle,0,nullptr,nullptr);
            if(error) { d.error=error;++f->events;continue; }
        }
        if(d.initialized && !d.call[1].pending && !d.call[1].queued && !d.call[1].complete) {
            error=rpcNext(f,n);if(error) { d.error=error;++f->events; }
        }
    }
}
// Return one SMQ datagram. Ownership moves only after GLINK TX head publication.
static inline int picasso_rpc_packet(PicassoFastrpc *f,uint32_t *words) {
    if(!f->bound || f->stopped)return -1;
    for(unsigned n=0;n<2;++n)for(unsigned slot=0;slot<2;++slot) {
        const auto &d=f->domain[n];const auto &c=d.call[slot];if(d.error || !c.queued)continue;
        words[0]=c.handle==1?0:d.pid;words[1]=d.pid*2+slot;words[2]=uint32_t(c.ctx);words[3]=uint32_t(c.ctx>>32);
        words[4]=c.handle;words[5]=c.sc;
        uint64_t addr=(c.sc&0xffffff)?d.address+slot*PicassoRpcSlotBytes:0;
        words[6]=uint32_t(addr);words[7]=uint32_t(addr>>32);words[8]=addr?PicassoRpcSlotBytes:0;words[9]=0;
        return int(n*2+slot);
    }
    return -1;
}
static inline void picasso_rpc_sent(PicassoFastrpc *f,unsigned index) {
    auto &c=f->domain[index/2].call[index%2];c.queued=false;c.pending=true;c.ticks=0;++f->sent;++f->events;
}
#endif
