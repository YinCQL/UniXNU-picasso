// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso stock net/qrtr/qrtr.c wire format. Bounded discovery, no QMI services.
#ifndef PICASSO_QRTR_H
#define PICASSO_QRTR_H
#include <stdint.h>
enum { PicassoQrtrServices = 64, PicassoQrtrQueue = 8 };
static constexpr uint32_t PicassoQrtrControl = 0xfffffffeU;
struct PicassoQrtrService { uint32_t service, instance, node, port; bool used; };
struct PicassoQrtr {
    PicassoQrtrService services[PicassoQrtrServices];
    uint32_t queue[PicassoQrtrQueue][13];
    uint32_t head, count, peer, received, malformed, ignored, events, overflow;
    uint32_t lastType, lastService, lastInstance, lastNode, lastPort, data, resumes;
    uint32_t wireVersion, wireBytes, sourceNode, sourcePort, destNode, destPort, payloadBytes;
    uint32_t payloadWords[4];
    bool helloSent, helloReceived, intentAsked, intentGranted;
    int error;
};
inline uint32_t picasso_qrtr_u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
inline uint32_t picasso_qrtr_u16(const uint8_t *p) { return p[0] | (uint32_t(p[1])<<8); }
inline void picasso_qrtr_control(uint32_t *w, uint32_t type, uint32_t sn, uint32_t sp,
                                uint32_t dn, uint32_t dp, uint32_t a=0, uint32_t b=0) {
    w[0]=1; w[1]=type; w[2]=sn; w[3]=sp; w[4]=0; w[5]=20; w[6]=dn; w[7]=dp;
    w[8]=type; w[9]=a; w[10]=b; w[11]=0; w[12]=0;
}
inline void picasso_qrtr_invalidate(PicassoQrtr *q, int error) {
    q->helloReceived=false; q->helloSent=false; q->count=0; q->error=error;
    for (auto &s:q->services) s.used=false;
}
// -11 retains the complete GLINK receive buffer for retry, without consuming
// a packet twice. RESUME_TX acknowledges transport receipt, never QMI success.
inline int picasso_qrtr_receive(PicassoQrtr *q, const uint8_t *p, uint32_t n) {
    uint32_t type=0,sn=0,sp=0,dn=0,dp=0,size=0,header=0; bool confirm=false;
    bool valid=n>=16 && !(n&3);
    if (valid && p[0]==1 && n>=32 && picasso_qrtr_u32(p)==1) {
        header=32; type=picasso_qrtr_u32(p+4); sn=picasso_qrtr_u32(p+8); sp=picasso_qrtr_u32(p+12);
        confirm=picasso_qrtr_u32(p+16)!=0; size=picasso_qrtr_u32(p+20);
        dn=picasso_qrtr_u32(p+24); dp=picasso_qrtr_u32(p+28);
    } else if (valid && p[0]==3) {
        header=16+p[3]; type=p[1]; confirm=(p[2]&1)!=0; size=picasso_qrtr_u32(p+4);
        sn=picasso_qrtr_u16(p+8); sp=picasso_qrtr_u16(p+10);
        dn=picasso_qrtr_u16(p+12); dp=picasso_qrtr_u16(p+14);
        if (sp==0xfffe) sp=PicassoQrtrControl;
        if (dp==0xfffe) dp=PicassoQrtrControl;
    } else valid=false;
    if (dp==0xffff) dp=PicassoQrtrControl;
    q->wireVersion=n?p[0]:0; q->wireBytes=n; q->sourceNode=sn; q->sourcePort=sp;
    q->destNode=dn; q->destPort=dp; q->payloadBytes=size;
    for (unsigned i=0;i<4;++i) q->payloadWords[i]=
        header && header<=n && i*4+4<=n-header && i*4+4<=size ? picasso_qrtr_u32(p+header+i*4):0;
    valid=valid && header<=n && size && size<=n-header && ((size+3)&~3U)==n-header &&
        (type==1 || type==7 || dp==PicassoQrtrControl);
    if (valid && type!=1) valid=size>=20 && picasso_qrtr_u32(p+header)==type;
    if (!valid) { ++q->received; ++q->malformed; return 0; }
    if (dn!=1 && dn!=0xffffffffU && !(p[0]==3 && dn==0xffff)) {
        ++q->received; ++q->ignored; return 0; // no forwarding route is installed
    }
    if (type==1 && confirm && q->count==PicassoQrtrQueue) return -11;
    ++q->received; q->lastType=type;
    const uint8_t *payload=p+header;
    if (type==2) {
        q->peer=sn; q->helloReceived=true; ++q->events;
    } else if (type==4 || type==5) {
        uint32_t service=picasso_qrtr_u32(payload+4), instance=picasso_qrtr_u32(payload+8);
        uint32_t node=picasso_qrtr_u32(payload+12), port=picasso_qrtr_u32(payload+16);
        q->lastService=service; q->lastInstance=instance; q->lastNode=node; q->lastPort=port;
        PicassoQrtrService *slot=nullptr;
        for (auto &s:q->services) {
            if (s.used && s.node==node && s.port==port) { slot=&s; break; }
            if (!s.used && !slot) slot=&s;
        }
        if (type==4) {
            if (slot) *slot={service,instance,node,port,true}; else ++q->overflow;
        } else {
            for (auto &s:q->services) if (s.used && s.node==node && s.port==port &&
                s.service==service && s.instance==instance) s.used=false;
        }
        ++q->events;
    } else if (type==3 || type==13) {
        uint32_t node=type==3 ? sn : picasso_qrtr_u32(payload+8);
        for (auto &s:q->services) if (s.used && s.node==node) s.used=false;
        if (node==q->peer) q->helloReceived=false;
        ++q->events;
    } else if (type==1) {
        ++q->data; ++q->ignored; // no local QMI consumer registered yet
        if (confirm) {
            auto *w=q->queue[(q->head+q->count)%PicassoQrtrQueue];
            picasso_qrtr_control(w,7,dn,dp,sn,sp,dn,dp); ++q->count;
        }
    } else ++q->ignored;
    return 0;
}
#endif
