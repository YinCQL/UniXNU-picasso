// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual GPT parser. Root manifest crypto is not entered by this GPT fixture.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
static uint8_t header[4096],entries[4096];
static unsigned reads;
static unsigned hostRead4k(uint8_t lun,uint64_t lba,uint32_t blocks,void *out) {
    assert(!lun&&blocks==1);++reads;
    if(lba==1)memcpy(out,header,4096);
    else if(lba==2)memcpy(out,entries,4096);
    else {assert(lba==100);memset(out,0,4096);}
    return 0;
}
#include "RootLayout.h"
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n) {put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static void part(unsigned i,const char *name,uint64_t first,uint64_t last) {
    uint8_t *p=entries+128*i;memset(p,0,128);p[0]=1;put64(p+32,first);put64(p+40,last);
    for(unsigned j=0;name[j];++j)p[56+2*j]=(uint8_t)name[j];
}
static void setup() {
    reads=0;memset(header,0,4096);memset(entries,0,4096);
    memcpy(header,"EFI PART",8);put32(header+8,0x10000);put32(header+12,92);put64(header+24,1);
    put64(header+32,1000);put64(header+40,3);put64(header+48,998);
    put64(header+72,2);put32(header+80,32);put32(header+84,128);
    part(0,"userdata",100,998);part(1,"modem",3,99);
}
static bool parse(RootLayout *l,bool crc=true) {
    if(crc){put32(header+88,rootCrc32(entries,4096));put32(header+16,rootCrc32(header,92,true));}
    const char *why=nullptr;return findRootLayout(l,&why);
}
int main() {
    RootLayout l;
    setup();assert(parse(&l)&&l.modemFirst==3&&l.modemLast==99&&l.userdataFirst==100);
    setup();part(1,"modem_a",3,99);assert(parse(&l)&&!l.modemFirst);
    setup();entries[128+57]=1;assert(parse(&l)&&!l.modemFirst);
    setup();entries[128+68]=1;assert(parse(&l)&&!l.modemFirst);
    setup();part(1,"modem",3,20);part(2,"modem",21,40);part(3,"modem",41,99);
    assert(parse(&l)&&l.modemLast<l.modemFirst); // firmware ambiguous; root remains usable
    setup();part(1,"modem",3,100);assert(!parse(&l));
    setup();part(1,"modem",2,99);assert(!parse(&l));
    setup();part(1,"modem",3,UINT64_MAX);assert(!parse(&l));
    setup();assert(parse(&l));header[16]^=1;assert(!parse(&l,false));
    setup();assert(parse(&l));entries[128+56]^=1;assert(!parse(&l,false));
    puts("PASS actual GPT: exact modem UTF16 name, optional/duplicate rejection, bounds, overlap and CRC");
}
