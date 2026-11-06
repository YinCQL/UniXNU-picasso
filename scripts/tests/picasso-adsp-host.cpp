// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the actual boot probe against synthetic SMEM and owned API stand-ins.
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "PicassoAdsp.h"
static uint8_t memory[WLAN_SMEM_SIZE];
static unsigned reads,change,fail_read,allocations,mappings,releases,unmaps,lines;
static bool fail_alloc,fail_map;
static void p16(uint32_t at,uint16_t v){memory[at]=(uint8_t)v;memory[at+1]=(uint8_t)(v>>8);}
static void p32(uint32_t at,uint32_t v){p16(at,(uint16_t)v);p16(at+2,(uint16_t)(v>>16));}
static int read(void *,uint32_t at,void *out,uint32_t n){
    assert(at<=sizeof memory && n<=sizeof memory-at);++reads;
    if(fail_read==reads)return -5;
    memcpy(out,memory+at,n);
    if(change && at<=change && change-at<n)memory[change]^=8;
    return 0;
}
static const uint32_t toc=WLAN_SMEM_SIZE-4096,part=0x1000,global=0x20000;
static void partition(unsigned i,uint32_t off,uint32_t size,uint16_t a,uint16_t b){
    uint32_t e=toc+32+48*i;p32(e,off);p32(e+4,size);p16(e+12,a);p16(e+14,b);p32(e+16,64);
    p32(off,0x54525024);p16(off+4,a);p16(off+6,b);p32(off+8,size);p32(off+12,32);p32(off+16,size);
}
static uint32_t item(uint32_t p,uint16_t id,uint32_t n){
    uint32_t at=wlan_sm32(memory+p+12),size=(n+7)&~7u;
    p16(p+at,0xa5a5);p16(p+at+2,id);p32(p+at+4,size);p16(p+at+8,(uint16_t)(size-n));
    p32(p+12,at+16+size);return p+at+16;
}
static void smp(uint32_t at,uint16_t local,uint16_t remote,const char *name,uint32_t v){
    p32(at,0x504d5324);memory[at+4]=1;p16(at+8,local);p16(at+10,remote);p16(at+12,16);p16(at+14,1);
    memcpy(memory+at+20,name,strlen(name)+1);p32(at+36,v);
}
static uint32_t desc,crash,inbound;
static void setup(){
    memset(memory,0,sizeof memory);reads=change=fail_read=0;
    p32(0x5c,12u<<16);p32(toc,0x434f5424);p32(toc+4,1);p32(toc+8,2);
    partition(0,part,0x10000,0,2);partition(1,global,0x1000,0xfffe,0xfffe);
    inbound=item(part,443,340);smp(inbound,2,0,"slave-kernel",14);
    smp(item(part,429,340),0,2,"master-kernel",1);
    desc=item(part,478,32);item(part,479,16384);item(part,480,16384);
    p32(desc,8);p32(desc+4,16);p32(desc+8,24);p32(desc+12,32);
    crash=item(global,423,256);memcpy(memory+crash,"previous\ncrash",15);
}
static void parser_tests(){
    wlan_smem_io io={nullptr,read};picasso_adsp_snapshot s;
    setup();picasso_adsp_survey(&io,&s);
    assert(!s.partition_error && !s.inbound_error && !s.outbound_error && !s.glink_error && !s.crash_error);
    assert(s.inbound==14 && s.outbound==1 && s.fifo[3]==32 && !strcmp(s.crash,"previous.crash"));
    unsigned max=reads;
    for(unsigned i=1;i<=max;++i){setup();fail_read=i;picasso_adsp_survey(&io,&s);
        assert(s.partition_error || s.inbound_error || s.outbound_error || s.glink_error || s.crash_error);}
    setup();p16(inbound+8,1);picasso_adsp_survey(&io,&s);assert(s.inbound_error==-22 && !s.inbound);
    setup();memory[inbound+20]='X';picasso_adsp_survey(&io,&s);assert(s.inbound_error==-2);
    setup();p32(desc+4,16384);picasso_adsp_survey(&io,&s);assert(s.glink_error==-22 && !s.fifo[0]);
    setup();p32(desc+4,3);picasso_adsp_survey(&io,&s);assert(s.glink_error==-22);
    setup();change=desc;picasso_adsp_survey(&io,&s);assert(s.glink_error==-11);
    setup();change=crash;picasso_adsp_survey(&io,&s);assert(s.crash_error==-11 && !s.crash[0]);
    setup();memset(memory+crash,'x',256);memcpy(memory+crash+80,"sensor watchdog suffix",22);
    picasso_adsp_survey(&io,&s);assert(!s.crash_error && strlen(s.crash)==255 && !memcmp(s.crash+80,"sensor watchdog suffix",22));
    // Non-NUL input is truncated safely; control characters are still sanitized.
    setup();memset(memory+crash,'y',256);memory[crash+200]='\n';picasso_adsp_survey(&io,&s);
    assert(strlen(s.crash)==255 && s.crash[200]=='.' && !s.crash[255]);
    setup();p32(crash-12,8);p16(crash-8,7);p32(global+12,56);memory[crash]='z';
    picasso_adsp_survey(&io,&s);assert(!s.crash_error && !strcmp(s.crash,"z"));
    setup();change=inbound+36;picasso_adsp_survey(&io,&s);assert(s.inbound_error==-11 && !s.inbound);
    setup();change=part+12;picasso_adsp_survey(&io,&s);assert(s.inbound_error || s.partition_error);
    setup();p32(part+12,32);picasso_adsp_survey(&io,&s);assert(s.inbound_error==-2 && s.glink_error==-2);
    // No private partition: global is allowed. Missing items in a private one
    // above did not trigger fallback to a different owner's global items.
    setup();p16(toc+32+12,0);p16(toc+32+14,1);p16(part+6,1);
    smp(item(global,443,340),2,0,"slave-kernel",2);picasso_adsp_survey(&io,&s);
    assert(!s.partition_error && s.partition.offset==global && !s.inbound_error && s.inbound==2);
    setup();wlan_smem_item i={inbound,340};uint32_t v=99;
    assert(wlan_smp2p_entry(&io,&i,2,0,"0123456789abcdef",&v)==-22 && v==99);
    assert(wlan_smp2p_entry(&io,&i,2,0,"",&v)==-22);
    assert(wlan_smp2p_entry(&io,&i,2,0,nullptr,&v)==-22);
    printf("PASS ADSP SMEM: identity, ranges, %u read failures, concurrency, global fallback, crash sanitization\n",max);
}
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,OSNoRetain=0};
class IOMemoryMap {public: uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(memory);} void release(){++unmaps;}};
class IODeviceMemory {
public:
    static IODeviceMemory *withRange(uint64_t base,uint64_t size){assert(base==WLAN_SMEM_BASE && size==sizeof memory);++allocations;static IODeviceMemory m;return fail_alloc?nullptr:&m;}
    IOMemoryMap *map(unsigned flags){assert(flags==3);++mappings;static IOMemoryMap m;return fail_map?nullptr:&m;}
    void release(){++releases;}
};
template<class T> class OSSharedPtr {T *p;public:OSSharedPtr(T *v,int):p(v){}~OSSharedPtr(){if(p)p->release();}explicit operator bool()const{return p!=nullptr;}T *operator->()const{return p;}};
static void IOLog(const char *fmt,...){char out[512];va_list args;va_start(args,fmt);int n=vsnprintf(out,sizeof out,fmt,args);va_end(args);assert(n>0 && n<(int)sizeof out);++lines;}
#include "PicassoAdsp-body.inc"
int main(int argc,char **argv){
    parser_tests();setup();
    fail_alloc=argc>1 && !strcmp(argv[1],"alloc");fail_map=argc>1 && !strcmp(argv[1],"map");
    uint8_t *before=(uint8_t *)malloc(sizeof memory);assert(before);memcpy(before,memory,sizeof memory);
    for(unsigned tick=0;tick<100;++tick){unsigned old=allocations;picasso_adsp_poll();assert(allocations-old==(tick==0 || tick==5 || tick==30));}
    assert(allocations==3 && releases==(fail_alloc?0:3) && mappings==(fail_alloc?0:3));
    assert(unmaps==((fail_alloc || fail_map)?0:3) && lines==((fail_alloc || fail_map)?3:10));
    assert(!memcmp(before,memory,sizeof memory));free(before);
    puts("PASS actual boot poll: 3 samples, 100 calls, no writes, no retained mappings, allocation failures");
}
