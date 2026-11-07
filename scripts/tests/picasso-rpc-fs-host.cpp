// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoFastrpc.h"
#include "PicassoRpcFs.h"
#include "WlanSmem.h"
static uint8_t in[PicassoRpcBlob],out[PicassoRpcBlob],prim[16],buffer[PicassoRpcBlob];
static char name[128];
static unsigned cursor;
static void arg(const void *p,unsigned n) { rpcPut32(in+cursor,n);cursor+=4;if(n) { cursor=(cursor+7)&~7U;memcpy(in+cursor,p,n);cursor+=n; } }
static int invoke(unsigned pd,uint32_t sc,const void **inputs,const unsigned *lens,const unsigned *outputs) {
    cursor=0;unsigned ni=(sc>>16)&255,no=(sc>>8)&255;
    for(unsigned i=0;i<ni;++i)arg(inputs[i],lens[i]);
    for(unsigned i=0;i<no;++i) { rpcPut32(in+cursor,outputs[i]);cursor+=4; }
    PicassoRpcArg a[8]={};unsigned size=0;
    assert(!rpcUnpack(in,cursor,sc,out,a,&size));
    name[0]=0;return picasso_rpc_fs_invoke(pd,sc,a,name);
}
static uint32_t openFile(const char *path,const char *mode,unsigned pd=2,int expected=0,bool env=false) {
    const char *key="ADSP_LIBRARY_PATH",*delim=";";
    const void *p[]={prim,path,mode};unsigned l[]={8,unsigned(strlen(path)+1),unsigned(strlen(mode)+1)},o[]={4};
    rpcPut32(prim,l[1]);rpcPut32(prim+4,l[2]);int error;
    if(env) {
        const void *v[]={prim,key,delim,path,mode};unsigned n[]={16,18,2,l[1],l[2]};
        for(unsigned i=0;i<4;++i)rpcPut32(prim+4*i,n[i+1]);error=invoke(pd,0x13050100,v,n,o);
    } else error=invoke(pd,0x30100,p,l,o);
    assert(error==expected);return error?0:rpc32(out+8);
}
static int fileOp(uint32_t h,uint32_t sc,unsigned pd=2,unsigned x=0,unsigned y=0,unsigned bytes=0) {
    rpcPut32(prim,h);rpcPut32(prim+4,x);rpcPut32(prim+8,y);
    const void *p[]={prim,buffer};unsigned l[]={((sc>>16)&255)==2?8U:((sc>>24)==9?12U:((sc>>24)==4 || (sc>>24)==6 || (sc>>24)==20?8U:4U)),x};
    unsigned o[]={((sc>>24)==10 || (sc>>24)==4 || (sc>>24)==5)?8U:4U,bytes};return invoke(pd,sc,p,l,o);
}
static int pathOp(const char *p,uint32_t sc,unsigned value=0) {
    const void *v[]={prim,p};unsigned l[]={8,unsigned(strlen(p)+1)},o[]={((sc>>24)==31)?96U:8U};
    rpcPut32(prim,l[1]);rpcPut32(prim+4,value);if((sc>>24)==31) { rpcPut32(prim,31);rpcPut32(prim+4,l[1]); }
    return invoke(2,sc,v,l,o);
}
static void stock() {
    picasso_rpc_fs_reset();const char *config="/vendor/etc/sensors/sns_reg_config";
    assert(!pathOp(config,0x1f020100));assert(rpc64(out+8+40)==553 && rpc32(out+8+24)==0100644);
    auto h=openFile(config,"r",2,0,true);assert(!fileOp(h,0x0a010100));assert(rpc64(out+8)==553);
    assert(!fileOp(h,0x14010200,2,100,0,100));assert(!rpc32(out+8) && !strcmp((char *)out+16,"version=1\n"));
    assert(!fileOp(h,0x08010100) && rpc32(out+8)==10);
    assert(!fileOp(h,0x09010000,2,0,0));
    assert(!fileOp(h,0x04010200,2,4096,0,4096));assert(rpc32(out+8)==553 && rpc32(out+12)==1);
    assert(!memcmp(out+24,"version=1\n",10));
    assert(!fileOp(h,0x03010000));assert(fileOp(h,0x08010100)==9);
    openFile(config,"w",2,13);openFile("/vendor/etc/sensors/nope","r",2,2,true);
    openFile("/vendor/etc/sensors/../sns_reg_config","r",2,22);
    h=openFile("/vendor/etc/sensors/config/sns_ccd.json","rb");unsigned total=0;
    for(;;) { assert(!fileOp(h,0x04010200,2,1024,0,1024));unsigned n=rpc32(out+8);total+=n;if(rpc32(out+12))break; }
    assert(total==6205);assert(!fileOp(h,0x03010000));
    assert(!pathOp("/vendor/etc/sensors/config",0x1a020100));uint64_t dir=rpc64(out+8);
    unsigned entries=0;for(;;) { rpcPut64(prim,dir);const void *p[]={prim};unsigned l[]={8},o[]={264};
        assert(!invoke(2,0x1c010100,p,l,o));if(rpc32(out+8+260))break;++entries;
        assert(strstr((char *)out+12,".json")); }
    assert(entries==40);rpcPut64(prim,dir);const void *p[]={prim};unsigned l[]={8};assert(!invoke(2,0x1b010000,p,l,nullptr));
    puts("PASS packed stock stat/open/read/line/seek/EOF/directory and missing-file semantics");
}
static void writes() {
    picasso_rpc_fs_reset();const char *base="/mnt/vendor/persist/sensors/registry/new";
    assert(!pathOp(base,0x1d020000,0700));assert(pathOp(base,0x1d020000,0700)==17);
    const char *p="/mnt/vendor/persist/sensors/registry/new/data";
    auto h=openFile(p,"w+");memcpy(buffer,"abc",3);assert(!fileOp(h,0x05020100,2,3));assert(rpc32(out+8)==3);
    assert(!fileOp(h,0x09010000,2,5));memcpy(buffer,"z",1);assert(!fileOp(h,0x05020100,2,1));
    assert(!fileOp(h,0x09010000,2,0));assert(!fileOp(h,0x04010200,2,10,0,10));assert(rpc32(out+8)==6 && !memcmp(out+24,"abc\0\0z",6));
    assert(fileOp(h,0x08010100,0)==9);assert(fileOp(h,0x17010000)==95);
    assert(!fileOp(h,0x09010000,2,8192));assert(fileOp(h,0x05020100,2,1)==28);
    assert(!fileOp(h,0x0d010100) && rpc32(out+8)==28);
    assert(!fileOp(h,0x03010000));assert(!pathOp(p,0x18020000));assert(pathOp(base,0x1e020000)==0);
    h=openFile("/persist/sensors/registry/sns_reg_version","r+");
    auto other=openFile("/mnt/vendor/persist/sensors/registry/sns_reg_version","r",0);
    memcpy(buffer,"AB",2);assert(!fileOp(h,0x05020100,2,2));assert(!fileOp(other,0x04010200,0,2,0,2));assert(!memcmp(out+24,"AB",2));
    assert(!fileOp(h,0x03010000));assert(!fileOp(other,0x03010000,0));
    // Handle exhaustion must not truncate an existing file.
    for(unsigned i=0;i<16;++i)openFile("/persist/sensors/registry/sns_reg_version","r");
    openFile("/persist/sensors/registry/sns_reg_version","w",2,24);
    assert(!pathOp("/persist/sensors/registry/sns_reg_version",0x1f020100) && rpc64(out+48)==11);
    puts("PASS bounded RAM writes, sparse seek, aliases, cross-PD handles, capacity and truthful fsync refusal");
}
static uint8_t smem[WLAN_SMEM_SIZE];
static int smread(void *,uint32_t at,void *p,uint32_t n) { if(at>sizeof smem || n>sizeof smem-at)return -22;memcpy(p,smem+at,n);return 0; }
static void socinfo() {
    picasso_rpc_fs_reset();memset(smem,0,sizeof smem);uint32_t values[6]={};wlan_smem_io io={nullptr,smread};
    assert(picasso_rpc_fs_socinfo(&io,values)!=0);openFile("/sys/devices/soc0/soc_id","r",2,2);
    unsigned toc=WLAN_SMEM_SIZE-4096,part=0x1000;rpcPut32(smem+0x5c,12<<16);
    rpcPut32(smem+toc,0x434f5424);rpcPut32(smem+toc+4,1);rpcPut32(smem+toc+8,1);
    rpcPut32(smem+toc+32,part);rpcPut32(smem+toc+36,4096);rpcPut32(smem+toc+44,0xfffefffe);rpcPut32(smem+toc+48,64);
    rpcPut32(smem+part,0x54525024);rpcPut32(smem+part+4,0xfffefffe);rpcPut32(smem+part+8,4096);
    rpcPut32(smem+part+12,120);rpcPut32(smem+part+16,4096);
    rpcPut32(smem+part+32,(137<<16)|0xa5a5);rpcPut32(smem+part+36,72);rpcPut32(smem+part+40,4);
    uint8_t *raw=smem+part+48;rpcPut32(raw,16);rpcPut32(raw+4,400);rpcPut32(raw+8,0x10002);
    rpcPut32(raw+52,36);rpcPut32(raw+56,0x10000);rpcPut32(raw+64,0);
    assert(!picasso_rpc_fs_socinfo(&io,values) && values[1]==400 && values[3]==36);
    auto h=openFile("/sys/devices/soc0/hw_platform","r");assert(!fileOp(h,0x04010200,2,32,0,32));assert(!memcmp(out+24,"PICASSO\n",8));
    openFile("/sys/devices/soc0/soc_id","w",2,13);
    h=openFile("/sys/devices/soc0/revision","r");assert(!fileOp(h,0x04010200,2,32,0,32));assert(!memcmp(out+24,"1.2\n",4));
    assert(picasso_rpc_fs_socinfo(&io,values)==-17);
    puts("PASS validated global SMEM socinfo and exact stock sysfs text with no fallback data");
}
static void malformed() {
    picasso_rpc_fs_reset();uint32_t rng=0x143f011;PicassoRpcArg a[8];unsigned bytes;
    for(unsigned test=0;test<10000;++test) {
        for(unsigned j=0;j<256;++j) { rng=rng*1664525+1013904223;in[j]=uint8_t(rng>>24); }
        uint32_t sc=(rng&0x1f000000)|((rng%6)<<16)|(((rng>>8)%3)<<8);
        if(!rpcUnpack(in,test%256,sc,out,a,&bytes))(void)picasso_rpc_fs_invoke(test&1?2:0,sc,a,name);
    }
    // Valid transport shape, malformed string lengths/terminators/output size.
    const char path[]="/vendor/etc/sensors/sns_reg_config";uint8_t bad[64];memcpy(bad,path,sizeof path);bad[sizeof(path)-1]='x';
    rpcPut32(prim,sizeof(path));rpcPut32(prim+4,2);const void *p[]={prim,bad,"r"};unsigned l[]={8,sizeof(path),2},o[]={4};
    assert(invoke(2,0x30100,p,l,o)==14);rpcPut32(prim,0xffffffffU);assert(invoke(2,0x30100,p,l,o)==14);
    rpcPut32(prim,sizeof(path));p[1]=path;o[0]=1;assert(invoke(2,0x30100,p,l,o)==14);
    puts("PASS malformed packed filesystem requests and string/output bounds under sanitizers");
}
int main() { stock();writes();socinfo();malformed(); }
