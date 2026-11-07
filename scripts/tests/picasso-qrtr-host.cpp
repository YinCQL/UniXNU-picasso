// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independently encoded v1/v3 peer packets exercise the production decoder.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoQrtr.h"
static void put(uint8_t *p,uint32_t v) { for(unsigned n=0;n<4;++n)p[n]=uint8_t(v>>(8*n)); }
static void control(uint8_t *p,unsigned type,unsigned service=0,unsigned instance=0,unsigned node=2,unsigned port=73) {
    memset(p,0,64);put(p,1);put(p+4,type);put(p+8,2);put(p+12,0xfffffffe);put(p+20,20);
    put(p+24,1);put(p+28,0xfffffffe);put(p+32,type);put(p+36,service);put(p+40,instance);put(p+44,node);put(p+48,port);
}
int main() {
    PicassoQrtr q={};uint8_t p[64];control(p,2);assert(!picasso_qrtr_receive(&q,p,52));
    assert(q.helloReceived && q.peer==2 && !q.helloSent);
    control(p,4,0x40,0x101);assert(!picasso_qrtr_receive(&q,p,52));
    assert(q.services[0].used && q.services[0].service==0x40 && q.services[0].node==2);
    control(p,4,0x40,0x102);assert(!picasso_qrtr_receive(&q,p,52));
    assert(q.services[0].instance==0x102 && !q.services[1].used);
    control(p,5,0x40,0x101);picasso_qrtr_receive(&q,p,52);assert(q.services[0].used);
    control(p,5,0x40,0x102);picasso_qrtr_receive(&q,p,52);assert(!q.services[0].used);
    for(unsigned n=0;n<65;++n) { control(p,4,n,1,2,n+100);picasso_qrtr_receive(&q,p,52); }
    assert(q.overflow==1 && q.services[63].used);
    control(p,3);picasso_qrtr_receive(&q,p,52);assert(!q.helloReceived);
    for(const auto &s:q.services)assert(!s.used);
    control(p,4,0x100,2,3,25);picasso_qrtr_receive(&q,p,52);
    control(p,13,0,3);picasso_qrtr_receive(&q,p,52);assert(!q.services[0].used);
    // Compact v3 with four optional header bytes and normalized control ports.
    memset(p,0,sizeof p);p[0]=3;p[1]=4;p[3]=4;put(p+4,20);put(p+8,0xfffe0002);put(p+12,0xfffe0001);
    put(p+20,4);put(p+24,0x55);put(p+28,0x100);put(p+32,2);put(p+36,98);
    picasso_qrtr_receive(&q,p,40);assert(q.services[0].used && q.services[0].service==0x55);
    unsigned bad=q.malformed;p[3]=255;picasso_qrtr_receive(&q,p,40);assert(q.malformed==++bad);
    control(p,2);put(p+20,0xffffffff);picasso_qrtr_receive(&q,p,52);assert(q.malformed==++bad);
    control(p,2);put(p+32,4);picasso_qrtr_receive(&q,p,52);assert(q.malformed==++bad);
    control(p,2);put(p+28,99);picasso_qrtr_receive(&q,p,52);assert(q.malformed==++bad);
    for(unsigned n=0;n<52;++n) { control(p,2);picasso_qrtr_receive(&q,p,n);assert(q.malformed==++bad); }
    control(p,2);put(p+28,0xffff);picasso_qrtr_receive(&q,p,52);assert(q.helloReceived);
    // Confirmed DATA is consumed once, with a bounded pending RESUME queue.
    control(p,1);put(p+12,0xabc);put(p+16,1);put(p+28,0xdef);
    for(unsigned n=0;n<PicassoQrtrQueue;++n)assert(!picasso_qrtr_receive(&q,p,52));
    unsigned received=q.received;assert(picasso_qrtr_receive(&q,p,52)==-11 && q.received==received);
    const uint32_t expected[]={1,7,1,0xdef,0,20,2,0xabc,7,1,0xdef,0,0};
    assert(!memcmp(q.queue[0],expected,sizeof expected));
    q.head=1;--q.count;assert(!picasso_qrtr_receive(&q,p,52) && q.received==received+1);
    assert(!memcmp(q.queue[0],expected,sizeof expected));
    put(p+24,99);received=q.received;assert(!picasso_qrtr_receive(&q,p,52) && q.received==received+1);
    assert(q.count==PicassoQrtrQueue); // off-node DATA cannot solicit a reply
    picasso_qrtr_invalidate(&q,-104);assert(!q.count && !q.helloReceived && q.error==-104);
    puts("PASS QRTR v1/v3 discovery, service lifecycle, bounds, confirmed DATA queue and exact RESUME endpoints; no QMI consumer");
}
