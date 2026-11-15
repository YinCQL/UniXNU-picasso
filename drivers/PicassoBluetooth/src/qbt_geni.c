/* SPDX-License-Identifier: GPL-2.0-only */
/* Qualcomm GENI SE UART DMA. Sequencing follows the locked Linux driver.
 * Hard IRQ only masks the GIC line and wakes the owning executor. */
#include "qbt_geni.h"
#include "qbt_geni_regs.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#else
#include <errno.h>
#include <string.h>
#endif
static uint32_t rd(struct qbt_geni *g, uint32_t r) { return g->ops.read(g->ops.context,r); }
static void wr(struct qbt_geni *g, uint32_t r, uint32_t v) { g->ops.write(g->ops.context,r,v); }
static void barrier(struct qbt_geni *g) { g->ops.barrier(g->ops.context); }
static void rfr(struct qbt_geni *g, bool ready) {
    /* Stopped flow control deasserts RTS across a controller baud change, as Linux does. */
    wr(g,G_MANUAL_RFR,ready&&g->flow?0:G_RFR_OFF);
}
static void arm_rx(struct qbt_geni *g) {
    if (g->rx_busy || g->rx_ready == QBT_GENI_RX_COUNT || g->state != QBT_GENI_RUN) return;
    unsigned i=(g->rx_head+g->rx_ready)%QBT_GENI_RX_COUNT;
    const struct qbt_dma_buffer *b=&g->rx[i];
    g->ops.sync(g->ops.context,b,QBT_GENI_RX_SIZE,true,false);
    wr(g,G_RX_IRQ_SET,G_DONE|G_EOT|G_DMA_ERROR);
    wr(g,G_RX_PTR_L,(uint32_t)b->address);wr(g,G_RX_PTR_H,(uint32_t)(b->address>>32));
    wr(g,G_RX_ATTR,0);barrier(g);
    g->rx_active=i;g->rx_busy=true;
    wr(g,G_RX_DMA_LEN,QBT_GENI_RX_SIZE);barrier(g);rfr(g,true);
}
static bool valid_buffer(const struct qbt_dma_buffer *b, size_t n) {
    return b->bytes && b->capacity>=n && !(b->address&63u) && b->address<=UINT64_MAX-n;
}
int qbt_geni_prepare(struct qbt_geni *g, const struct qbt_geni_ops *ops,
    const struct qbt_dma_buffer *tx, const struct qbt_dma_buffer rx[QBT_GENI_RX_COUNT]) {
    if (!g || !ops || !ops->read || !ops->write || !ops->barrier || !ops->sync ||
        !ops->now_ns || !ops->irq_mask || !ops->wake_worker || !ops->receive ||
        !tx || !rx || !valid_buffer(tx,QBT_GENI_TX_SIZE)) return -EINVAL;
    for (unsigned i=0;i<QBT_GENI_RX_COUNT;i++) {
        if (!valid_buffer(&rx[i],QBT_GENI_RX_SIZE)) return -EINVAL;
        if (rx[i].address<tx->address+QBT_GENI_TX_SIZE && tx->address<rx[i].address+QBT_GENI_RX_SIZE) return -EINVAL;
        for (unsigned j=0;j<i;j++)
            if (rx[i].address<rx[j].address+QBT_GENI_RX_SIZE && rx[j].address<rx[i].address+QBT_GENI_RX_SIZE) return -EINVAL;
    }
    memset(g,0,sizeof(*g));g->ops=*ops;g->tx=*tx;memcpy(g->rx,rx,sizeof(g->rx));g->flow=true;
    ops->irq_mask(ops->context,true);
    if (((rd(g,G_FW_REV)>>8)&255u)!=2) return -ENXIO;
    if (rd(g,G_STATUS)&(G_M_ACTIVE|G_S_ACTIVE)) return -EBUSY;
    unsigned depth=(rd(g,G_HW_PARAM1)>>16)&255u;if(depth<4)return -ENXIO;
    wr(g,G_IRQ_EN,0);wr(g,G_GSI_EVENT,0);
    wr(g,G_M_IRQ_EN,0);wr(g,G_S_IRQ_EN,0);wr(g,G_TX_IRQ_UNSET,UINT32_MAX);wr(g,G_RX_IRQ_UNSET,UINT32_MAX);
    wr(g,G_M_CLEAR,UINT32_MAX);wr(g,G_S_CLEAR,UINT32_MAX);
    wr(g,G_TX_CLEAR,UINT32_MAX);wr(g,G_RX_CLEAR,UINT32_MAX);
    wr(g,G_CGC,rd(g,G_CGC)|0x7f);wr(g,G_DMA_CFG,rd(g,G_DMA_CFG)|0x0f);
    wr(g,G_OUTPUT,0x7f);wr(g,G_FORCE_DEFAULT,1);
    wr(g,G_DMA_MODE,1);wr(g,G_DMA_IF_EN,rd(g,G_DMA_IF_EN)|1);
    /* Four 8-bit words, low byte first, same packing as geni_se_config_packing. */
    wr(g,G_TX_PACK0,0x4380e);wr(g,G_TX_PACK1,0xc3e0e);
    wr(g,G_RX_PACK0,0x4380e);wr(g,G_RX_PACK1,0xc3e0e);wr(g,G_BYTE_GRAN,0);
    wr(g,G_TX_WORD,8);wr(g,G_RX_WORD,8);wr(g,G_TX_STOP,0);
    wr(g,G_TX_PARITY,0);wr(g,G_RX_PARITY,0);wr(g,G_TX_CFG,0);wr(g,G_RX_CFG,0);
    wr(g,G_LOOPBACK,0);wr(g,G_RX_STALE,160);wr(g,G_RX_WM,2);wr(g,G_RX_RFR_WM,depth-2);
    rfr(g,false);barrier(g);g->state=QBT_GENI_STOPPED;return 0;
}
int qbt_geni_set_rate(struct qbt_geni *g,uint32_t baud,uint32_t hz,unsigned sample) {
    if (g->state!=QBT_GENI_STOPPED || !baud || !hz || (sample!=16 && sample!=32)) return -EINVAL;
    uint64_t target=(uint64_t)baud*sample,div=((uint64_t)hz+target/2)/target;
    if (!div || div>4095) return -ERANGE;
    uint64_t actual=(uint64_t)hz/div,diff=actual>target?actual-target:target-actual;
    if (diff*100>target) return -ERANGE;
    wr(g,G_M_CLK,((uint32_t)div<<4)|1);wr(g,G_S_CLK,((uint32_t)div<<4)|1);wr(g,G_CLK_SEL,0);
    barrier(g);g->baud=baud;g->source_hz=hz;g->oversampling=sample;return 0;
}
int qbt_geni_start(struct qbt_geni *g) {
    if (g->state!=QBT_GENI_STOPPED || !g->baud || g->rx_ready) return -EINVAL;
    /* Reset-time line state is stale when the board releases the controller. */
    wr(g,G_M_CLEAR,UINT32_MAX);wr(g,G_S_CLEAR,UINT32_MAX);
    wr(g,G_TX_CLEAR,UINT32_MAX);wr(g,G_RX_CLEAR,UINT32_MAX);
    wr(g,G_M_IRQ_EN,G_DONE|G_CMD_ERRORS|(1u<<31));
    wr(g,G_S_IRQ_EN,G_CMD_ERRORS|G_LINE_ERRORS|(1u<<4)|(1u<<5));
    g->state=QBT_GENI_RUN;
    /* Linux starts the secondary sequencer with UART_PARAM_RFR_OPEN. */
    wr(g,G_S_CMD,(1u<<27)|(1u<<7));arm_rx(g);wr(g,G_IRQ_EN,G_IRQ_MASK);barrier(g);
    __atomic_store_n(&g->irq_pending,false,__ATOMIC_RELEASE);
    g->ops.irq_mask(g->ops.context,false);return 0;
}
void qbt_geni_irq(struct qbt_geni *g) {
    g->ops.irq_mask(g->ops.context,true);
    __atomic_store_n(&g->irq_pending,true,__ATOMIC_RELEASE);
    g->ops.wake_worker(g->ops.context);
}
static bool deliver(struct qbt_geni *g) {
    unsigned budget=QBT_GENI_RX_COUNT;
    while(g->rx_ready && budget--) {
        unsigned i=g->rx_head,remaining=g->rx_length[i]-g->rx_offset;
        size_t n=g->ops.receive(g->ops.context,g->rx[i].bytes+g->rx_offset,remaining);
        if(n>remaining)n=remaining;g->rx_offset+=(unsigned)n;
        if(g->rx_offset!=g->rx_length[i]) {g->stats.rx_backpressure++;return n!=0;}
        g->rx_head=(i+1)%QBT_GENI_RX_COUNT;g->rx_ready--;g->rx_offset=0;arm_rx(g);
    }
    return g->rx_ready!=0;
}
static void fail(struct qbt_geni *g,int error) {
    g->error=error;g->state=QBT_GENI_FAILED;g->stop_deadline=0;
    /* DMA buffers stay pinned until reset/power-off proves DMA is stopped. */
    rfr(g,false);wr(g,G_IRQ_EN,0);barrier(g);g->ops.irq_mask(g->ops.context,true);
}
bool qbt_geni_service(struct qbt_geni *g) {
    if(g->state==QBT_GENI_OFF || g->state==QBT_GENI_STOPPED || g->state==QBT_GENI_FAILED)return false;
    bool irq=__atomic_exchange_n(&g->irq_pending,false,__ATOMIC_ACQ_REL);
    uint32_t mi=rd(g,G_M_IRQ),si=rd(g,G_S_IRQ),ti=rd(g,G_TX_IRQ),ri=rd(g,G_RX_IRQ);
    if(irq)g->stats.interrupts++;
    wr(g,G_M_CLEAR,mi);wr(g,G_S_CLEAR,si);wr(g,G_TX_CLEAR,ti);wr(g,G_RX_CLEAR,ri);
    if(((mi|si)&G_CMD_ERRORS) || ((ti|ri)&G_DMA_ERROR) || (si&G_LINE_ERRORS) || (ri&((1u<<5)|(3u<<7)))) {
        g->fault_irq[0]=mi;g->fault_irq[1]=si;g->fault_irq[2]=ti;g->fault_irq[3]=ri;
    }
    if((mi|si)&G_CMD_ERRORS || (ti|ri)&G_DMA_ERROR) {g->stats.dma_errors++;fail(g,-EIO);return false;}
    /* Line errors drop or mark received data; the transport keeps running, as in Linux. */
    if((si&G_LINE_ERRORS) || (ri&((1u<<5)|(3u<<7))))g->stats.line_errors++;
    if(g->tx_busy && (ti&G_DONE)) {
        g->ops.sync(g->ops.context,&g->tx,g->tx_length,false,true);
        g->stats.tx_bytes+=g->tx_length;g->stats.tx_dma++;g->tx_length=0;g->tx_busy=false;
    }
    if(g->rx_busy && (ri&(G_DONE|G_EOT))) {
        unsigned n=rd(g,G_RX_LEN_IN),i=g->rx_active;
        g->rx_busy=false;
        if(n>QBT_GENI_RX_SIZE){g->stats.dma_errors++;fail(g,-EOVERFLOW);return false;}
        g->ops.sync(g->ops.context,&g->rx[i],n,false,false);
        if(ri&(1u<<5))n=0; /* Parity failure: drop this completion. */
        g->stats.rx_dma++;g->stats.rx_bytes+=n;
        if(n){g->rx_length[i]=n;g->rx_ready++;}
        if(g->rx_ready==QBT_GENI_RX_COUNT){rfr(g,false);g->stats.rx_pauses++;}
        arm_rx(g);
    }
    bool more=deliver(g);
    if(g->state!=QBT_GENI_RUN) {
        uint64_t t=g->ops.now_ns(g->ops.context);
        if(!(rd(g,G_STATUS)&G_S_ACTIVE) && !g->rx_busy && g->state!=QBT_GENI_RESET) {
            g->stop_deadline=0;
            if(g->rx_ready)return more;
            g->state=QBT_GENI_STOPPED;g->stop_deadline=0;wr(g,G_IRQ_EN,0);barrier(g);
            g->ops.irq_mask(g->ops.context,true);return more;
        }
        if(g->state==QBT_GENI_RESET && (ri&G_RESET_DONE)) {
            g->rx_busy=false;
            if(rd(g,G_STATUS)&G_S_ACTIVE){fail(g,-ETIMEDOUT);return false;}
            g->state=QBT_GENI_CANCEL;g->stop_deadline=0;
            if(g->rx_ready)return more;
            g->state=QBT_GENI_STOPPED;g->stop_deadline=0;wr(g,G_IRQ_EN,0);barrier(g);
            g->ops.irq_mask(g->ops.context,true);return more;
        }
        if(g->stop_deadline && t>=g->stop_deadline) {
            g->stats.cancel_timeouts++;g->stop_deadline=t+G_STOP_TIMEOUT_NS;
            if(g->state==QBT_GENI_CANCEL){wr(g,G_S_CTRL,G_ABORT);g->state=QBT_GENI_ABORT;}
            else if(g->state==QBT_GENI_ABORT){wr(g,G_RX_IRQ_SET,G_RESET_DONE);wr(g,G_RX_RESET,1);g->state=QBT_GENI_RESET;}
            else {fail(g,-ETIMEDOUT);return false;}
        }
    }
    barrier(g);g->ops.irq_mask(g->ops.context,false);return more;
}
bool qbt_geni_drained(struct qbt_geni *g) {
    if(g->tx_busy)return false;
    /* M_GP_LENGTH keeps the finished command's byte count; it is not a pending count. */
    return !(rd(g,G_STATUS)&G_M_ACTIVE) && !rd(g,G_TX_FIFO_STATUS);
}
size_t qbt_geni_write(struct qbt_geni *g,const uint8_t *bytes,size_t n) {
    if(g->state!=QBT_GENI_RUN || !bytes || !n || !qbt_geni_drained(g))return 0;
    if(n>QBT_GENI_TX_SIZE)n=QBT_GENI_TX_SIZE;
    memcpy(g->tx.bytes,bytes,n);g->ops.sync(g->ops.context,&g->tx,n,true,true);
    g->tx_busy=true;g->tx_length=(unsigned)n;
    wr(g,G_TX_LEN,(uint32_t)n);wr(g,G_M_CMD,1u<<27);
    wr(g,G_TX_IRQ_SET,G_DONE|G_EOT|G_DMA_ERROR);
    wr(g,G_TX_PTR_L,(uint32_t)g->tx.address);wr(g,G_TX_PTR_H,(uint32_t)(g->tx.address>>32));
    wr(g,G_TX_ATTR,1);barrier(g);wr(g,G_TX_DMA_LEN,(uint32_t)n);barrier(g);return n;
}
void qbt_geni_set_flow(struct qbt_geni *g,bool enabled) {
    g->flow=enabled;uint32_t cfg=rd(g,G_TX_CFG);
    wr(g,G_TX_CFG,enabled?(cfg&~2u):(cfg|2u));
    rfr(g,g->rx_busy && g->state==QBT_GENI_RUN);barrier(g);
}
int qbt_geni_quiesce(struct qbt_geni *g) {
    if(g->state==QBT_GENI_STOPPED)return 0;
    if(g->state==QBT_GENI_FAILED)return g->error;
    if(g->state!=QBT_GENI_RUN)return -EINPROGRESS;
    if(!qbt_geni_drained(g) || g->rx_ready)return -EBUSY;
    g->state=QBT_GENI_CANCEL;rfr(g,false);
    g->stop_deadline=g->ops.now_ns(g->ops.context)+G_STOP_TIMEOUT_NS;
    wr(g,G_S_CTRL,G_CANCEL);barrier(g);return -EINPROGRESS;
}
int qbt_geni_resume(struct qbt_geni *g) { return qbt_geni_start(g); }
uint64_t qbt_geni_deadline(const struct qbt_geni *g) { return g->stop_deadline?g->stop_deadline:UINT64_MAX; }
