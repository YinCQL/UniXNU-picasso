/* SPDX-License-Identifier: GPL-2.0-only */
#include "qbt_linux.h"
#include "btqca.h"
#include <stdarg.h>
/* Linux startup runs on the single owning wireless executor. The other
 * adapters receive their hci_dev explicitly; allocation helpers use this scope. */
static struct qbt *startup_owner;
struct allocation { size_t size; struct qbt *q; unsigned char bytes[]; };
void *vmalloc(size_t n) {
    struct qbt *q=startup_owner;
    if (!q || n > SIZE_MAX-sizeof(struct allocation)) return NULL;
    struct allocation *p=q->ops.allocate(q->ops.context,sizeof(*p)+n);
    if (!p) return NULL; p->size=n; p->q=q; return p->bytes;
}
void vfree(void *bytes) {
    if (!bytes) return;
    struct allocation *p=(void *)((unsigned char *)bytes-sizeof(struct allocation));
    p->q->ops.release(p->q->ops.context,p,sizeof(*p)+p->size);
}
void kfree(void *p) { vfree(p); }
char *kstrndup(const void *s,size_t n,int flags) {
    (void)flags; char *out=vmalloc(n+1); if(out) {memcpy(out,s,n);out[n]=0;} return out;
}
size_t strscpy(char *out,const char *s,size_t n) {
    size_t len=strlen(s); if(n) {size_t take=len<n-1?len:n-1;memcpy(out,s,take);out[take]=0;}return len;
}
void msleep(unsigned n) { if(startup_owner) qbt_delay(startup_owner,n); }
struct sk_buff *bt_skb_alloc(size_t n,int flags) {
    (void)flags;
    if (n>300 || !startup_owner) return NULL;
    struct sk_buff *s=vmalloc(sizeof(*s));
    if(s) {memset(s,0,sizeof(*s));s->q=startup_owner;s->data=s->storage;s->capacity=sizeof(s->storage);}return s;
}
void kfree_skb(struct sk_buff *s) { vfree(s); }
void *skb_put(struct sk_buff *s,size_t n) {
    if(n>s->capacity-s->len) return NULL;
    void *p=s->data+s->len;s->len+=n;return p;
}
void skb_put_u8(struct sk_buff *s,u8 v) {u8 *p=skb_put(s,1);if(p)*p=v;}
void *skb_pull_data(struct sk_buff *s,size_t n) {
    if(n>s->len)return NULL;void *p=s->data;s->data+=n;s->len-=n;s->capacity-=n;return p;
}
/* btqca's synthetic completion serves its Linux command scheduler during
 * download. It stays inside startup and never reaches the native transport. */
int hci_recv_frame(struct hci_dev *h,struct sk_buff *s) {(void)h;kfree_skb(s);return 0;}
struct sk_buff *__hci_cmd_sync_ev(struct hci_dev *h,u16 op,unsigned n,const void *p,u8 event,unsigned timeout) {
    (void)timeout;
    struct sk_buff *s=bt_skb_alloc(258,0);if(!s)return ERR_PTR(-ENOMEM);
    size_t length=s->capacity;
    int e=qbt_command(h->q,op,p,n,event==HCI_EV_CMD_COMPLETE?0:event,s->data,&length);
    if (!e && (event == 0 || event == HCI_EV_CMD_COMPLETE) && (!length || s->data[0])) e = -EIO;
    /* Older btqca only logs a failed TLV status. Propagate it here so a
     * rejected firmware segment can never yield a successful bring-up. */
    if(!e && op==EDL_PATCH_CMD_OPCODE && n>=1 && ((const uint8_t *)p)[0]==EDL_PATCH_TLV_REQ_CMD &&
       event==HCI_EV_VENDOR && length==3 && s->data[0]==0 && s->data[1]==EDL_TVL_DNLD_RES_EVT && s->data[2])e=-EIO;
    if(e){kfree_skb(s);return ERR_PTR(e);}s->len=length;return s;
}
struct sk_buff *__hci_cmd_sync(struct hci_dev *h,u16 op,unsigned n,const void *p,unsigned timeout) {
    return __hci_cmd_sync_ev(h,op,n,p,0,timeout);
}
int __hci_cmd_send(struct hci_dev *h,u16 op,unsigned n,const void *p) {return qbt_send_command(h->q,op,p,n);}
int __hci_reset_sync(struct hci_dev *h) {
    struct sk_buff *s=__hci_cmd_sync(h,0x0c03,0,NULL,HCI_INIT_TIMEOUT);
    if(IS_ERR(s))return PTR_ERR(s);int e=s->len==1&&!s->data[0]?0:-EIO;kfree_skb(s);return e;
}
int request_firmware(const struct firmware **out,const char *name,void *device) {
    struct qbt *q=((struct qbt_device *)device)->q;
    struct firmware *f=vmalloc(sizeof(*f));if(!f)return -ENOMEM;
    f->q=q;int e=q->ops.firmware_get(q->ops.context,name,&f->data,&f->size);
    if(e){vfree(f);return e;}*out=f;return 0;
}
void release_firmware(const struct firmware *f) {
    f->q->ops.firmware_put(f->q->ops.context,f->data,f->size);vfree((void *)f);
}
void hci_set_fw_info(struct hci_dev *h,const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vsnprintf(h->q->info.firmware,sizeof(h->q->info.firmware),fmt,ap);va_end(ap);
}
void hci_set_quirk(struct hci_dev *h,unsigned value) {(void)h;(void)value;}
void hci_set_msft_opcode(struct hci_dev *h,unsigned value) {(void)h;(void)value;}
static int set_speed(struct qbt *q,uint32_t speed,uint8_t baud) {
    int e=q->ops.uart_flow(q->ops.context,false);if(e)return e;
    q->speed_reply=false;
    e=qbt_send_command(q,0xfc48,&baud,1);
    if(!e)e=qbt_drain(q,q->ops.now_ns(q->ops.context)+QBT_COMMAND_TIMEOUT);
    if(!e){qbt_delay(q,1);e=q->ops.uart_speed(q->ops.context,speed);}
    int flow=q->ops.uart_flow(q->ops.context,true);if(!e)e=flow;
    /* The first TLV segment holds the download mode and is lost if it precedes the switch. */
    for(unsigned ms=0;!e && !q->speed_reply && ms<QBT_SPEED_REPLY_MS;ms++)qbt_delay(q,1);
    if(!e)qbt_delay(q,QBT_SPEED_SETTLE_MS);
    if(!e)q->info.baud=speed;return e;
}
/* Version discovery sends exactly one command. Select the Linux parser mode
 * from the real response envelope, then validate the controller identity. */
static int wcn399x_version(struct hci_dev *h,struct qca_btsoc_version *v,enum qca_btsoc_type *soc) {
    uint8_t cmd=EDL_PATCH_VER_REQ_CMD,reply[32];size_t length=sizeof(reply);
    int e=qbt_command(h->q,EDL_PATCH_CMD_OPCODE,&cmd,1,QBT_VERSION_EVENT,reply,&length);
    /* Version discovery precedes firmware download and native HCI traffic.
     * Keep a bounded trace of this public controller identity response so a
     * rejected envelope can be compared with the pinned Linux ABI. */
    static unsigned version_samples;
    if(version_samples<16) {
        ++version_samples;
        char bytes[97];unsigned n=h->q->reply_length;
        if(n>32)n=32;
        for(unsigned i=0;i<n;++i)snprintf(bytes+3*i,sizeof(bytes)-3*i,"%02x ",h->q->reply[i]);
        bytes[3*n]=0;
        qbt_linux_log(h->q,3,"PICASSO_BT version-reply sample=%u error=%d event_bytes=%u raw=%s",
            version_samples,e,h->q->reply_length,bytes);
    }
    if(e)return e;
    bool cc=h->q->reply[0]==HCI_EV_CMD_COMPLETE;
    /* The CC EDL header has a version-length byte; status/cresp is reply[0]. */
    unsigned offset=cc?3:2;
    if(length!=offset+sizeof(*v))return -EILSEQ;
    if(reply[0]!=EDL_CMD_REQ_RES_EVT || reply[1]!=(cc?EDL_PATCH_VER_REQ_CMD:EDL_APP_VER_RES_EVT) ||
       (cc && reply[2]!=sizeof(*v)))return -EIO;
    memcpy(v,reply+offset,sizeof(*v));
    if(!v->soc_id || !v->rom_ver)return -EILSEQ;
    // Stock btfm_slim_slave.h identifies Cherokee 3.2 and its UMC variant.
    // These are the only ROMs covered by this board's pinned cr*32 firmware.
    uint32_t id=le32_to_cpu(v->soc_id);
    if(le16_to_cpu(v->rom_ver)!=0x0302 ||
       (id!=0x40010320u && !(cc && id==QCA_WCN3991_SOC_ID)))return -ENODEV;
    /* Picasso's 40010320 ROM also uses the newer command-complete envelope.
     * Select the reply/TLV protocol from that envelope. Upstream selects the
     * UMC NVM variant independently from the actual reported SoC identity. */
    *soc=cc?QCA_WCN3991:QCA_WCN3990;
    qbt_linux_log(h->q,3,"PICASSO_BT version identity product=%x soc=%x rom=%x patch=%x cc=%u",
        le32_to_cpu(v->product_id),id,le16_to_cpu(v->rom_ver),le16_to_cpu(v->patch_ver),cc);
    return 0;
}
static int boot_soc(struct qbt *q,uint32_t speed,const uint8_t address[6],enum qca_btsoc_type soc) {
    if(!address || !(address[0]|address[1]|address[2]|address[3]|address[4]|address[5]) || (address[0]&1))return -EINVAL;
    uint8_t baud;
    switch(speed) {case 115200:baud=QCA_BAUDRATE_115200;break;case 3000000:baud=QCA_BAUDRATE_3000000;break;
    case 3200000:baud=QCA_BAUDRATE_3200000;break;case 4000000:baud=QCA_BAUDRATE_4000000;break;default:return -EINVAL;}
    struct qbt *expected=NULL;
    if(!__atomic_compare_exchange_n(&startup_owner,&expected,q,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -EBUSY;
    struct qbt_platform platform=q->ops;struct qbt_stats stats=q->stats;
    memset(q,0,sizeof(*q));q->ops=platform;q->stats=stats;q->stats.tlv_sent=q->stats.tlv_acks=0;q->clock_on=true;q->info.baud=115200;q->info.state=1;
    struct hci_dev h={.q=q,.dev={.q=q}};struct qca_btsoc_version version;
    int e=soc==QCA_WCN3990?wcn399x_version(&h,&version,&soc):qca_read_soc_version(&h,&version,soc);
    if(!e) {
        q->info.product=version.product_id;q->info.soc=version.soc_id;q->info.rom=version.rom_ver;q->info.patch=version.patch_ver;
        if(speed!=115200)e=set_speed(q,speed,baud);
    }
    if(!e)e=qca_uart_setup(&h,baud,soc,version,NULL,NULL);
    if(!e) {
        q->ibs=true;q->tx_state=QBT_ASLEEP;q->rx_awake=false;
        bdaddr_t local;for(unsigned i=0;i<6;i++)local.b[i]=address[5-i];e=qca_set_bdaddr(&h,&local);
    }
    __atomic_store_n(&startup_owner,NULL,__ATOMIC_RELEASE);q->info.error=e;q->info.state=e?3:2;
    if(e) {q->ibs=false;q->command_length=q->command_offset=0;q->retry_at=q->idle_at=0;}
    else q->stats.resets++;
    return e;
}
int qbt_shutdown(struct qbt *q) {
    struct qbt *expected=NULL;
    if(!__atomic_compare_exchange_n(&startup_owner,&expected,q,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -EBUSY;
    qbt_set_service(q,false);
    struct hci_dev h={.q=q,.dev={.q=q}};
    int e=q->info.state==2?qca_send_pre_shutdown_cmd(&h):0;
    __atomic_store_n(&startup_owner,NULL,__ATOMIC_RELEASE);q->ibs=false;q->waiting=false;
    q->command_length=q->command_offset=q->control_length=0;q->retry_at=q->idle_at=0;
    q->header_need=q->body=0;q->info.state=0;q->info.error=e;return e;
}

int qbt_boot(struct qbt *q,uint32_t speed,const uint8_t address[6]) { return boot_soc(q,speed,address,QCA_WCN7850); }
int qbt_boot_wcn399x(struct qbt *q,uint32_t speed,const uint8_t address[6]) { return boot_soc(q,speed,address,QCA_WCN3990); }
