// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoQrtr.h"
static PicassoTftp t;
static uint8_t p[1024],out[516],saved[516];
static uint32_t from;
static unsigned n;
static void str(const char *s) { unsigned size=unsigned(strlen(s))+1;memcpy(p+n,s,size);n+=size; }
static void request(unsigned op,const char *name="/readwrite/server_check.txt") {
    memset(p,0,sizeof p);p[1]=uint8_t(op);n=2;str(name);str("octet");
}
static void option(const char *a,const char *b) { str(a);str(b); }
static unsigned send(unsigned port=PicassoTftpPort,unsigned peer=73,unsigned node=5) {
    return picasso_tftp_receive(&t,node,peer,port,p,n,out,&from);
}
static void data(unsigned block,unsigned bytes,unsigned seed=0) {
    p[0]=0;p[1]=3;p[2]=uint8_t(block>>8);p[3]=uint8_t(block);n=4+bytes;
    for(unsigned i=0;i<bytes;++i)p[4+i]=uint8_t(seed+i);
}
static void ack(unsigned block) { data(block,0);p[1]=4; }
static void error(unsigned code) { assert(out[0]==0 && out[1]==5 && out[2]==0 && out[3]==code); }
static void cancel(unsigned port,unsigned peer=73) { p[0]=0;p[1]=5;p[2]=0;p[3]=9;n=4;assert(!send(port,peer)); }
static unsigned writeFile(const char *name,unsigned bytes,unsigned peer=73) {
    request(2,name);assert(send(PicassoTftpPort,peer)==4);unsigned port=from;
    unsigned block=1,at=0;
    do { unsigned count=bytes-at;if(count>512)count=512;data(block++,count,at);assert(send(port,peer)==4);at+=count;if(count<512)break; }while(true);
    return port;
}
static void basic() {
    t={};request(1);send();error(1);assert(!t.files[0].used);
    request(2);assert(send()==4 && out[1]==4 && !out[2] && !out[3]);unsigned port=from;
    assert(port>PicassoTftpPort);assert(send()==4 && from==port); // lost ACK0
    data(1,3,10);assert(send(port)==4 && out[3]==1 && t.writes==1);
    assert(t.files[0].used && t.files[0].size==3 && t.files[0].data[2]==12);
    assert(send(port)==4 && t.writes==1); // duplicate final DATA is idempotent
    p[4]++;send(port);error(4);assert(t.writes==1 && t.files[0].data[0]==10);
    request(1);unsigned size=send();assert(size==7 && out[1]==3 && out[3]==1 && out[6]==12);port=from;
    memcpy(saved,out,size);assert(send()==size && from==port && !memcmp(saved,out,size));
    ack(0);assert(!send(port)); // suppress duplicate ACK amplification
    ack(1);assert(!send(port) && t.reads==1);assert(!send(port) && t.reads==1);
    request(1);option("tsize","0");size=send();port=from;
    const uint8_t stat[]={0,6,'t','s','i','z','e',0,'3',0};assert(size==sizeof stat && !memcmp(out,stat,size));cancel(port);
    // RRQ cannot mutate readonly files. Unknown options fail before writes.
    request(1,"/readonly/firmware/image/missing");send();error(1);
    request(2,"/readonly/firmware/image/missing");send();error(2);
    const char *bad[]={"relative","/readwrite/../a","/readwrite/./a","/readwrite//a","/readwrite/a/","/readwrite/a\\b","/"};
    for(auto name:bad) { request(2,name);send();error(2); }
    request(2);option("unlink","1");send();error(8);assert(t.files[0].used);
    request(2);option("blksize","7");send();error(8);
    request(2);option("tsize","4294967296");send();error(8);
    request(2);option("tsize","-1");send();error(8);
    request(2);option("blksize","8");option("BLKSIZE","8");send();error(8);
    request(2);option("timeoutms","1000");option("timeout","1");send();error(8);
    request(2);n--;send();error(4);
    request(2);option("tsize","10");n--;send();error(8);
    request(2);n=513;send();error(4);
    data(1,0);send(0xfffc);error(5);
    request(2);send();port=from;data(1,0);send(port,74);error(5);send(port,73,6);error(5);cancel(port);
}
static void blocks() {
    t={};request(2);option("blksize","7680");option("wsize","10");option("timeoutms","1000");option("tsize","512");
    unsigned size=send(),port=from;
    const uint8_t options[]={0,6,'b','l','k','s','i','z','e',0,'5','1','2',0,'t','i','m','e','o','u','t','m','s',0,'1','0','0','0',0,
        't','s','i','z','e',0,'5','1','2',0,'w','s','i','z','e',0,'1',0};
    assert(size==sizeof options && !memcmp(out,options,size));
    data(2,512);send(port);error(4);assert(!t.files[0].used);
    data(1,512);assert(send(port)==4 && !t.files[0].used && !t.writes);
    assert(send(port)==4 && !t.writes);
    data(2,0);assert(send(port)==4 && t.writes==1 && t.files[0].size==512);
    request(1);option("blksize","512");send();port=from;ack(0);assert(send(port)==516 && out[3]==1);
    for(unsigned i=0;i<512;++i)assert(out[4+i]==uint8_t(i));
    ack(0);assert(!send(port));ack(1);assert(send(port)==4 && out[1]==3 && out[3]==2);
    ack(2);assert(!send(port) && t.reads==1);
    // Bounded read at an offset, stat cancellation and pwrite tail preservation.
    request(1);option("seek","10");option("rsize","3");send();port=from;
    ack(0);assert(send(port)==7 && out[4]==10 && out[6]==12);ack(1);assert(!send(port));
    request(2);option("seek","10");option("tsize","2");send();port=from;data(1,2,99);assert(send(port)==4);
    assert(t.files[0].size==512 && t.files[0].data[9]==9 && t.files[0].data[10]==99 && t.files[0].data[12]==12);
    request(2);option("tsize","3");send();port=from;data(1,2,1);send(port);error(4);
    assert(t.files[0].data[10]==99); // incomplete overwrite never commits
    request(2);option("tsize","4097");send();error(3);
    // Max-sized file and mandatory terminal zero block.
    t={};writeFile("/readwrite/max",4096);assert(t.files[0].size==4096 && t.writes==1);
    request(2,"/readwrite/max");send();port=from;
    for(unsigned i=1;i<=8;++i) { data(i,512,0x55);assert(send(port)==4); }
    data(9,1);send(port);error(3);assert(t.writes==1 && t.files[0].data[0]==0);
}
static void lifecycle() {
    t={};request(2);option("timeoutms","100");send();unsigned port=from;
    for(unsigned i=0;i<4;++i)assert(picasso_tftp_tick(&t)==-1);
    assert(picasso_tftp_tick(&t)==0 && t.retries==1);
    for(unsigned i=0;i<26;++i)picasso_tftp_tick(&t);
    assert(t.expired==1 && !t.files[0].used);data(1,1);send(port);error(5);
    // A second writer may not steal an uncommitted name or file slot.
    t={};request(2);send();port=from;request(2);send(PicassoTftpPort,74);error(3);
    for(unsigned i=1;i<4;++i) { char name[40];snprintf(name,sizeof name,"/readwrite/pending%u",i);request(2,name);assert(send(PicassoTftpPort,73+i)==4); }
    request(2,"/readwrite/fifth");send(PicassoTftpPort,80);error(3);
    cancel(port);request(2,"/readwrite/after-cancel");assert(send()==4);
    picasso_tftp_invalidate(&t);assert(!t.announced);for(const auto &x:t.transfers)assert(!x.used);
    t={};for(unsigned i=0;i<8;++i) { char name[40];snprintf(name,sizeof name,"/readwrite/f%u",i);writeFile(name,1); }
    request(2,"/readwrite/full");send();error(3);
    unsigned next=t.nextPort;picasso_tftp_invalidate(&t);writeFile("/readwrite/new",0);
    assert(t.nextPort>next && !t.files[0].size && t.files[0].used);
    t.nextPort=0xfffd-PicassoTftpPort;request(2,"/readwrite/port-limit");send();error(3);
}
static void put(uint8_t *b,uint32_t v) { for(unsigned i=0;i<4;++i)b[i]=uint8_t(v>>(8*i)); }
static unsigned wire(uint8_t *w,unsigned dp,bool confirm=true,bool v3=false) {
    memset(w,0,1088);unsigned header=v3?16:32;
    if(v3) { w[0]=3;w[1]=1;w[2]=confirm;put(w+4,n);put(w+8,(73<<16)|5);put(w+12,(dp<<16)|1); }
    else { put(w,1);put(w+4,1);put(w+8,5);put(w+12,73);put(w+16,confirm);put(w+20,n);put(w+24,1);put(w+28,dp); }
    memcpy(w+header,p,n);return header+((n+3)&~3U);
}
static void routing() {
    static PicassoQrtr q;uint8_t w[1088];q={};q.helloReceived=true;q.peer=5;q.tftp.announced=true;
    request(2);unsigned bytes=wire(w,PicassoTftpPort);q.count=7;
    assert(picasso_qrtr_receive(&q,w,bytes)==-11 && !q.tftp.requests && !q.received && !q.tftp.nextPort);
    q.count=0;assert(!picasso_qrtr_receive(&q,w,bytes));
    assert(q.count==2 && q.queue[0][1]==7 && q.queue[1][3]==PicassoTftpPort+1 && q.tftp.requests==1);
    unsigned port=q.queue[1][3];data(1,3,9);bytes=wire(w,port,true,true);q.count=7;
    assert(picasso_qrtr_receive(&q,w,bytes)==-11 && !q.tftp.writes);
    q.count=0;assert(!picasso_qrtr_receive(&q,w,bytes) && q.tftp.writes==1 && q.count==2);
    q.count=0;assert(!picasso_qrtr_receive(&q,w,bytes) && q.tftp.writes==1); // retransmitted wire packet
    unsigned requests=q.tftp.requests;q.count=0;w[8]=6;picasso_qrtr_receive(&q,w,bytes);assert(q.tftp.requests==requests);
    w[8]=5;w[12]=2;picasso_qrtr_receive(&q,w,bytes);assert(q.tftp.requests==requests);
    w[12]=1;q.tftp.announced=false;picasso_qrtr_receive(&q,w,bytes);assert(q.tftp.requests==requests);
    q.tftp.announced=true;
    const uint32_t hello[]={1,2,6,0xfffffffe,0,20,1,0xfffffffe,2,0,0,0,0};
    picasso_qrtr_receive(&q,reinterpret_cast<const uint8_t *>(hello),sizeof hello);
    assert(q.peer==6 && !q.tftp.announced && !q.tftp.files[0].used && !q.count);
    q.peer=5;q.tftp.announced=true;request(2);bytes=wire(w,PicassoTftpPort);picasso_qrtr_receive(&q,w,bytes);
    picasso_qrtr_invalidate(&q,-104);assert(!q.tftp.announced && !q.count);for(auto &x:q.tftp.transfers)assert(!x.used);
    // Deterministic malformed corpus, including overlong requests and DATA.
    t={};uint32_t random=1;
    for(unsigned run=0;run<10000;++run) {
        for(auto &v:p) { random=random*1664525+1013904223;v=uint8_t(random>>24); }
        n=random%sizeof p;if(run&1) { p[0]=0;p[1]=uint8_t(1+run%6); }
        assert(send(run&2?PicassoTftpPort:PicassoTftpPort+1)<=sizeof out);
    }
}
int main() {
    basic();blocks();lifecycle();routing();
    puts("PASS TFTP exact OACK, RAM write/readback, EOF, seek, duplicate/foreign packets, limits, timeout, peer loss, QRTR v1/v3 atomic flow control and malformed corpus");
}
