/* SPDX-License-Identifier: APSL-2.0 */
/* Copyright (c) 2026 daimile2 and contributors.
 * Adapted for Picasso WCN399x and XNU arm64. See qbt_transport.c.license. */
#include "qbt_transport.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <sys/kern_control.h>
#include <sys/sysctl.h>
#include <sys/mbuf.h>
#include <sys/mcache.h>
#include <sys/proc.h>
#include <uuid/uuid.h>
#include <skywalk/os_skywalk.h>
#include <mach/mach_interface.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <kern/sync_sema.h>
#include <kern/task.h>
#define BT_SK_FIFO 8192u
static struct qbt_transport_ops bt_ops;
static uint32_t bt_sk_speed=115200;

static void bt_kick(void)
{
    if(bt_ops.wake)bt_ops.wake(bt_ops.context);
}
struct bt_termios64 {
    uint64_t iflag,oflag,cflag,lflag;
    uint8_t cc[20];
    uint32_t pad;
    uint64_t ispeed,ospeed;
};
static lck_grp_t *bt_sk_grp;
static lck_mtx_t bt_sk_lock;
static struct {
    kern_ctl_ref ctl;
    bool connected,creating,closing;
    kern_nexus_t nexus;
    uint32_t unit,spec[3];
    struct bt_termios64 termios;
    nexus_controller_t ncd;
    uuid_t provider,instance;
    kern_pbufpool_t pool;
    kern_channel_ring_t rxring,txring;
    unsigned busy;
    uint32_t rx_head,rx_len,tx_head,tx_len;
    uint8_t rx[BT_SK_FIFO],tx[BT_SK_FIFO];
    int boot;           /* -1 pending, 0 ready, else errno */
    bool power_pending;
    int power_result;
    char firmware[64];
    uint64_t rx_dropped;
} bt_sk={.boot=-1,.spec={2048,16,16}};

/* Native stack activity: power requests, boot-state reads, control connects, channel requests. */
volatile uint32_t qbt_transport_activity[4];
static inline void bt_sk_count(unsigned index)
{
    (void)__atomic_add_fetch(&qbt_transport_activity[index],1u,__ATOMIC_RELAXED);
}
static void bt_sk_termios_default(void)
{
    memset(&bt_sk.termios,0,sizeof(bt_sk.termios));
    /* The hardware line: 8N1, CREAD, RTS/CTS. */
    bt_sk.termios.cflag=0x300|0x800|0x8000|0x10000|0x20000;
    bt_sk.termios.ispeed=bt_sk.termios.ospeed=bt_sk_speed;
}
static void bt_sk_fifo_put(uint8_t *fifo,uint32_t head,uint32_t *len,const uint8_t *data,uint32_t n)
{
    uint32_t at=(head+*len)%BT_SK_FIFO,first=MIN(n,BT_SK_FIFO-at);
    memcpy(fifo+at,data,first);memcpy(fifo,data+first,n-first);
    *len+=n;
}
static void bt_sk_fifo_get(const uint8_t *fifo,uint32_t *head,uint32_t *len,uint8_t *data,uint32_t n)
{
    uint32_t first=MIN(n,BT_SK_FIFO-*head);
    memcpy(data,fifo+*head,first);memcpy(data+first,fifo,n-first);
    *head=(*head+n)%BT_SK_FIFO;*len-=n;
}
/* Ring notification outside the lock; disconnect drains these first. */
static void bt_sk_notify(kern_channel_ring_t ring)
{
    if(!ring) {lck_mtx_unlock(&bt_sk_lock);return;}
    ++bt_sk.busy;lck_mtx_unlock(&bt_sk_lock);
    (void)kern_channel_notify(ring,0);
    lck_mtx_lock(&bt_sk_lock);
    if(!--bt_sk.busy) wakeup(&bt_sk.busy);
    lck_mtx_unlock(&bt_sk_lock);
}
/* Called with the transport lock held by both sync callbacks. */
static errno_t bt_sk_io_status(void)
{
    if(!bt_sk.connected || bt_sk.closing) return ENOTCONN;
    return bt_sk.boot<0?EAGAIN:bt_sk.boot;
}

/* UART owner side. rx_put returns the bytes taken: all of them while no
 * RX ring exists (discarded), else what fits in the FIFO. */
uint32_t qbt_transport_receive(const uint8_t *data,uint32_t n)
{
    if(!n) return 0;
    lck_mtx_lock(&bt_sk_lock);
    if(!bt_sk.rxring || bt_sk.closing || bt_sk.boot) {
        bt_sk.rx_dropped+=n;lck_mtx_unlock(&bt_sk_lock);return n;
    }
    if(n>BT_SK_FIFO-bt_sk.rx_len) n=BT_SK_FIFO-bt_sk.rx_len;
    if(!n) {lck_mtx_unlock(&bt_sk_lock);return 0;}
    bt_sk_fifo_put(bt_sk.rx,bt_sk.rx_head,&bt_sk.rx_len,data,n);
    bt_sk_notify(bt_sk.rxring);
    return n;
}
uint32_t qbt_transport_tx_pending(void)
{
    lck_mtx_lock(&bt_sk_lock);
    uint32_t n=bt_sk.tx_len;
    lck_mtx_unlock(&bt_sk_lock);
    return n;
}
uint32_t qbt_transport_tx_take(uint8_t *data,uint32_t max)
{
    lck_mtx_lock(&bt_sk_lock);
    uint32_t n=bt_sk.tx_len<max?bt_sk.tx_len:max;
    bt_sk_fifo_get(bt_sk.tx,&bt_sk.tx_head,&bt_sk.tx_len,data,n);
    if(n && bt_sk.txring && !bt_sk.closing) bt_sk_notify(bt_sk.txring);
    else lck_mtx_unlock(&bt_sk_lock);
    return n;
}
bool qbt_transport_active(void)
{
    lck_mtx_lock(&bt_sk_lock);
    bool active=bt_sk.connected && !bt_sk.closing && bt_sk.rxring && bt_sk.txring && !bt_sk.boot;
    lck_mtx_unlock(&bt_sk_lock);return active;
}

/* Kernel pipe provider. */
static errno_t bt_sk_pre_connect(kern_nexus_provider_t prov __unused,proc_t p __unused,kern_nexus_t nexus __unused,
    nexus_port_t port __unused,kern_channel_t channel __unused,void **ctx __unused) {return 0;}
static errno_t bt_sk_connected(kern_nexus_provider_t prov __unused,kern_nexus_t nexus,kern_channel_t channel __unused)
{
    lck_mtx_lock(&bt_sk_lock);
    bool valid=bt_sk.connected && !bt_sk.closing;
    if(valid) bt_sk.nexus=nexus;
    lck_mtx_unlock(&bt_sk_lock);
    bt_kick();return valid?0:ENXIO;
}
/* bluetoothd closes the control socket before destroying its channel when it turns off, so this
 * can arrive after bt_sk_disconnect finished. That channel no longer owns the transport state. */
static void bt_sk_pre_disconnect(kern_nexus_provider_t prov __unused,kern_nexus_t nexus,kern_channel_t channel __unused)
{
    lck_mtx_lock(&bt_sk_lock);
    if(bt_sk.nexus==nexus) bt_sk.closing=true;
    while(bt_sk.busy) (void)msleep(&bt_sk.busy,&bt_sk_lock,PZERO,"btsk",NULL);
    lck_mtx_unlock(&bt_sk_lock);
    bt_kick();
}
static void bt_sk_disconnected(kern_nexus_provider_t prov __unused,kern_nexus_t nexus __unused,kern_channel_t channel __unused) {}
static errno_t bt_sk_ring_init(kern_nexus_provider_t prov __unused,kern_nexus_t nexus __unused,kern_channel_t channel __unused,
    kern_channel_ring_t ring,boolean_t is_tx,void **ctx __unused)
{
    lck_mtx_lock(&bt_sk_lock);
    if(is_tx) bt_sk.txring=ring; else bt_sk.rxring=ring;
    lck_mtx_unlock(&bt_sk_lock);
    bt_kick();
    return 0;
}
static void bt_sk_ring_fini(kern_nexus_provider_t prov __unused,kern_nexus_t nexus __unused,kern_channel_ring_t ring)
{
    lck_mtx_lock(&bt_sk_lock);
    if(bt_sk.txring==ring) bt_sk.txring=NULL;
    if(bt_sk.rxring==ring) bt_sk.rxring=NULL;
    lck_mtx_unlock(&bt_sk_lock);
    bt_kick();
}
/* Host -> controller: whole slots into the TX FIFO; a slot that does not fit
 * stays in the ring until the UART owner drains the FIFO and notifies. */
static errno_t bt_sk_sync_tx(kern_nexus_provider_t prov __unused,kern_nexus_t nexus __unused,kern_channel_ring_t ring,uint32_t flags __unused)
{
    kern_channel_slot_t prev=NULL,slot;
    errno_t error=0;
    lck_mtx_lock(&bt_sk_lock);
    error=bt_sk_io_status();
    if(error) {lck_mtx_unlock(&bt_sk_lock);return error;}
    for(slot=kern_channel_get_next_slot(ring,NULL,NULL);slot && !bt_sk.closing;slot=kern_channel_get_next_slot(ring,slot,NULL)) {
        kern_packet_t ph=kern_channel_slot_get_packet(ring,slot);
        if(ph) {
            kern_buflet_t buf=kern_packet_get_next_buflet(ph,NULL);
            uint32_t n=kern_packet_get_data_length(ph);
            if(!buf || n>bt_sk.spec[0] || kern_buflet_get_data_offset(buf)>kern_buflet_get_data_limit(buf) ||
                n>kern_buflet_get_data_limit(buf)-kern_buflet_get_data_offset(buf)) {
                error=EPROTO;break;
            }
            if(n>BT_SK_FIFO-bt_sk.tx_len) break;
            const uint8_t *data=(const uint8_t *)kern_buflet_get_data_address(buf)+kern_buflet_get_data_offset(buf);
            bt_sk_fifo_put(bt_sk.tx,bt_sk.tx_head,&bt_sk.tx_len,data,n);
        }
        prev=slot;
    }
    if(prev) {kern_channel_advance_slot(ring,prev);(void)kern_channel_reclaim(ring);}
    lck_mtx_unlock(&bt_sk_lock);
    if(prev) bt_kick();
    return error;
}
/* Controller -> host: the RX FIFO into freshly allocated slot packets. */
static errno_t bt_sk_sync_rx(kern_nexus_provider_t prov __unused,kern_nexus_t nexus,kern_channel_ring_t ring,uint32_t flags __unused)
{
    kern_channel_slot_t prev=NULL,slot;kern_pbufpool_t pool=NULL;
    if(kern_nexus_get_pbufpool(nexus,NULL,&pool) || !pool) return 0;
    lck_mtx_lock(&bt_sk_lock);
    errno_t error=bt_sk_io_status();
    if(error) {lck_mtx_unlock(&bt_sk_lock);return error;}
    (void)kern_channel_reclaim(ring);
    for(slot=kern_channel_get_next_slot(ring,NULL,NULL);slot && bt_sk.rx_len && !bt_sk.closing;slot=kern_channel_get_next_slot(ring,slot,NULL)) {
        kern_packet_t ph=0;
        if(kern_pbufpool_alloc_nosleep(pool,1,&ph)) break;
        kern_buflet_t buf=kern_packet_get_next_buflet(ph,NULL);
        uint32_t n=bt_sk.rx_len<bt_sk.spec[0]?bt_sk.rx_len:bt_sk.spec[0];
        if(!buf || !kern_buflet_get_data_address(buf) || n>kern_buflet_get_data_limit(buf)) {
            kern_pbufpool_free(pool,ph);break;
        }
        uint32_t head=bt_sk.rx_head,length=bt_sk.rx_len;
        bt_sk_fifo_get(bt_sk.rx,&head,&length,kern_buflet_get_data_address(buf),n);
        kern_packet_clear_flow_uuid(ph);
        if(kern_buflet_set_data_offset(buf,0) || kern_buflet_set_data_length(buf,n) ||
            kern_packet_finalize(ph) || kern_channel_slot_attach_packet(ring,slot,ph)) {
            kern_pbufpool_free(pool,ph);break;
        }
        bt_sk.rx_head=head;bt_sk.rx_len=length;
        prev=slot;
    }
    if(prev) {kern_channel_advance_slot(ring,prev);}
    lck_mtx_unlock(&bt_sk_lock);
    if(prev) bt_kick();
    return 0;
}

static void bt_sk_destroy(nexus_controller_t ncd,const uuid_t provider,const uuid_t instance,kern_pbufpool_t pool)
{
    if(!uuid_is_null(instance)) (void)kern_nexus_controller_free_provider_instance(ncd,instance);
    if(pool) kern_pbufpool_destroy(pool);
    if(!uuid_is_null(provider)) (void)kern_nexus_controller_deregister_provider(ncd,provider);
    if(ncd) kern_nexus_controller_destroy(ncd);
}
static errno_t bt_sk_create(const uint32_t spec[3],pid_t pid,nexus_controller_t *ncdp,uuid_t provider,uuid_t instance,kern_pbufpool_t *poolp)
{
    nexus_controller_t ncd=NULL;nexus_attr_t attr=NULL;kern_pbufpool_t pool=NULL;uuid_t domain;
    uuid_clear(provider);uuid_clear(instance);
    errno_t e=kern_nexus_controller_create(&ncd);
    if(!e) e=kern_nexus_get_default_domain_provider(NEXUS_TYPE_KERNEL_PIPE,&domain);
    if(!e) e=kern_nexus_attr_create(&attr);
    if(!e) e=kern_nexus_attr_set(attr,NEXUS_ATTR_SLOT_BUF_SIZE,spec[0]);
    if(!e) e=kern_nexus_attr_set(attr,NEXUS_ATTR_TX_SLOTS,spec[1]);
    if(!e) e=kern_nexus_attr_set(attr,NEXUS_ATTR_RX_SLOTS,spec[2]);
    if(!e) {
        struct kern_nexus_provider_init init={
            .nxpi_version=KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,.nxpi_flags=NXPIF_VIRTUAL_DEVICE,
            .nxpi_pre_connect=bt_sk_pre_connect,.nxpi_connected=bt_sk_connected,
            .nxpi_pre_disconnect=bt_sk_pre_disconnect,.nxpi_disconnected=bt_sk_disconnected,
            .nxpi_ring_init=bt_sk_ring_init,.nxpi_ring_fini=bt_sk_ring_fini,
            .nxpi_sync_tx=bt_sk_sync_tx,.nxpi_sync_rx=bt_sk_sync_rx,
        };
        e=kern_nexus_controller_register_provider(ncd,domain,(const uint8_t *)"com.apple.uart.sk.bluetooth",&init,sizeof(init),attr,(uuid_t *)provider);
    }
    if(attr) kern_nexus_attr_destroy(attr);
    if(!e) {
        struct kern_pbufpool_init pp={.kbi_version=KERN_PBUFPOOL_CURRENT_VERSION,.kbi_flags=KBIF_VIRTUAL_DEVICE|KBIF_QUANTUM,
            .kbi_packets=(spec[1]+spec[2])*2,.kbi_bufsize=spec[0],.kbi_max_frags=1};
        (void)snprintf((char *)pp.kbi_name,sizeof(pp.kbi_name),"com.apple.kpipe.uart.bluetooth");
        e=kern_pbufpool_create(&pp,&pool,NULL);
    }
    if(!e) {
        struct kern_nexus_init init={.nxi_version=KERN_NEXUS_CURRENT_VERSION,.nxi_tx_pbufpool=pool};
        e=kern_nexus_controller_alloc_provider_instance(ncd,provider,NULL,NULL,(uuid_t *)instance,&init);
    }
    if(!e) {
        nexus_port_t port=NEXUS_PORT_KERNEL_PIPE_CLIENT;
        e=kern_nexus_controller_bind_provider_instance(ncd,instance,&port,pid,NULL,NULL,0,NEXUS_BIND_PID);
    }
    if(e) {bt_sk_destroy(ncd,provider,instance,pool);return e;}
    *ncdp=ncd;*poolp=pool;
    return 0;
}

/* Kernel control. */
static errno_t bt_sk_connect(kern_ctl_ref ref __unused,struct sockaddr_ctl *sac,void **unitinfo)
{
    lck_mtx_lock(&bt_sk_lock);
    if(bt_sk.connected) {lck_mtx_unlock(&bt_sk_lock);return EBUSY;}
    bt_sk.connected=true;bt_sk.closing=false;bt_sk.unit=sac->sc_unit;*unitinfo=&bt_sk;
    bt_sk_termios_default();
    bool restart=bt_sk.boot>0;
    lck_mtx_unlock(&bt_sk_lock);
    bt_sk_count(2);
    printf("[bluetooth sk] connect unit=%u pid=%d\n",sac->sc_unit,proc_pid(current_proc()));
    /* A client on a stopped controller restarts it; failures reach the client through I/O status. */
    if(restart && bt_ops.power)(void)bt_ops.power(bt_ops.context,true);
    return 0;
}
static errno_t bt_sk_disconnect(kern_ctl_ref ref __unused,u_int32_t unit,void *unitinfo __unused)
{
    lck_mtx_lock(&bt_sk_lock);
    if(!bt_sk.connected || bt_sk.unit!=unit) {lck_mtx_unlock(&bt_sk_lock);return 0;}
    bt_sk.closing=true;
    while(bt_sk.creating) (void)msleep(&bt_sk.creating,&bt_sk_lock,PZERO,"btskclose",NULL);
    while(bt_sk.busy) (void)msleep(&bt_sk.busy,&bt_sk_lock,PZERO,"btskclose",NULL);
    nexus_controller_t ncd=bt_sk.ncd;kern_pbufpool_t pool=bt_sk.pool;uuid_t provider,instance;
    uuid_copy(provider,bt_sk.provider);uuid_copy(instance,bt_sk.instance);
    bt_sk.ncd=NULL;bt_sk.pool=NULL;uuid_clear(bt_sk.provider);uuid_clear(bt_sk.instance);
    lck_mtx_unlock(&bt_sk_lock);
    if(ncd) bt_sk_destroy(ncd,provider,instance,pool);
    lck_mtx_lock(&bt_sk_lock);
    printf("[bluetooth sk] disconnect unit=%u rx_dropped=%llu\n",unit,bt_sk.rx_dropped);
    bt_sk.connected=bt_sk.closing=false;bt_sk.nexus=NULL;
    /* A channel that outlives this session gets no more notifications; its ring_fini finds nothing to clear. */
    bt_sk.rxring=bt_sk.txring=NULL;
    bt_sk.rx_head=bt_sk.rx_len=bt_sk.tx_head=bt_sk.tx_len=0;
    bt_sk.spec[0]=2048;bt_sk.spec[1]=bt_sk.spec[2]=16;
    lck_mtx_unlock(&bt_sk_lock);
    bt_kick();
    return 0;
}
static errno_t bt_sk_send(kern_ctl_ref ref __unused,u_int32_t unit __unused,void *unitinfo __unused,mbuf_t m,int flags __unused)
{
    mbuf_freem(m);
    return EOPNOTSUPP;
}
static errno_t bt_sk_setopt(kern_ctl_ref ref __unused,u_int32_t unit __unused,void *unitinfo __unused,int opt,void *data,size_t len)
{
    if((uint32_t)opt==0x800c5414u) {             /* IOAOSSKYSETCHANNELSPEC */
        if(len!=12) return EINVAL;
        const uint32_t *spec=data;
        lck_mtx_lock(&bt_sk_lock);
        if(!bt_sk.connected || bt_sk.closing) {lck_mtx_unlock(&bt_sk_lock);return ENXIO;}
        if(bt_sk.ncd) {lck_mtx_unlock(&bt_sk_lock);return 0;}
        if(bt_sk.creating) {lck_mtx_unlock(&bt_sk_lock);return EBUSY;}
        if(!spec[0] || (spec[0]&(spec[0]-1)) || spec[0]>4096 || spec[1]-1>=512 || spec[2]-1>=512) {
            lck_mtx_unlock(&bt_sk_lock);return EINVAL;
        }
        memcpy(bt_sk.spec,spec,12);bt_sk.creating=true;
        lck_mtx_unlock(&bt_sk_lock);
        nexus_controller_t ncd=NULL;kern_pbufpool_t pool=NULL;uuid_t provider,instance;
        pid_t pid=proc_pid(current_proc());
        errno_t e=bt_sk_create(spec,pid,&ncd,provider,instance,&pool);
        lck_mtx_lock(&bt_sk_lock);
        bt_sk.creating=false;
        if(!e) {bt_sk.ncd=ncd;bt_sk.pool=pool;uuid_copy(bt_sk.provider,provider);uuid_copy(bt_sk.instance,instance);}
        wakeup(&bt_sk.creating);
        lck_mtx_unlock(&bt_sk_lock);
        bt_sk_count(3);
        printf("[bluetooth sk] channel spec buf=%u slots=%u/%u pid=%d error=%d\n",spec[0],spec[1],spec[2],pid,e);
        return e?ENXIO:0;
    }
    if((uint32_t)opt==0x80487414u) {             /* TIOCSETA */
        if(len!=sizeof(struct bt_termios64)) return EINVAL;
        const struct bt_termios64 *t=data;
        /* The line is fixed at 8N1 RTS/CTS and the operating speed set in the bring-up. */
        bool ok=(t->cflag&(0x300|0x400|0x1000))==0x300 && (t->cflag&0x30000)==0x30000 &&
            t->ispeed==bt_sk_speed && t->ospeed==bt_sk_speed;
        printf("[bluetooth sk] TIOCSETA cflag=%#llx speed=%llu/%llu %s\n",t->cflag,t->ispeed,t->ospeed,ok?"accepted":"unsupported");
        if(!ok) return EINVAL;
        lck_mtx_lock(&bt_sk_lock);bt_sk.termios=*t;lck_mtx_unlock(&bt_sk_lock);
        return 0;
    }
    return ENOPROTOOPT;
}
static errno_t bt_sk_getopt(kern_ctl_ref ref __unused,u_int32_t unit __unused,void *unitinfo __unused,int opt,void *data,size_t *len)
{
    errno_t e=0;
    lck_mtx_lock(&bt_sk_lock);
    if((uint32_t)opt==0x400c5413u) {              /* channel spec */
        if(*len<12) e=EINVAL; else {memcpy(data,bt_sk.spec,12);*len=12;}
    } else if((uint32_t)opt==0x40105412u) {       /* nexus UUID */
        if(*len<sizeof(uuid_t)) e=EINVAL;
        else if(!bt_sk.ncd) e=ENXIO;
        else {memcpy(data,bt_sk.instance,sizeof(uuid_t));*len=sizeof(uuid_t);}
    } else if((uint32_t)opt==0x40487413u) {       /* TIOCGETA */
        if(*len<sizeof(struct bt_termios64)) e=EINVAL;
        else {memcpy(data,&bt_sk.termios,sizeof(struct bt_termios64));*len=sizeof(struct bt_termios64);}
    } else e=ENOPROTOOPT;
    lck_mtx_unlock(&bt_sk_lock);
    return e;
}

static int bt_sk_boot_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int error=0,value;
    bt_sk_count(1);
    lck_mtx_lock(&bt_sk_lock);
    struct timespec timeout={30,0};
    while(bt_sk.boot<0 && !error) error=msleep(&bt_sk.boot,&bt_sk_lock,PZERO|PCATCH,"btboot",&timeout);
    value=bt_sk.boot;
    lck_mtx_unlock(&bt_sk_lock);
    return error?error:SYSCTL_OUT(req,&value,sizeof(value));
}
SYSCTL_OID_MANUAL(_kern,OID_AUTO,qbt_boot,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,bt_sk_boot_sysctl,"I","WCN399x kernel bring-up result");
SYSCTL_OID_MANUAL(_kern,OID_AUTO,qbt_firmware,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED,
    bt_sk.firmware,sizeof(bt_sk.firmware),sysctl_handle_string,"A","WCN399x firmware build label");

/* BlueTool's sandbox denies sysctl writes, so reading this value requests startup
 * and waits for the worker's result. The worker keeps a running controller as it is. */
static int bt_sk_power_on_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int error=0,result=0;
    if(req->newptr)return EPERM;
    if(!req->oldptr)return SYSCTL_OUT(req,&result,sizeof(result));
    bt_sk_count(0);
    lck_mtx_lock(&bt_sk_lock);
    if(bt_sk.power_pending || !bt_ops.power){lck_mtx_unlock(&bt_sk_lock);return EBUSY;}
    bt_sk.power_pending=true;bt_sk.power_result=EINPROGRESS;
    lck_mtx_unlock(&bt_sk_lock);
    error=bt_ops.power(bt_ops.context,true);
    lck_mtx_lock(&bt_sk_lock);
    if(error){bt_sk.power_pending=false;bt_sk.power_result=error;}
    struct timespec timeout={30,0};
    while(bt_sk.power_pending && !error)error=msleep(&bt_sk.power_pending,&bt_sk_lock,PZERO|PCATCH,"btpower",&timeout);
    result=bt_sk.power_result;lck_mtx_unlock(&bt_sk_lock);
    return error?error:SYSCTL_OUT(req,&result,sizeof(result));
}
SYSCTL_OID_MANUAL(_kern,OID_AUTO,qbt_power_on,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,bt_sk_power_on_sysctl,"I","WCN399x startup request");

/* Bounded boot diagnostics: status and length only, never root-key bytes.
 * The daemon retains its native Keychain operations and error handling. */
static int bt_sk_keychain_trace_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    uint32_t trace[4]={0};
    if(!req->newptr)return EPERM;
    char name[32]={0};
    if(!req->p)return EPERM;
    proc_name(proc_getpid(req->p),name,sizeof(name));
    if(strcmp(name,"bluetoothd"))return EPERM;
    if(req->newlen!=sizeof(trace))return EINVAL;
    int error=SYSCTL_IN(req,trace,sizeof(trace));if(error)return error;
    if(trace[0]<1 || trace[0]>2 || trace[1]>16 || trace[3]>65536)return EINVAL;
    static unsigned samples;
    unsigned n=__atomic_fetch_add(&samples,1u,__ATOMIC_RELAXED);
    if(n<32)printf("PICASSO_BT keychain operation=%u kind=%u status=%d bytes=%u sample=%u\n",
        trace[0],trace[1],(int32_t)trace[2],trace[3],n+1);
    return 0;
}
SYSCTL_OID_MANUAL(_kern,OID_AUTO,qbt_keychain_trace,CTLTYPE_OPAQUE|CTLFLAG_WR|CTLFLAG_ANYBODY|CTLFLAG_LOCKED,NULL,0,bt_sk_keychain_trace_sysctl,"S,bt_keychain_trace","Bluetooth keychain boot status");

int qbt_transport_init(const struct qbt_transport_ops *ops)
{
    if(!ops || !ops->wake || !ops->power)return EINVAL;
    /* These objects are linked after the base XNU startup table was built.
     * Register explicitly from the one late executor, before publishing OIDs. */
    if(bt_sk_grp)return EALREADY;
    bt_sk_grp=lck_grp_alloc_init("picasso.bt.uart.sk",LCK_GRP_ATTR_NULL);
    if(!bt_sk_grp)return ENOMEM;
    lck_mtx_init(&bt_sk_lock,bt_sk_grp,LCK_ATTR_NULL);
    lck_mtx_lock(&bt_sk_lock);
    if(bt_ops.wake){lck_mtx_unlock(&bt_sk_lock);return EALREADY;}
    bt_ops=*ops;lck_mtx_unlock(&bt_sk_lock);
    sysctl_register_oid(&sysctl__kern_qbt_boot);
    sysctl_register_oid(&sysctl__kern_qbt_firmware);
    sysctl_register_oid(&sysctl__kern_qbt_power_on);
    sysctl_register_oid(&sysctl__kern_qbt_keychain_trace);
    return 0;
}

/* Called once by the UART owner: 0 after a complete bring-up (with the
 * UART speed it configured), else errno. */
void qbt_transport_power_result(bool enabled,int error,const char *firmware,uint32_t speed)
{
    if(enabled && !error && !bt_sk.ctl) {
        bt_sk_speed=speed;
        struct kern_ctl_reg reg={.ctl_flags=CTL_FLAG_REG_SOCK_STREAM,
            .ctl_connect=bt_sk_connect,.ctl_disconnect=bt_sk_disconnect,.ctl_send=bt_sk_send,
            .ctl_setopt=bt_sk_setopt,.ctl_getopt=bt_sk_getopt};
        strlcpy(reg.ctl_name,"com.apple.uart.sk.bluetooth",sizeof(reg.ctl_name));
        error=ctl_register(&reg,&bt_sk.ctl);
        printf("[bluetooth sk] ctl_register com.apple.uart.sk.bluetooth error=%d firmware=%s speed=%u\n",error,firmware,speed);
    }
    lck_mtx_lock(&bt_sk_lock);
    if(firmware) strlcpy(bt_sk.firmware,firmware,sizeof(bt_sk.firmware));
    if(enabled && !error){bt_sk_speed=speed;bt_sk_termios_default();}
    bt_sk.boot=error?error:enabled?0:ENETDOWN;
    if(!enabled || error)bt_sk.rx_head=bt_sk.rx_len=bt_sk.tx_head=bt_sk.tx_len=0;
    bt_sk.power_pending=false;bt_sk.power_result=error;
    kern_channel_ring_t rx=NULL,tx=NULL;
    if(bt_sk.boot && bt_sk.connected && !bt_sk.closing) {
        rx=bt_sk.rxring;tx=bt_sk.txring;
        if(rx || tx) ++bt_sk.busy;
    }
    lck_mtx_unlock(&bt_sk_lock);
    wakeup(&bt_sk.boot);
    wakeup(&bt_sk.power_pending);
    /* Wake blocked channel users so their next sync observes the error. The
     * busy reference keeps both rings alive until notifications finish. */
    if(rx) (void)kern_channel_notify(rx,0);
    if(tx && tx!=rx) (void)kern_channel_notify(tx,0);
    if(rx || tx) {
        lck_mtx_lock(&bt_sk_lock);
        if(!--bt_sk.busy) wakeup(&bt_sk.busy);
        lck_mtx_unlock(&bt_sk_lock);
    }
    bt_kick();
}
void qbt_transport_boot_result(int error,const char *firmware,uint32_t speed)
{
    qbt_transport_power_result(true,error,firmware,speed);
}
