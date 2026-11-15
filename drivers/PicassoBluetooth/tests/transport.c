/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "qbt_transport.h"
#define __unused __attribute__((unused))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define PZERO 0
#define EPROTO 71
#define ENOTCONN 107
#define EAGAIN 11
typedef int errno_t,boolean_t;
typedef void *kern_ctl_ref,*kern_nexus_t,*nexus_controller_t,*kern_pbufpool_t;
typedef void *kern_nexus_provider_t,*proc_t,*kern_channel_t,*nexus_port_t;
typedef uint8_t uuid_t[16];
typedef int lck_grp_t;
typedef pthread_mutex_t lck_mtx_t;
struct packet {uint32_t n,offset,limit;uint8_t data[4096];};
struct slot {struct packet *packet;};
struct ring {struct slot slots[8];unsigned count,advanced,notifications;};
typedef struct packet *kern_packet_t,*kern_buflet_t;
typedef struct slot *kern_channel_slot_t;
typedef struct ring *kern_channel_ring_t;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned held;
static void lck_mtx_lock(lck_mtx_t *m){assert(!held);assert(!pthread_mutex_lock(m));held=1;}
static void lck_mtx_unlock(lck_mtx_t *m){assert(held);held=0;assert(!pthread_mutex_unlock(m));}
static void wakeup(void *v){(void)v;pthread_cond_broadcast(&cond);}
static int msleep(void *v,lck_mtx_t *m,int flags,const char *name,void *timeout){assert(held);held=0;int e=pthread_cond_wait(&cond,m);held=1;return e;}
static bool pause_notify,notify_entered,release_notify;
static pthread_mutex_t notify_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notify_cond=PTHREAD_COND_INITIALIZER;
static int kern_channel_notify(kern_channel_ring_t r,int flags){
    assert(!held);pthread_mutex_lock(&notify_mutex);r->notifications++;
    if(pause_notify){notify_entered=true;pthread_cond_broadcast(&notify_cond);while(!release_notify)pthread_cond_wait(&notify_cond,&notify_mutex);}
    pthread_mutex_unlock(&notify_mutex);return 0;
}
static kern_channel_slot_t kern_channel_get_next_slot(kern_channel_ring_t r,kern_channel_slot_t s,void *p){
    unsigned i=s?(unsigned)(s-r->slots)+1:r->advanced;return i<r->count?r->slots+i:NULL;
}
static kern_packet_t kern_channel_slot_get_packet(kern_channel_ring_t r,kern_channel_slot_t s){return s->packet;}
static kern_buflet_t kern_packet_get_next_buflet(kern_packet_t p,void *unused){return p;}
static uint32_t kern_packet_get_data_length(kern_packet_t p){return p->n;}
static uint32_t kern_buflet_get_data_offset(kern_buflet_t p){return p->offset;}
static uint32_t kern_buflet_get_data_limit(kern_buflet_t p){return p->limit;}
static void *kern_buflet_get_data_address(kern_buflet_t p){return p->data;}
static void kern_channel_advance_slot(kern_channel_ring_t r,kern_channel_slot_t s){r->advanced=(unsigned)(s-r->slots)+1;}
static int kern_channel_reclaim(kern_channel_ring_t r){return 0;}
static int kern_nexus_get_pbufpool(kern_nexus_t n,void *u,kern_pbufpool_t *p){*p=n;return 0;}
static bool fail_attach;
static unsigned packets;
static int kern_pbufpool_alloc_nosleep(kern_pbufpool_t p,int count,kern_packet_t *packet){*packet=calloc(1,sizeof(**packet));assert(*packet);(*packet)->limit=4096;packets++;return 0;}
static void kern_pbufpool_free(kern_pbufpool_t p,kern_packet_t packet){free(packet);packets--;}
static void kern_packet_clear_flow_uuid(kern_packet_t p){}
static int kern_buflet_set_data_offset(kern_buflet_t p,unsigned n){p->offset=n;return 0;}
static int kern_buflet_set_data_length(kern_buflet_t p,unsigned n){p->n=n;return 0;}
static int kern_packet_finalize(kern_packet_t p){return 0;}
static int kern_channel_slot_attach_packet(kern_channel_ring_t r,kern_channel_slot_t s,kern_packet_t p){if(fail_attach)return EIO;s->packet=p;return 0;}
#include "transport-unit.inc"
static void *receive_thread(void *v){assert(qbt_transport_receive((uint8_t *)"H4",2)==2);return NULL;}
static void *close_thread(void *v){bt_sk_pre_disconnect(NULL,(void *)1,NULL);return NULL;}
int main(void){
    assert(!pthread_mutex_init(&bt_sk_lock,NULL));
    struct ring tx={0},rx={0};
    bt_sk.connected=true;bt_sk.boot=0;bt_sk.txring=&tx;bt_sk.rxring=&rx;bt_sk.nexus=(void *)1;
    assert(qbt_transport_active());
    // The former dangling if(prev) leaked this lock on every empty sync.
    for(unsigned i=0;i<100;i++){assert(!bt_sk_sync_tx(NULL,NULL,&tx,0));assert(!held);assert(!qbt_transport_tx_pending());}
    uint8_t data[8192];assert(!qbt_transport_tx_take(data,sizeof(data)) && !held);
    struct packet a={.n=2048,.limit=4096};memset(a.data,0xa5,a.n);
    tx.count=5;for(unsigned i=0;i<tx.count;i++)tx.slots[i].packet=&a;
    assert(!bt_sk_sync_tx(NULL,NULL,&tx,0));assert(tx.advanced==4 && qbt_transport_tx_pending()==8192);
    assert(qbt_transport_tx_take(data,4096)==4096 && tx.notifications==1);
    for(unsigned i=0;i<4096;i++)assert(data[i]==0xa5);
    assert(!bt_sk_sync_tx(NULL,NULL,&tx,0) && tx.advanced==5);
    assert(qbt_transport_tx_take(data,sizeof(data))==6144);
    tx.count=1;tx.advanced=0;a.offset=4097;
    assert(bt_sk_sync_tx(NULL,NULL,&tx,0)==EPROTO && !held && tx.advanced==0);a.offset=0;
    memset(data,0x5a,sizeof(data));assert(qbt_transport_receive(data,sizeof(data))==8192);
    assert(!qbt_transport_receive(data,1));rx.count=4;fail_attach=true;
    assert(!bt_sk_sync_rx(NULL,(void *)1,&rx,0));assert(bt_sk.rx_len==8192 && !packets);
    fail_attach=false;assert(!bt_sk_sync_rx(NULL,(void *)1,&rx,0));assert(!bt_sk.rx_len && rx.advanced==4);
    for(unsigned i=0;i<4;i++){assert(rx.slots[i].packet->n==2048);for(unsigned j=0;j<2048;j++)assert(rx.slots[i].packet->data[j]==0x5a);kern_pbufpool_free(NULL,rx.slots[i].packet);}
    // Pre-disconnect must wait for a notification executing without the lock.
    pause_notify=true;pthread_t reader,closer;assert(!pthread_create(&reader,NULL,receive_thread,NULL));
    pthread_mutex_lock(&notify_mutex);while(!notify_entered)pthread_cond_wait(&notify_cond,&notify_mutex);pthread_mutex_unlock(&notify_mutex);
    assert(!pthread_create(&closer,NULL,close_thread,NULL));
    bool closing=false;for(unsigned i=0;i<1000&&!closing;i++){lck_mtx_lock(&bt_sk_lock);closing=bt_sk.closing;if(closing)assert(bt_sk.busy==1);lck_mtx_unlock(&bt_sk_lock);struct timespec t={0,1000000};nanosleep(&t,NULL);}assert(closing);
    pthread_mutex_lock(&notify_mutex);release_notify=true;pthread_cond_broadcast(&notify_cond);pthread_mutex_unlock(&notify_mutex);
    pthread_join(reader,NULL);pthread_join(closer,NULL);assert(!bt_sk.busy && !qbt_transport_active());
    assert(bt_sk_sync_tx(NULL,NULL,&tx,0)==ENOTCONN && !held);
    assert(qbt_transport_receive(data,32)==32);
    bt_sk_ring_fini(NULL,NULL,&tx);bt_sk_ring_fini(NULL,NULL,&rx);assert(!bt_sk.txring && !bt_sk.rxring);
    assert(!packets);pthread_mutex_destroy(&bt_sk_lock);
    puts("PASS UART.SK empty TX unlock, FIFO backpressure, malformed slot, RX attach rollback, notification/disconnect lifetime");
}
