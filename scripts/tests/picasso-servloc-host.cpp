// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoQrtr.h"
static unsigned query(uint8_t *p,const char *name,unsigned offset=0) {
    unsigned len=unsigned(strlen(name));memset(p,0,128);
    p[1]=0x34;p[2]=0x12;p[3]=0x21;p[5]=uint8_t(10+len);p[7]=1;p[8]=uint8_t(len);
    memcpy(p+10,name,len);p[10+len]=0x10;p[11+len]=4;
    for(unsigned n=0;n<4;++n)p[13+len+n]=uint8_t(offset>>(8*n));return 17+len;
}
static unsigned packet(uint8_t *p,const uint8_t *data,unsigned bytes,bool confirm=true) {
    memset(p,0,544);uint32_t words[]={1,1,5,88,unsigned(confirm),bytes,1,PicassoServlocPort};
    memcpy(p,words,sizeof words);memcpy(p+32,data,bytes);return 32+((bytes+3)&~3U);
}
int main() {
    uint8_t p[128],out[512],wire[544];PicassoServlocRequest r={};
    unsigned bytes=query(p,"avs/audio");unsigned n=picasso_servloc_answer(p,bytes,out,&r);
    const uint8_t expected[]={2,0x34,0x12,0x21,0,48,0,2,4,0,0,0,0,0,0x10,2,0,1,0,0x11,2,0,1,0,
        0x12,28,0,1,17,'m','s','m','/','a','d','s','p','/','a','u','d','i','o','_','p','d',74,0,0,0,0,0,0,0,0};
    assert(n==sizeof expected && !memcmp(out,expected,n) && r.valid && r.total==1);
    bytes=query(p,"tms/servreg");n=picasso_servloc_answer(p,bytes,out,&r);
    assert(r.total==6 && out[27]==6 && n<256);
    unsigned at=28;
    const char *domains[]={"msm/adsp/root_pd","msm/adsp/sensor_pd","msm/adsp/audio_pd","msm/cdsp/root_pd","msm/modem/root_pd","msm/modem/wlan_pd"};
    const unsigned instances[]={74,74,74,76,180,180};
    for(unsigned d=0;d<6;++d) {
        unsigned len=out[at++];assert(len==strlen(domains[d]) && !memcmp(out+at,domains[d],len));at+=len;
        assert(picasso_qmi_u32(out+at)==instances[d] && !out[at+4] && !picasso_qmi_u32(out+at+5));at+=9;
    }
    assert(at==n);
    bytes=query(p,"tms/servreg",3);picasso_servloc_answer(p,bytes,out,&r);assert(r.total==6 && out[27]==3);
    bytes=query(p,"tms/servreg",0xffffffffU);n=picasso_servloc_answer(p,bytes,out,&r);assert(r.total==6 && n==24);
    bytes=query(p,"no/such");n=picasso_servloc_answer(p,bytes,out,&r);assert(r.total==0 && n==24 && !r.error);
    // Full parse before success: truncation, trailing bytes, duplicates, invalid
    // string bytes, offset length and unknown mandatory messages never succeed.
    bytes=query(p,"avs/audio");for(unsigned i=0;i<bytes;++i)assert(!picasso_servloc_answer(p,i,out,&r));
    assert(!picasso_servloc_answer(p,bytes+1,out,&r));
    p[7]=2;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error);
    bytes=query(p,"avs/audio");p[10]=0;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==1);
    bytes=query(p,"avs/audio");p[20]=3;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==1);
    bytes=query(p,"avs/audio");p[3]=0x24;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==94);
    bytes=query(p,"avs/audio");memcpy(p+bytes,p+7,12);bytes+=12;p[5]=uint8_t(bytes-7);
    assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==1);
    // Static registration is a real validation, not an unconditional ACK.
    memset(p,0,sizeof p);p[1]=7;p[3]=0x22;p[7]=1;p[8]=16;memcpy(p+10,"msm/adsp/root_pd",16);
    // Actual root domain is 16 bytes, aggregate string has its own one-byte length.
    p[26]=2;p[27]=22;p[29]=1;p[30]=11;memcpy(p+31,"tms/servreg",11);p[42]=74;
    bytes=51;p[5]=44;
    assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.registration && !r.error);
    p[42]=75;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==94 && !r.registration);
    p[42]=74;p[46]=1;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==94);
    p[46]=0;p[27]=23;assert(picasso_servloc_answer(p,bytes,out,&r)==14 && r.error==1);
    // A confirmed request needs two slots. Retrying a held receive buffer must
    // not count/register it twice or discard either the ACK or domain response.
    PicassoQrtr q={};q.helloReceived=true;q.peer=5;q.locator.announced=true;
    bytes=query(p,"avs/audio");n=packet(wire,p,bytes);q.count=7;
    assert(picasso_qrtr_receive(&q,wire,n)==-11 && !q.received && !q.locator.requests && q.count==7);
    q.count=0;assert(!picasso_qrtr_receive(&q,wire,n));
    assert(q.count==2 && q.queue[0][1]==7 && q.queue[1][1]==1 && q.queueBytes[1]==88);
    assert(q.locator.requests==1 && !q.locator.replies && q.queue[1][3]==PicassoServlocPort && q.queue[1][7]==88);
    assert(!memcmp(q.queue[1]+8,expected,sizeof expected));
    // Non-peer, off-node and unannounced traffic cannot use the server.
    q={};q.helloReceived=true;q.peer=6;q.locator.announced=true;
    picasso_qrtr_receive(&q,wire,n);assert(!q.locator.requests && q.count==1);
    q.peer=5;q.locator.announced=false;picasso_qrtr_receive(&q,wire,n);assert(!q.locator.requests);
    q.locator.announced=true;wire[24]=2;picasso_qrtr_receive(&q,wire,n);assert(!q.locator.requests);
    picasso_qrtr_invalidate(&q,-104);assert(!q.count && !q.locator.announced);
    q={};q.helloReceived=true;q.peer=5;q.locator.announced=true;
    const uint32_t goodbye[]={1,3,5,0xfffffffe,0,20,1,0xfffffffe,3,0,0,0,0};
    q.count=1;picasso_qrtr_receive(&q,reinterpret_cast<const uint8_t *>(goodbye),sizeof goodbye);
    assert(!q.count && !q.locator.announced && !q.helloReceived);
    puts("PASS service locator exact firmware table/wire, offset, unknown, malformed, registration validation, atomic backpressure and peer loss");
}
