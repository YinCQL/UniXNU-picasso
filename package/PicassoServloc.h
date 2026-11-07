// SPDX-License-Identifier: BSD-2-Clause-Patent
// APSS service locator wire facts: stock service-locator-private.h/qmi_encdec.c.
// Database from this device's six NON-HLOS image/*.jsn files, not live PD state.
#ifndef PICASSO_SERVLOC_H
#define PICASSO_SERVLOC_H
#include "PicassoQmi.h"
enum { PicassoServlocPort=0x187, PicassoServlocBytes=512, PicassoServlocDomains=6 };
struct PicassoServloc {
    uint32_t requests, replies, malformed, unsupported, registrations, events;
    uint32_t lastNode, lastPort, lastMessage, lastTransaction, lastOffset, lastTotal, lastError;
    char lastName[65];
    bool announced;
};
struct PicassoServlocRequest {
    uint32_t message, transaction, offset, total, error;
    char name[65];
    bool valid, registration;
};
inline const char *picasso_servloc_domain(unsigned n) {
    return n<3?picasso_qmi_domain(n):n==3?"msm/cdsp/root_pd":n==4?"msm/modem/root_pd":"msm/modem/wlan_pd";
}
inline unsigned picasso_servloc_instance(unsigned n) { return n<3?74:n==3?76:180; }
inline const char *picasso_servloc_service(unsigned domain,unsigned index) {
    if (!index) return "tms/servreg";
    if (domain==2 && index==1) return "avs/audio";
    if (domain==4 && index<3) return index==1?"tms/pdr_enabled":"gps/gps_service";
    if (domain==5 && index<3) return index==1?"kernel/elf_loader":"wlan/fw";
    return nullptr;
}
inline bool picasso_servloc_same(const char *a,const uint8_t *b,unsigned size) {
    unsigned n=0;while(n<size && a[n] && uint8_t(a[n])==b[n])++n;
    return n==size && !a[n];
}
inline unsigned picasso_servloc_length(const char *s) { unsigned n=0;while(s[n])++n;return n; }
inline void picasso_servloc_put16(uint8_t *p,unsigned v) { p[0]=uint8_t(v);p[1]=uint8_t(v>>8); }
inline void picasso_servloc_put32(uint8_t *p,uint32_t v) {
    picasso_servloc_put16(p,v);picasso_servloc_put16(p+2,v>>16);
}
// Pure encoder: no state changes until QRTR has reserved BOTH the answer and
// optional RESUME slots. Invalid/truncated headers cannot solicit a response.
inline unsigned picasso_servloc_answer(const uint8_t *p,unsigned size,uint8_t *out,PicassoServlocRequest *r) {
    *r={};
    if(size<7 || p[0]!=0 || picasso_qmi_u16(p+5)!=size-7) return 0;
    r->transaction=picasso_qmi_u16(p+1);r->message=picasso_qmi_u16(p+3);
    const uint8_t *name=nullptr,*list=nullptr;unsigned nameBytes=0,listBytes=0;bool offsetSeen=false;
    for(unsigned at=7;at<size;) {
        if(size-at<3) { r->error=1;break; }
        unsigned type=p[at],len=picasso_qmi_u16(p+at+1);at+=3;
        if(len>size-at) { r->error=1;break; }
        if(type==1) {
            if(name || !len || len>64) { r->error=1;break; }
            name=p+at;nameBytes=len;
            for(unsigned n=0;n<len;++n) if(p[at+n]<0x21 || p[at+n]>0x7e) r->error=1;
        } else if(type==0x10 && r->message==0x21) {
            if(offsetSeen || len!=4) { r->error=1;break; }
            offsetSeen=true;r->offset=picasso_qmi_u32(p+at);
        } else if(type==2 && r->message==0x22) {
            if(list || !len) { r->error=1;break; }list=p+at;listBytes=len;
        } else { r->error=94;break; }
        at+=len;
    }
    if(name) for(unsigned n=0;n<nameBytes;++n) r->name[n]=char(name[n]);
    if(r->message!=0x21 && r->message!=0x22) r->error=94;
    else if(!name && !r->error) r->error=1;
    // A repeated registration is accepted only after matching every service,
    // instance and data field against the existing immutable firmware database.
    // This cannot silently register a different domain/service or fake success.
    if(r->message==0x22 && !r->error) {
        unsigned domain=0;while(domain<3 && !picasso_servloc_same(picasso_servloc_domain(domain),name,nameBytes))++domain;
        if(domain==3) r->error=41;
        else if(!list) r->error=1;
        else {
            unsigned count=list[0],at=1,seen=0;
            if(count!=(domain==2?2U:1U)) r->error=94;
            for(unsigned n=0;n<count && !r->error;++n) {
                if(at>=listBytes) { r->error=1;break; }
                unsigned len=list[at++];
                if(!len || len>64 || len>listBytes-at || listBytes-at-len<9) { r->error=1;break; }
                unsigned index=0;const char *service;
                while((service=picasso_servloc_service(domain,index)) && !picasso_servloc_same(service,list+at,len))++index;
                if(!service || (seen&(1U<<index)) || picasso_qmi_u32(list+at+len)!=74 ||
                   list[at+len+4]!=0 || picasso_qmi_u32(list+at+len+5)!=0) r->error=94;
                else seen|=1U<<index;
                at+=len+9;
            }
            if(at!=listBytes && !r->error) r->error=1;
            r->registration=!r->error;
        }
    }
    for(unsigned n=0;n<PicassoServlocBytes;++n) out[n]=0;
    out[0]=2;picasso_servloc_put16(out+1,r->transaction);picasso_servloc_put16(out+3,r->message);
    out[7]=2;out[8]=4;picasso_servloc_put16(out+10,r->error?1:0);picasso_servloc_put16(out+12,r->error);
    unsigned at=14;
    if(!r->error && r->message==0x21) {
        unsigned listed=0;at=28;
        for(unsigned n=0;n<PicassoServlocDomains;++n) {
            bool provides=false;const char *service;
            for(unsigned s=0;(service=picasso_servloc_service(n,s));++s)
                if(picasso_servloc_same(service,name,nameBytes)) provides=true;
            if(!provides) continue;
            if(r->total++<r->offset) continue;
            const char *domain=picasso_servloc_domain(n);unsigned len=picasso_servloc_length(domain);
            // Six bounded domain names always fit; still fail without overflow.
            if(at+1+len+9>PicassoServlocBytes) return 0;
            out[at++]=uint8_t(len);for(unsigned i=0;i<len;++i)out[at++]=uint8_t(domain[i]);
            picasso_servloc_put32(out+at,picasso_servloc_instance(n));at+=9;++listed;
        }
        out[14]=0x10;out[15]=2;picasso_servloc_put16(out+17,r->total);
        out[19]=0x11;out[20]=2;out[22]=1; // immutable database revision
        out[24]=0x12;picasso_servloc_put16(out+25,at-27);out[27]=uint8_t(listed);
        if(!listed) at=24; // total=0 or exhausted offset; no fabricated entry
    }
    r->valid=!r->error;picasso_servloc_put16(out+5,at-7);return at;
}
#endif
