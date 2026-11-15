/* SPDX-License-Identifier: GPL-2.0-only */
#include "qbt_internal.h"
#include <assert.h>
#include <stdlib.h>
struct event { uint64_t at; uint8_t bytes[260]; size_t size, offset; };
struct fixture {
    struct qbt *q; uint64_t now, tx_done;
    struct event events[128]; unsigned allocations, frees, waits;
    unsigned speed, flow_off, flow_on, clock_off, clock_on, commands, firmware_gets, tx_acks;
    size_t write_limit, receive_room, received_length;
    uint8_t received[70000], tx_frame[70000]; unsigned tx_count, tx_need;
    uint8_t patch[1001], nvm[603], downloaded[2048]; size_t downloaded_length, download_size;
    bool progress_acks, progress_loss, speed_reply; unsigned progress_sent; uint64_t progress_delay, speed_settle, speed_command_at;
    unsigned download_mode, failed_opcode; bool missing_replies, missing_wake, board_specific;
    unsigned family, bad_version, reject_segment; bool missing_firmware, captured_version;
    bool nvm_edited, address_written, wake_armed, hardware_fault, clock_fault;
};
static void put16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p,unsigned v){put16(p,v);put16(p+2,v>>16);}
static unsigned le16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static unsigned le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void queue(struct fixture *f,const uint8_t *p,size_t n,uint64_t delay){
    for(unsigned i=0;i<128;i++)if(!f->events[i].size){
        struct event *e=&f->events[i];assert(n<=sizeof(e->bytes));memcpy(e->bytes,p,n);e->size=n;e->offset=0;e->at=f->now+delay;return;
    }assert(!"event overflow");
}
static void complete(struct fixture *f,unsigned op,const uint8_t *p,size_t n){
    if(f->missing_replies)return;
    uint8_t e[260]={4,0x0e,(uint8_t)(n+3),1,(uint8_t)op,(uint8_t)(op>>8)};
    memcpy(e+6,p,n);queue(f,e,n+6,200000);
}
static void controller(struct fixture *f,const uint8_t *p,unsigned n){
    unsigned op=le16(p+1);assert(n==p[3]+4u);f->commands++;
    const uint8_t *param=p+4;uint8_t response[64]={0};size_t length=1;
    if(op==0xfc48){ /* no synchronous completion; a late report may follow the switch */
        f->speed_command_at=f->now;
        if(f->speed_reply){const uint8_t e[]={4,0x0e,4,1,0x48,0xfc,1};queue(f,e,sizeof(e),f->speed_settle/2);}
        return;
    }
    if(op==0xfc00){
        response[1]=param[0];length=2;
        if(param[0]==0x19){
            if(f->captured_version) {
                /* Exact pre-firmware Picasso serial response, including H4. */
                static const uint8_t captured[]={4,0x0e,0x12,1,0,0xfc,0,0x19,0x0c,
                    0x0a,0,0,0,0xe9,0x0d,2,3,0x20,3,1,0x40};
                queue(f,captured,sizeof(captured),200000);return;
            }
            length=f->family==3990?14:15;unsigned at=f->family==3990?2:3;
            response[1]=f->family==3990?2:0x19;response[2]=f->family==3990?0:12;
            put32(response+at,0x21);put16(response+at+4,0x1234);
            put16(response+at+6,f->family?0x0302:0x0200);
            put32(response+at+8,f->family==3991?0x40014320:f->family==3990?0x40010320:0x400c0200);
            if(f->bad_version==1)--length;
            if(f->bad_version==2)response[1]=0x55;
            if(f->bad_version==3)put32(response+at+8,0);
            if(f->bad_version==4)put32(response+at+8,0x400c0200);
            if(f->bad_version==5)put16(response+at+6,0x1302);
            if(f->bad_version==6)response[0]=1;
            if(f->bad_version==7 && f->family!=3990)response[2]=11;
        }
        else if(param[0]==0x23){length=5;if(f->board_specific){response[3]=0x12;response[4]=0x34;}}
        else if(param[0]==0x20){const char *label="WCN7850-fixture";length=strlen(label)+3;response[2]=strlen(label);memcpy(response+3,label,strlen(label));}
        else if(param[0]==0x1e){
            unsigned bytes=param[1];assert(bytes==n-6&&bytes<=243);
            /* A segment that arrives before the controller finished its speed change is lost. */
            if(f->speed_settle)assert(f->now>=f->speed_command_at+f->speed_settle);
            if(!f->downloaded_length){assert(bytes>=28);f->download_size=(le32(param+2)>>8)+4;f->download_mode=param[2]==1?param[16]:0;}
            assert(f->downloaded_length+bytes<=sizeof(f->downloaded));memcpy(f->downloaded+f->downloaded_length,param+2,bytes);f->downloaded_length+=bytes;
            if(f->downloaded_length==f->download_size){
                if(f->downloaded[0]==2){
                    const uint8_t *tag=f->downloaded+4;assert(le16(tag)==17);assert((tag[12]&0x80)&&tag[f->family==3990?14:13]==(f->family?14:17));
                    tag+=15;assert(le16(tag)==27&&(tag[12]&1));f->nvm_edited=true;
                }f->downloaded_length=0;
            } else if(f->download_mode==1||f->download_mode==3){
                if(f->progress_acks){
                    /* Stale reports arrive behind the bytes the host is sending. */
                    static const uint8_t status[]={0x10,0x11,0x10,0x01};
                    uint8_t ack[]={4,0x0e,5,1,0,0xfc,status[f->progress_sent%4],0x1e};
                    f->progress_sent++;
                    if(!f->progress_loss)queue(f,ack,sizeof(ack),f->progress_delay?f->progress_delay:50000);
                }
                return;
            }
        }
    } else if(op==0xfc14){const uint8_t expected[]={2,2,3,4,5,7};assert(!memcmp(param,expected,6));f->address_written=true;}
    else if(op==0x1009){length=7;memset(response+1,0x42,6);}
    if(f->failed_opcode==op)response[0]=1;
    if(f->family==3990 && op==0xfc00 && (param[0]==0x19 || param[0]==0x1e)) {
        if(param[0]==0x1e){response[1]=4;response[2]=f->reject_segment?1:0;length=3;}
        if(!f->missing_replies){
            /* Legacy ROM acknowledges first, then sends its vendor payload. */
            uint8_t status=0;complete(f,op,&status,1);
            uint8_t vendor[260]={4,0xff,(uint8_t)length};
            memcpy(vendor+3,response,length);queue(f,vendor,length+3,300000);
        }
    } else {
        if(f->reject_segment && op==0xfc00 && param[0]==0x1e)response[0]=1;
        complete(f,op,response,length);
    }
}
static void tx_byte(struct fixture *f,uint8_t b){
    if(!f->tx_count){
        if(b==0xfd){if(!f->missing_wake){uint8_t ack=0xfc;queue(f,&ack,1,200000);}return;}
        if(b==0xfc){f->tx_acks++;return;}
        if(b==0xfe)return;
        assert(b==1||b==2);f->tx_need=b==1?4:5;
    }
    assert(f->tx_count<sizeof(f->tx_frame));f->tx_frame[f->tx_count++]=b;
    if(f->tx_count==(f->tx_frame[0]==1?4u:5u))f->tx_need+=f->tx_frame[0]==1?f->tx_frame[3]:le16(f->tx_frame+3);
    if(f->tx_count==f->tx_need){if(f->tx_frame[0]==1)controller(f,f->tx_frame,f->tx_count);f->tx_count=0;}
}
static void *allocate(void *c,size_t n){struct fixture *f=c;f->allocations++;return calloc(1,n);}
static void release(void *c,void *p,size_t n){(void)n;((struct fixture *)c)->frees++;free(p);}
static uint64_t now(void *c){return ((struct fixture *)c)->now;}
static bool drained(void *c){struct fixture *f=c;return f->now>=f->tx_done;}
static size_t write_uart(void *c,const uint8_t *p,size_t n){
    struct fixture *f=c;if(!drained(c))return 0;if(n>f->write_limit)n=f->write_limit;
    f->tx_done=f->now+100000;for(size_t i=0;i<n;i++)tx_byte(f,p[i]);return n;
}
static int speed(void *c,uint32_t n){struct fixture *f=c;assert(drained(c));f->speed=n;return 0;}
static int flow(void *c,bool on){struct fixture *f=c;if(on)f->flow_on++;else f->flow_off++;return 0;}
static int clock_vote(void *c,bool on){struct fixture *f=c;if(on){if(f->clock_fault)return -ENXIO;f->clock_on++;}else{if(!f->wake_armed)return -EBUSY;assert(drained(c));f->clock_off++;}return 0;}
static size_t receive(void *c,const uint8_t *p,size_t n){
    struct fixture *f=c;if(n>f->receive_room)n=f->receive_room;assert(n<=sizeof(f->received)-f->received_length);
    memcpy(f->received+f->received_length,p,n);f->received_length+=n;f->receive_room-=n;return n;
}
static void pump(void *c){
    struct fixture *f=c;
    if(f->hardware_fault){qbt_fail(f->q,-EIO);return;}
    /* The wire is ordered: finish the earliest due event before starting another. */
    for(;;){
        struct event *e=NULL;
        for(unsigned i=0;i<128;i++){
            struct event *c=&f->events[i];if(!c->size||c->at>f->now)continue;
            if(c->offset){e=c;break;}
            if(!e||c->at<e->at)e=c;
        }
        if(!e)return;
        size_t n=e->size-e->offset;if(n>3)n=3; /* every reply spans parser calls */
        size_t used=qbt_receive(f->q,e->bytes+e->offset,n);
        e->offset+=used;if(e->offset==e->size)e->size=0;
        if(!used)return;
    }
}
static void wait_until(void *c,uint64_t deadline){
    struct fixture *f=c;assert(++f->waits<100000);
    uint64_t next=deadline;
    if(f->tx_done>f->now&&f->tx_done<next)next=f->tx_done;
    for(unsigned i=0;i<128;i++)if(f->events[i].size){
        uint64_t at=f->events[i].at;if(at<=f->now)at=f->now+1000;if(at<next)next=at;
    }
    assert(next>=f->now);f->now=next;
}
static int firmware(void *c,const char *name,const uint8_t **p,size_t *n){
    struct fixture *f=c;f->firmware_gets++;
    if(f->family){
        if(f->missing_firmware)return -ENOENT;
        if(!strcmp(name,"qca/crbtfw32.tlv")){*p=f->patch;*n=sizeof(f->patch);return 0;}
        assert(!strcmp(name,(f->family==3991 && !f->captured_version)?"qca/crnv32u.bin":"qca/crnv32.bin"));
        *p=f->nvm;*n=sizeof(f->nvm);return 0;
    }
    if(!strcmp(name,"qca/hmtbtfw20.tlv")){*p=f->patch;*n=sizeof(f->patch);return 0;}
    if(!strcmp(name,"qca/hmtnv20.bin")){*p=f->nvm;*n=sizeof(f->nvm);return 0;}
    assert(!strcmp(name,"qca/hmtnv20.b1234"));return -ENOENT;
}
static void firmware_put(void *c,const uint8_t *p,size_t n){(void)c;(void)p;(void)n;}
static void make(struct fixture *f,unsigned mode){
    memset(f,0,sizeof(*f));f->now=1000000000;f->write_limit=31;f->receive_room=sizeof(f->received);f->wake_armed=true;
    put32(f->patch,((sizeof(f->patch)-4)<<8)|1);f->patch[14]=mode;
    put32(f->nvm,((sizeof(f->nvm)-4)<<8)|2);uint8_t *tag=f->nvm+4;
    put16(tag,17);put16(tag+2,3);tag+=15;put16(tag,27);put16(tag+2,1);tag+=13;
    put16(tag,2);put16(tag+2,6);memset(tag+12,0x42,6);tag+=18;
    put16(tag,99);put16(tag+2,(unsigned)(f->nvm+sizeof(f->nvm)-tag-12));
    struct qbt_platform p={.context=f,.allocate=allocate,.release=release,.now_ns=now,.uart_write=write_uart,.uart_drained=drained,.uart_speed=speed,.uart_flow=flow,.clock=clock_vote,.pump=pump,.wait_until=wait_until,.receive=receive,.firmware_get=firmware,.firmware_put=firmware_put};
    f->q=qbt_create(&p);assert(f->q);
}
static int boot(struct fixture *f){const uint8_t address[]={2,2,3,4,5,7};return qbt_boot(f->q,3200000,address);}
static void destroy(struct fixture *f){qbt_destroy(f->q);assert(f->allocations==f->frees);}
static void drive(struct fixture *f,unsigned steps){for(unsigned i=0;i<steps;i++){pump(f);qbt_step(f->q);if(f->q->info.state==3)return;uint64_t d=qbt_deadline(f->q);if(d==UINT64_MAX){if(drained(f))return;d=f->tx_done;}if(!d)d=f->now+1000;wait_until(f,d);}}
static void initialization(void){
    for(unsigned mode=0;mode<=3;mode+=3){struct fixture *f=calloc(1,sizeof(*f));make(f,mode);f->board_specific=true;
        assert(!boot(f));struct qbt_info info;qbt_get_info(f->q,&info);assert(info.state==2&&info.baud==3200000&&info.rom==0x0200&&!strcmp(info.firmware,"WCN7850-fixture"));
        assert(f->firmware_gets==3&&f->nvm_edited&&f->address_written&&f->speed==3200000&&f->flow_off==1&&f->flow_on==1);
        assert(!qbt_shutdown(f->q));destroy(f);free(f);
    }
}
/* WCN7850 reports every skipped TLV segment with status 0x10 in Command Complete.
 * Those reports must not answer the wait on the final segment. */
static void progress_reports(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);f->progress_acks=true;f->progress_delay=400000;
    assert(!boot(f));struct qbt_info info;qbt_get_info(f->q,&info);assert(info.state==2&&f->address_written);
    struct qbt_stats stats;qbt_get_stats(f->q,&stats);
    assert(f->progress_sent>0&&stats.tlv_sent==stats.tlv_acks);
    destroy(f);
    /* Missing reports for skipped segments do not delay the final status. */
    make(f,3);f->progress_acks=true;f->progress_loss=true;
    assert(!boot(f));
    destroy(f);free(f);
}
/* WCN7850 changes speed after the host could already be sending. The first TLV
 * segment holds the download mode, so it must wait for the controller. */
static void speed_settle(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);f->speed_settle=4000000;f->speed_reply=true;
    assert(!boot(f));assert(f->address_written);destroy(f);
    make(f,3);f->speed_settle=50000000;
    assert(!boot(f));assert(f->address_written);destroy(f);free(f);
}
static void failures(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);f->failed_opcode=0xfc17;assert(boot(f)==-EIO&&f->q->info.state==3);
    uint8_t packet[]={1,3,12,0};assert(qbt_transmit(f->q,packet,sizeof(packet))==-ENETDOWN);destroy(f);
    make(f,0);f->missing_replies=true;uint64_t start=f->now;assert(boot(f)==-ETIMEDOUT);assert(f->now-start==QBT_COMMAND_TIMEOUT&&f->waits<10);destroy(f);
    make(f,0);f->hardware_fault=true;start=f->now;assert(boot(f)==-EIO && f->now==start && !f->waits);destroy(f);free(f);
}
static void receive_pressure(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);assert(!boot(f));qbt_set_service(f->q,true);
    uint8_t packet[]={2,1,0,6,0,0xfd,0xfc,0xfe,1,2,3};f->receive_room=0;
    size_t used=qbt_receive(f->q,packet,sizeof(packet));assert(used==5);assert(qbt_receive(f->q,packet+used,sizeof(packet)-used)==0);
    f->receive_room=sizeof(f->received);qbt_step(f->q);assert(qbt_receive(f->q,packet+used,sizeof(packet)-used)==sizeof(packet)-used);
    assert(f->received_length==sizeof(packet)&&!memcmp(f->received,packet,sizeof(packet)));
    uint8_t e[]={4,0x0e,4,1,3,12,0};for(unsigned i=0;i<sizeof(e);i++)assert(qbt_receive(f->q,e+i,1)==1);
    assert(f->received_length==sizeof(packet)+sizeof(e)&&!memcmp(f->received+sizeof(packet),e,sizeof(e)));
    /* A channel reopened during an RX frame must not inherit its old bytes. */
    f->received_length=0;
    assert(qbt_receive(f->q,packet,4)==4);qbt_set_service(f->q,false);qbt_set_service(f->q,true);
    assert(qbt_receive(f->q,packet+4,sizeof(packet)-4)==sizeof(packet)-4);assert(!f->received_length);
    assert(qbt_receive(f->q,packet,sizeof(packet))==sizeof(packet));assert(f->received_length==sizeof(packet));
    destroy(f);free(f);
}
static void sleep_and_wake(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);assert(!boot(f));qbt_set_service(f->q,true);drive(f,50);assert(!f->q->clock_on&&f->clock_off);
    uint8_t acl[205]={2,1,0,200,0};assert(!qbt_transmit(f->q,acl,sizeof(acl)));drive(f,80);assert(!f->q->tx_length&&!f->tx_count&&f->clock_on);
    qbt_rx_wake(f->q);uint8_t wake=0xfd;assert(qbt_receive(f->q,&wake,1)==1);qbt_step(f->q);assert(f->q->rx_awake&&f->q->clock_on);
    uint8_t asleep=0xfe;qbt_receive(f->q,&asleep,1);drive(f,50);assert(!f->q->clock_on);destroy(f);free(f);
}
static void lost_wake(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);assert(!boot(f));qbt_set_service(f->q,true);drive(f,40);f->missing_wake=true;
    uint8_t acl[]={2,1,0,1,0,0};assert(!qbt_transmit(f->q,acl,sizeof(acl)));drive(f,60);assert(f->q->info.state==3&&f->q->info.error==-ETIMEDOUT);destroy(f);free(f);
}
static void clock_failure(void){
    struct fixture *f=calloc(1,sizeof(*f));
    for(unsigned path=0;path<3;path++) {
        make(f,3);assert(!boot(f));qbt_set_service(f->q,true);drive(f,50);
        assert(!f->q->clock_on);f->clock_fault=true;
        unsigned commands=f->commands,acks=f->tx_acks;
        uint8_t acl[]={2,1,0,1,0,0},wake=0xfd;
        if(path==0){assert(!qbt_transmit(f->q,acl,sizeof(acl)));qbt_step(f->q);}
        else if(path==1)qbt_rx_wake(f->q);
        else assert(qbt_receive(f->q,&wake,1)==1);
        assert(f->q->info.state==3 && f->q->info.error==-ENXIO && !f->q->service);
        assert(qbt_deadline(f->q)==UINT64_MAX && !qbt_transmit_capacity(f->q));
        assert(!qbt_receive(f->q,&wake,1));qbt_step(f->q);
        assert(f->commands==commands && f->tx_acks==acks);
        destroy(f);
    }
    free(f);
}
static void tx_ibs_boundaries(void){
    struct fixture *f=calloc(1,sizeof(*f));make(f,3);assert(!boot(f));qbt_set_service(f->q,true);
    uint8_t acl[4101]={2,1,0,0,16};memset(acl+5,0xfc,sizeof(acl)-5);
    assert(!qbt_transmit(f->q,acl,20));drive(f,40);
    assert(f->q->tx_frame.type==2 && f->tx_count==20);
    /* Empty host queue at a partial frame must neither sleep nor busy-spin. */
    f->now+=QBT_TX_IDLE;qbt_step(f->q);assert(f->q->tx_state==QBT_AWAKE);
    assert(qbt_deadline(f->q)==UINT64_MAX);
    unsigned acks=f->tx_acks;uint8_t wake=0xfd;qbt_receive(f->q,&wake,1);
    qbt_step(f->q);assert(f->tx_acks==acks);
    assert(!qbt_transmit(f->q,acl+20,sizeof(acl)-20));drive(f,500);
    assert(!f->q->tx_length && !f->tx_count && !f->q->tx_frame.type);
    assert(f->tx_acks==acks+1 && !memcmp(f->tx_frame,acl,sizeof(acl)));
    assert(!qbt_transmit(f->q,acl,10));drive(f,5);qbt_set_service(f->q,false);
    assert(f->q->info.state==3 && f->q->info.error==-ECANCELED);
    destroy(f);free(f);
}
static void wcn399x(void) {
    struct fixture *f=calloc(1,sizeof(*f));const uint8_t address[]={2,2,3,4,5,7};
    for(unsigned family=3990;family<=3991;++family) {
        for(unsigned mode=0;mode<=3;mode+=3) {
            make(f,mode);f->family=family;
            assert(!qbt_boot_wcn399x(f->q,3000000,address));
            assert(f->q->info.state==2 && f->q->info.rom==0x0302 && f->firmware_gets==2);
            assert(f->address_written && f->nvm_edited && f->speed==3000000);
            assert(!qbt_shutdown(f->q));
            qbt_reset(f->q);assert(!qbt_boot_wcn399x(f->q,3000000,address));
            destroy(f);
        }
        for(unsigned bad=1;bad<=(family==3990?6u:7u);++bad) {
            make(f,0);f->family=family;f->bad_version=bad;
            assert(qbt_boot_wcn399x(f->q,3000000,address)<0 && !f->firmware_gets && !f->address_written);
            destroy(f);
        }
        make(f,0);f->family=family;f->reject_segment=1;
        int rejected=qbt_boot_wcn399x(f->q,3000000,address);
        assert((family==3990?rejected==-EIO:rejected<0) && !f->address_written);
        destroy(f);
        make(f,0);f->family=family;f->missing_firmware=true;
        assert(qbt_boot_wcn399x(f->q,3000000,address)==-ENOENT && !f->address_written);
        destroy(f);
        make(f,0);f->family=family;f->missing_replies=true;
        assert(qbt_boot_wcn399x(f->q,3000000,address)==-ETIMEDOUT && f->commands==1);
        destroy(f);
    }
    make(f,0);f->family=3991;f->captured_version=true;
    assert(!qbt_boot_wcn399x(f->q,3000000,address));
    assert(f->q->info.product==0x0a && f->q->info.soc==0x40010320 &&
        f->q->info.rom==0x0302 && f->q->info.patch==0x0de9);
    assert(f->q->info.state==2 && f->firmware_gets==2 && f->address_written && f->nvm_edited);
    assert(!qbt_shutdown(f->q));destroy(f);
    puts("PASS: exact Picasso CC version replay, 12-byte length, SoC-based non-UMC NVM, strict status/length rejection");
    free(f);
    puts("PASS: WCN3990 VSE/3991 CC, ROM-specific NVM and baud edit, rejected TLV, malformed/unknown version, missing firmware/reply, shutdown/restart");
}
int main(void){wcn399x();initialization();failures();progress_reports();speed_settle();receive_pressure();sleep_and_wake();lost_wake();clock_failure();tx_ibs_boundaries();puts("PASS: original Linux WCN7850 setup, firmware/BID fallback, NVM edits, H4 fragmentation/backpressure, status failure, true timeout, partial DMA TX, IBS packet boundaries, clock/wake recovery and terminal clock faults");}
