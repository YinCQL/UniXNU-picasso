// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "RootDiagnostics.h"
struct SHA256_CTX {};
static void SHA256_Init(SHA256_CTX *) {abort();}
static void SHA256_Update(SHA256_CTX *,const void *,size_t) {abort();}
static void SHA256_Final(void *,SHA256_CTX *) {abort();}
#ifdef _WIN32
static void bzero(void *p,size_t n) {memset(p,0,n);}
#endif
using IOReturn=unsigned;
constexpr unsigned kDataMax=131072,kIOReturnSuccess=0;
static UfsRootDiagnostics gRootDiagnostic;
static unsigned ready=1,calls,fail,geometries;
static int geometryError[8];
static uint64_t capacities[8];
static uint8_t header[8][4096],entries[8][16384],storage[4*4096];
static unsigned native_ufs_store_ready() {return ready;}
static void IOLog(const char *,...) {}
static int hostFirmwareGeometry(uint8_t lun,uint64_t *blocks) {
    assert(lun==4);++geometries;*blocks=capacities[lun];return geometryError[lun];
}
static unsigned hostRead4k(uint8_t lun,uint64_t lba,uint32_t blocks,void *out) {
    assert(lun<8 && blocks && blocks<=4 && lba+blocks<=capacities[lun]);
    if(++calls==fail)return 1;
    if(lba==1) {assert(blocks==1);memcpy(out,header[lun],4096);}
    else if(lba==2)memcpy(out,entries[lun],blocks*4096);
    else if(lun==0 && lba==100) {assert(blocks==1);memset(out,0,4096);}
    else {assert(lun==4 && blocks==1 && lba>=100 && lba<104);memcpy(out,storage+(lba-100)*4096,4096);}
    return 0;
}
#include "RootLayout.h"
#include "FirmwareRead.h"
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n) {put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static void part(unsigned lun,unsigned index,const char *name,uint64_t first,uint64_t last) {
    uint8_t *p=entries[lun]+128*index;memset(p,0,128);p[0]=1;put64(p+32,first);put64(p+40,last);
    for(unsigned j=0;name[j];++j)p[56+2*j]=(uint8_t)name[j];
}
static void crc(unsigned lun) {
    put32(header[lun]+88,rootCrc32(entries[lun],le32(header[lun]+80)*128));
    put32(header[lun]+16,rootCrc32(header[lun],92,true));
}
static void setup() {
    ready=1;calls=fail=geometries=0;memset(header,0,sizeof header);memset(entries,0,sizeof entries);
    for(unsigned i=0;i<8;++i) {
        geometryError[i]=-2;capacities[i]=1001;
        uint8_t *h=header[i];memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);put64(h+24,1);
        put64(h+32,1000);put64(h+40,6);put64(h+48,995);put64(h+72,2);put32(h+80,128);put32(h+84,128);
    }
    geometryError[0]=geometryError[1]=geometryError[4]=0;
    part(0,0,"userdata",100,995);part(4,0,"modem",100,103);
    for(unsigned i=0;i<8;++i)crc(i);
    memset(header[1],0,4096); // enabled boot LU without a GPT is not a candidate
}
int main(int argc,char **argv) {
    UfsFirmwareWindow w;uint8_t scratch[16384],bounce[4096],out[8192];
    setup();RootLayout root;const char *why=nullptr;
    assert(findRootLayout(&root,&why)&&root.userdataFirst==100&&!root.modemFirst);
    assert(!native_ufs_modem_open(&w,scratch,sizeof scratch)&&w.lun==4&&w.first==100&&w.last==103&&geometries==1);
    unsigned before=calls;
    for(unsigned i=0;i<sizeof storage;++i)storage[i]=(uint8_t)(i*13+i/4096);
    assert(native_ufs_modem_read(&w,UINT64_MAX,out,2,bounce)==-22);
    assert(native_ufs_modem_read(&w,sizeof storage-1,out,2,bounce)==-22);
    assert(native_ufs_modem_read(&w,0,out,65537,bounce)==-22);
    assert(native_ufs_modem_read(&w,0,bounce+1,1,bounce)==-22);
    assert(native_ufs_modem_read(&w,0,out,4096,out+4095)==-22);
    assert(calls==before);assert(!native_ufs_modem_read(&w,4093,out,8192,bounce));
    assert(calls==before+3&&!memcmp(out,storage+4093,8192));
    fail=calls+2;assert(native_ufs_modem_read(&w,4093,out,8192,bounce)==-5&&calls==fail);
    ready=0;assert(native_ufs_modem_read(&w,0,out,1,bounce)==-11);
    assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-11&&!w.first);
    setup();assert(native_ufs_modem_open(&w,scratch,16383)==-22&&!calls&&!geometries);
    setup();part(4,0,"modem_a",100,103);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-2&&!w.first);
    setup();entries[4][57]=1;crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-2);
    setup();entries[4][68]=1;crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-2);
    setup();part(4,1,"modem",104,107);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-17&&!w.first);
    setup();part(0,1,"modem",6,99);crc(0);assert(!native_ufs_modem_open(&w,scratch,sizeof scratch)&&w.lun==4); // LU0 is not a firmware source
    setup();part(4,1,"other",103,107);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();part(4,0,"modem",5,103);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();part(4,0,"modem",100,UINT64_MAX);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();header[4][16]^=1;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();entries[4][56]^=1;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();capacities[4]=999;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();put64(header[4]+72,UINT64_MAX);crc(4);assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();put32(header[4]+80,129);put32(header[4]+16,rootCrc32(header[4],92,true));
    assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-22);
    setup();geometryError[4]=-5;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-5&&!w.first&&geometries==1);
    setup();geometryError[4]=-95;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-95&&!w.first);
    setup();fail=2;assert(native_ufs_modem_open(&w,scratch,sizeof scratch)==-5&&!w.first&&calls==fail);
    setup();assert(!native_ufs_modem_open(&w,scratch,sizeof scratch));w.lun=8;
    assert(native_ufs_modem_read(&w,0,out,1,bounce)==-22);
    w.lun=4;w.last=UINT64_MAX;assert(native_ufs_modem_read(&w,0,out,1,bounce)==-22);
    if(argc==2) {
        setup();FILE *input=fopen(argv[1],"rb");assert(input);
        assert(!fseek(input,4096,SEEK_SET));assert(fread(header[4],1,4096,input)==4096);
        assert(le64(header[4]+72)==2 && le32(header[4]+80)==64);
        assert(!fseek(input,8192,SEEK_SET));assert(fread(entries[4],1,8192,input)==8192);fclose(input);
        capacities[4]=le64(header[4]+32)+1;
        assert(!native_ufs_modem_open(&w,scratch,sizeof scratch)&&w.lun==4&&w.first==0xf0a6&&w.last==0x230a5);
        // patch4.xml applies these capacity-dependent fields at flashing time.
        // Use a modeled larger capacity; never claim it is the phone's geometry.
        capacities[4]+=65536;put64(header[4]+32,capacities[4]-1);put64(header[4]+48,capacities[4]-6);
        put64(entries[4]+63*128+40,capacities[4]-6);crc(4);
        assert(!native_ufs_modem_open(&w,scratch,sizeof scratch)&&w.first==0xf0a6&&w.last==0x230a5);
        puts("PASS stock gpt_main4.bin and patch4.xml capacity fields: modem LU4 0xf0a6..0x230a5 320MiB");
    }
    puts("PASS actual LU4 discovery: CRC/capacity/UTF16/overlap/ambiguity, bounded reads and failures");
}
