/* SPDX-License-Identifier: GPL-2.0-only */
#include "qbt_geni.h"
#include "qbt_geni_regs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fixture {
    struct qbt_geni g;
    uint32_t regs[0x4000/4];
    _Alignas(64) uint8_t tx[QBT_GENI_TX_SIZE],rx[QBT_GENI_RX_COUNT][QBT_GENI_RX_SIZE];
    uint8_t received[65536],sent[QBT_GENI_TX_SIZE];
    unsigned length, sent_length, reads, writes, barriers, wakes, syncs;
    bool masked, blocked, device_rx[QBT_GENI_RX_COUNT], device_tx;
    size_t max_receive;
    uint64_t time;
};
static uint32_t read_reg(void *p,uint32_t r) {
    struct fixture *f=p;f->reads++;assert(r<sizeof(f->regs));return f->regs[r/4];
}
static void write_reg(void *p,uint32_t r,uint32_t v) {
    struct fixture *f=p;f->writes++;assert(r<sizeof(f->regs));
    if(r==G_M_CLEAR)f->regs[G_M_IRQ/4]&=~v;
    else if(r==G_S_CLEAR)f->regs[G_S_IRQ/4]&=~v;
    else if(r==G_TX_CLEAR)f->regs[G_TX_IRQ/4]&=~v;
    else if(r==G_RX_CLEAR)f->regs[G_RX_IRQ/4]&=~v;
    else f->regs[r/4]=v;
    if(r==G_S_CMD)f->regs[G_STATUS/4]|=G_S_ACTIVE;
    if(r==G_M_CMD)f->regs[G_STATUS/4]|=G_M_ACTIVE;
    if(r==G_TX_DMA_LEN){assert(f->device_tx);f->sent_length=v;memcpy(f->sent,f->tx,v);f->regs[0x910/4]=v;}
    if(r==G_RX_DMA_LEN)assert(f->device_rx[f->g.rx_active]);
}
static void barrier(void *p) { ((struct fixture *)p)->barriers++; }
static void sync_dma(void *p,const struct qbt_dma_buffer *b,size_t n,bool device,bool tx) {
    struct fixture *f=p;f->syncs++;assert(n<=b->capacity);
    if(tx) {assert(b->bytes==f->tx);f->device_tx=device;return;}
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)if(b->bytes==f->rx[i]){f->device_rx[i]=device;return;}
    assert(false);
}
static uint64_t now(void *p) {return ((struct fixture *)p)->time;}
static void mask(void *p,bool masked) {((struct fixture *)p)->masked=masked;}
static void wake(void *p) {((struct fixture *)p)->wakes++;}
static size_t receive(void *p,const uint8_t *b,size_t n) {
    struct fixture *f=p;if(f->blocked)return 0;
    if(f->max_receive && n>f->max_receive)n=f->max_receive;
    assert(f->length+n<=sizeof(f->received));memcpy(f->received+f->length,b,n);f->length+=(unsigned)n;return n;
}
static void prepare(struct fixture *f) {
    memset(f,0,sizeof(*f));f->regs[G_FW_REV/4]=0x200;f->regs[G_HW_PARAM1/4]=16u<<16;
    struct qbt_geni_ops ops={.context=f,.read=read_reg,.write=write_reg,.barrier=barrier,.sync=sync_dma,
        .now_ns=now,.irq_mask=mask,.wake_worker=wake,.receive=receive};
    struct qbt_dma_buffer tx={f->tx,0x102000000ULL,sizeof(f->tx)},rx[QBT_GENI_RX_COUNT];
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)rx[i]=(struct qbt_dma_buffer){f->rx[i],0x103000000ULL+i*4096,sizeof(f->rx[i])};
    assert(!qbt_geni_prepare(&f->g,&ops,&tx,rx));assert(f->masked);
    assert(f->regs[G_TX_PACK0/4]==(14u|(270u<<10)));
    assert(f->regs[G_TX_PACK1/4]==(526u|(783u<<10)));
    assert(qbt_geni_set_rate(&f->g,3200000,100000000,16)==-ERANGE);
    assert(!qbt_geni_set_rate(&f->g,115200,7372800,16));assert(f->regs[G_M_CLK/4]==0x41);
    f->regs[G_S_IRQ/4]=1u<<11;f->regs[G_RX_IRQ/4]=3u<<7;
    assert(!qbt_geni_start(&f->g));
    assert(!f->regs[G_S_IRQ/4] && !f->regs[G_RX_IRQ/4]);assert(!f->masked && f->g.rx_busy);
    assert(f->regs[G_RX_PTR_H/4]==1);assert(f->regs[G_RX_DMA_LEN/4]==QBT_GENI_RX_SIZE);
}
static void irq(struct fixture *f) {
    unsigned reads=f->reads,writes=f->writes,syncs=f->syncs;
    qbt_geni_irq(&f->g);assert(f->masked && f->wakes);
    assert(reads==f->reads && writes==f->writes && syncs==f->syncs);
}
static void inject(struct fixture *f,const uint8_t *b,unsigned n) {
    assert(f->g.rx_busy && n<=QBT_GENI_RX_SIZE);
    unsigned i=f->g.rx_active;assert(f->device_rx[i]);memcpy(f->rx[i],b,n);
    f->regs[G_RX_LEN_IN/4]=n;f->regs[G_RX_IRQ/4]|=G_EOT;irq(f);
}
static void test_tx(void) {
    struct fixture f;prepare(&f);const uint8_t b[]={1,3,12,0};
    assert(qbt_geni_write(&f.g,b,sizeof(b))==sizeof(b));assert(f.device_tx);
    assert(!memcmp(f.sent,b,sizeof(b)));assert(f.regs[G_TX_PTR_H/4]==1);
    assert(f.regs[G_TX_IRQ_SET/4]==(G_DONE|G_EOT|G_DMA_ERROR));
    assert(!qbt_geni_write(&f.g,b,sizeof(b)));assert(!qbt_geni_drained(&f.g));
    /* DMA completion can precede the last bit leaving the UART. */
    f.regs[G_TX_IRQ/4]|=G_DONE;irq(&f);qbt_geni_service(&f.g);
    assert(!f.device_tx && !qbt_geni_drained(&f.g));assert(!qbt_geni_write(&f.g,b,sizeof(b)));
    f.regs[G_STATUS/4]&=~G_M_ACTIVE;f.regs[G_M_IRQ/4]|=G_DONE;
    irq(&f);qbt_geni_service(&f.g);assert(qbt_geni_drained(&f.g));
    assert(f.g.stats.tx_bytes==sizeof(b) && f.g.stats.tx_dma==1);
    /* Hardware retains M_GP_LENGTH after completion; the next command still starts. */
    assert(f.regs[0x910/4]==sizeof(b));assert(qbt_geni_write(&f.g,b,sizeof(b))==sizeof(b));
    f.regs[G_TX_IRQ/4]|=G_DONE;f.regs[G_STATUS/4]&=~G_M_ACTIVE;irq(&f);qbt_geni_service(&f.g);
    qbt_geni_set_flow(&f.g,false);assert(f.regs[G_TX_CFG/4]&2);assert(f.regs[G_MANUAL_RFR/4]==G_RFR_OFF);
    qbt_geni_set_flow(&f.g,true);assert(!(f.regs[G_TX_CFG/4]&2));assert(!f.regs[G_MANUAL_RFR/4]);
}
static void test_rx(void) {
    struct fixture f;prepare(&f);f.blocked=true;
    uint8_t payload[QBT_GENI_RX_SIZE];
    for(unsigned k=0;k<QBT_GENI_RX_COUNT;k++){
        memset(payload,(int)k,sizeof(payload));inject(&f,payload,sizeof(payload));qbt_geni_service(&f.g);
        assert(f.g.rx_ready==k+1);
    }
    assert(!f.g.rx_busy);assert(f.regs[G_MANUAL_RFR/4]==G_RFR_OFF);assert(f.g.stats.rx_pauses==1);
    assert(qbt_geni_quiesce(&f.g)==-EBUSY);
    f.blocked=false;f.max_receive=37;
    for(unsigned i=0;i<240 && f.g.rx_ready;i++)qbt_geni_service(&f.g);
    assert(f.length==sizeof(payload)*QBT_GENI_RX_COUNT);assert(f.g.rx_busy && !f.g.rx_ready);
    for(unsigned k=0;k<QBT_GENI_RX_COUNT;k++)for(unsigned i=0;i<sizeof(payload);i++)assert(f.received[k*sizeof(payload)+i]==k);
    assert(f.g.stats.rx_backpressure && f.g.stats.rx_bytes==f.length);
    /* Stale/EOT interrupts with zero bytes do not create empty queue entries. */
    inject(&f,payload,0);qbt_geni_service(&f.g);assert(!f.g.rx_ready && f.g.rx_busy);
    assert(qbt_geni_quiesce(&f.g)==-EINPROGRESS);
    f.regs[G_STATUS/4]&=~G_S_ACTIVE;f.regs[G_S_IRQ/4]|=1u<<4;
    f.regs[G_RX_LEN_IN/4]=0;f.regs[G_RX_IRQ/4]|=G_EOT;irq(&f);qbt_geni_service(&f.g);
    assert(f.g.state==QBT_GENI_STOPPED && f.masked && !f.g.rx_busy);
    assert(!qbt_geni_set_rate(&f.g,3200000,51200000,16));assert(f.regs[G_M_CLK/4]==0x11);
    assert(!qbt_geni_resume(&f.g));assert(!f.masked && f.g.rx_busy);
}
static void test_faults(void) {
    struct fixture f;prepare(&f);
    assert(qbt_geni_quiesce(&f.g)==-EINPROGRESS);
    assert(qbt_geni_deadline(&f.g)==G_STOP_TIMEOUT_NS);
    for(unsigned i=0;i<3;i++){f.time+=G_STOP_TIMEOUT_NS;qbt_geni_service(&f.g);}
    assert(f.g.state==QBT_GENI_FAILED && f.g.error==-ETIMEDOUT && f.masked);
    assert(f.g.rx_busy); /* Outstanding DMA storage must remain pinned. */
    prepare(&f);f.regs[G_RX_IRQ/4]=G_DMA_ERROR;irq(&f);qbt_geni_service(&f.g);
    assert(f.g.state==QBT_GENI_FAILED && f.g.stats.dma_errors==1 && f.masked);
    /* A parity error drops its data and RX continues. */
    prepare(&f);f.regs[G_RX_IRQ/4]=(1u<<5)|G_EOT;f.regs[G_RX_LEN_IN/4]=8;irq(&f);qbt_geni_service(&f.g);
    assert(f.g.state==QBT_GENI_RUN && f.g.stats.line_errors==1 && f.g.fault_irq[3]==((1u<<5)|G_EOT));
    assert(!f.length && !f.g.rx_ready && f.g.rx_busy);
    /* A break or a line status bit is counted without stopping the transport. */
    prepare(&f);f.regs[G_S_IRQ/4]=1u<<11;irq(&f);qbt_geni_service(&f.g);
    assert(f.g.state==QBT_GENI_RUN && f.g.stats.line_errors==1);
    prepare(&f);f.regs[G_RX_IRQ/4]=G_DONE;f.regs[G_RX_LEN_IN/4]=QBT_GENI_RX_SIZE+1;
    irq(&f);qbt_geni_service(&f.g);assert(f.g.error==-EOVERFLOW);
}
int main(void) {
    test_tx();test_rx();test_faults();
    puts("PASS: GENI DMA ownership, 64-bit addresses, wire drain, bounded IRQ, RX backpressure, clock-rate validation, quiesce/resume and DMA faults");
    return 0;
}
