// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only servreg QUERY_STATE wire facts: stock service-notifier-private.h.
// Domains/instance from NON-HLOS image/{adspr,adsps,adspua}.jsn. No fake states.
#ifndef PICASSO_QMI_H
#define PICASSO_QMI_H
#include <stdint.h>
enum { PicassoQmiDomains=3, PicassoQmiPort=0x186 };
struct PicassoQmiDomain {
    uint32_t ticks, state, result, remoteError;
    bool sent, pending, completed, stateValid;
    int error;
};
struct PicassoQmi {
    PicassoQmiDomain domains[PicassoQmiDomains];
    uint32_t node, port, events, received, malformed, ignored;
    bool bound, lost;
};
inline const char *picasso_qmi_domain(unsigned n) {
    return n==0?"msm/adsp/root_pd":n==1?"msm/adsp/sensor_pd":"msm/adsp/audio_pd";
}
inline uint32_t picasso_qmi_u16(const uint8_t *p) { return p[0]|(uint32_t(p[1])<<8); }
inline uint32_t picasso_qmi_u32(const uint8_t *p) { return picasso_qmi_u16(p)|(picasso_qmi_u16(p+2)<<16); }
inline void picasso_qmi_lost(PicassoQmi *q) {
    if (!q->bound) return;
    q->bound=false; q->lost=true; ++q->events;
    for(auto &d:q->domains) { d.pending=false; d.stateValid=false; d.error=-104; }
}
inline int picasso_qmi_due(PicassoQmi *q) {
    int due=-1;
    if (!q->bound) return due;
    for(unsigned n=0;n<PicassoQmiDomains;++n) {
        auto &d=q->domains[n];
        if (d.pending && ++d.ticks>100) { d.pending=false; d.error=-60; ++q->events; }
        if (d.pending) return -1;
        if (!d.sent && due<0) due=int(n);
    }
    return due;
}
// QMI is byte packed, with a seven-byte header and top-level string TLV.
// Fill a whole QRTR v1 packet; caller owns the bounded 96-byte output storage.
inline uint32_t picasso_qmi_query(const PicassoQmi *q,unsigned domain,uint8_t *p) {
    for(unsigned n=0;n<96;++n)p[n]=0;
    auto put=[p](unsigned at,uint32_t v) { for(unsigned n=0;n<4;++n)p[at+n]=uint8_t(v>>(8*n)); };
    const char *name=picasso_qmi_domain(domain);unsigned len=0;while(name[len])++len;
    unsigned payload=10+len;
    put(0,1);put(4,1);put(8,1);put(12,PicassoQmiPort);put(20,payload);put(24,q->node);put(28,q->port);
    p[32]=0;p[33]=uint8_t(domain+1);p[35]=0x21;p[37]=uint8_t(3+len);
    p[39]=1;p[40]=uint8_t(len);for(unsigned n=0;n<len;++n)p[42+n]=uint8_t(name[n]);
    return 32+((payload+3)&~3U);
}
inline bool picasso_qmi_receive(PicassoQmi *q,uint32_t sn,uint32_t sp,uint32_t dp,const uint8_t *p,uint32_t size) {
    if (!q->bound || sn!=q->node || sp!=q->port || dp!=PicassoQmiPort) return false;
    ++q->received;
    if (size<7 || picasso_qmi_u16(p+5)!=size-7) { ++q->malformed; return true; }
    unsigned txn=picasso_qmi_u16(p+1);
    if(p[0]!=2 || picasso_qmi_u16(p+3)!=0x21 || txn<1 || txn>PicassoQmiDomains || !q->domains[txn-1].pending) {
        ++q->ignored;return true;
    }
    bool resultSeen=false,stateSeen=false;uint32_t result=0,remoteError=0,state=0;
    for(unsigned at=7;at<size;) {
        if(size-at<3) { ++q->malformed;return true; }
        unsigned type=p[at],len=picasso_qmi_u16(p+at+1);at+=3;
        if(len>size-at) { ++q->malformed;return true; }
        if(type==2) {
            if(resultSeen || len!=4) { ++q->malformed;return true; }
            resultSeen=true;result=picasso_qmi_u16(p+at);remoteError=picasso_qmi_u16(p+at+2);
        } else if(type==0x10) {
            if(stateSeen || len!=4) { ++q->malformed;return true; }
            stateSeen=true;state=picasso_qmi_u32(p+at);
        } else if(type<0x10) { ++q->malformed;return true; }
        at+=len;
    }
    if(!resultSeen || result>1 || (!result && remoteError) ||
        (!result && stateSeen && state!=0x0fffffff && state!=0x1fffffff && state!=0x2fffffff && state!=0x7fffffff)) {
        ++q->malformed;return true;
    }
    auto &d=q->domains[txn-1];d.pending=false;d.completed=true;d.result=result;d.remoteError=remoteError;
    d.state=state;d.stateValid=!result && stateSeen;d.error=result?-5:0;++q->events;return true;
}
#endif
