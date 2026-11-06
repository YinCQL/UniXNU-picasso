// SPDX-License-Identifier: BSD-2-Clause-Patent
// Executes production firmware worker and FAT parser; platform transport only
// is replaced. Optional stock ROM verifies every byte against its pinned hash.
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
struct SHA256_CTX { BCRYPT_ALG_HANDLE algorithm; BCRYPT_HASH_HANDLE hash; };
static void SHA256_Init(SHA256_CTX *c) {
    assert(BCryptOpenAlgorithmProvider(&c->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0);
    assert(BCryptCreateHash(c->algorithm, &c->hash, nullptr, 0, nullptr, 0, 0) == 0);
}
static void SHA256_Update(SHA256_CTX *c, const void *p, size_t n) {
    assert(BCryptHashData(c->hash, (PUCHAR)p, (ULONG)n, 0) == 0);
}
static void SHA256_Final(void *p, SHA256_CTX *c) {
    assert(BCryptFinishHash(c->hash, (PUCHAR)p, 32, 0) == 0);
    BCryptDestroyHash(c->hash); BCryptCloseAlgorithmProvider(c->algorithm, 0);
}
#else
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#endif
#include "PicassoFat.h"
#include "FirmwareWindow.h"
static uint8_t *disk;
static size_t diskBytes;
static unsigned calls, failRead, sleepCount, allocated, freed, starts, released, lines;
static uint64_t ticks;
static bool noPartition, noMemory, noThread, neverReady;
static unsigned pending;
static unsigned aopProbes, smpStarts;
static bool aopFailed, alterAfterValidation;
static uint64_t metadataAt;
extern "C" int picasso_aop_probe() { assert(freed == 1); ++aopProbes; return aopFailed ? -60 : 0; }
extern "C" void picasso_smp2p_start() {assert(aopProbes==1 && freed==1); ++smpStarts;}
static int readDisk(void *, uint64_t at, void *to, size_t n) {
    if (++calls == failRead || at > diskBytes || n > diskBytes - at) return -1;
    memcpy(to, disk + at, n); return 0;
}
extern "C" unsigned native_ufs_store_ready() { if (neverReady || pending) {if(pending)--pending;return 0;} return 1; }
extern "C" int native_ufs_modem_open(UfsFirmwareWindow *w, void *, uint32_t) { *w={100,100+335544320/4096-1,4}; return noPartition ? -2 : 0; }
extern "C" int native_ufs_modem_read(const UfsFirmwareWindow *,uint64_t at, void *to, uint32_t n, void *) {return readDisk(nullptr, at, to, n);}
static void IOSleep(unsigned) { ++sleepCount; }
static uint64_t mach_absolute_time() { return ++ticks; }
static void clock_interval_to_deadline(unsigned, unsigned, uint64_t *out) { *out = ticks + 1000000; }
static void *IOMalloc(size_t n) { ++allocated; return noMemory ? nullptr : calloc(1,n); }
static void IOFree(void *p, size_t) {++freed; free(p);}
#ifdef _WIN32
static void bzero(void *p, size_t n) {memset(p,0,n);}
#endif
static void IOLog(const char *fmt, ...) {
    va_list args; va_start(args,fmt); vprintf(fmt,args); va_end(args); ++lines;
    if (alterAfterValidation && strstr(fmt,"firmware source=modem lun=")) disk[metadataAt]^=1;
}
using wait_result_t = int; using kern_return_t = int; using thread_t = void *;
enum {KERN_SUCCESS=0, kSecondScale=1};
static thread_t current_thread() {return nullptr;}
static void thread_set_thread_name(thread_t,const char *) {}
static kern_return_t kernel_thread_start(void (*fn)(void *,wait_result_t),void *p,thread_t *t) {
    ++starts; if(noThread)return -1;*t=(void *)1;fn(p,0);return 0;
}
static void thread_deallocate(thread_t) {++released;}
#include "PicassoAdspFirmware-body.inc"
static void planTests() {
    uint8_t m[picassoAdspMetadataBytes]={}, good[sizeof m];uint32_t pieces[34]={};
    auto put=[&](unsigned at,uint32_t v){for(unsigned j=0;j<4;++j)m[at+j]=uint8_t(v>>(8*j));};
    put(0,0x464c457f);put(4,0x10101);put(16,2|(164U<<16));put(20,1);put(24,picassoAdspBase);
    put(28,52);put(40,52|(32U<<16));put(44,34);
    put(52+16,1140);put(52+24,0x07000000);pieces[0]=1140;
    put(84+4,4096);put(84+12,picassoAdspBase+picassoAdspBytes);put(84+16,8200);
    put(84+20,12288);put(84+24,0x02200000);pieces[1]=8200;
    for(unsigned i=2;i<34;++i) {
        unsigned at=52+32*i;put(at,1);put(at+4,0x4000+(i-2)*4096);
        put(at+12,picassoAdspBase+(i-2)*0x100000);pieces[i]=i==33?0:64;put(at+16,pieces[i]);
        put(at+20,i==33?0x900000:0x100000);put(at+24,0x08000005);put(at+28,0x100000);
    }
    memcpy(good,m,sizeof m);PicassoAdspLoadPlan p={};
    auto parse=[&](){return picasso_adsp_load_plan(m,sizeof m,pieces,34,&p);};
    assert(!parse() && p.count==32 && p.fileBytes==31*64 && p.zeroBytes==picassoAdspBytes-31*64);
    assert(p.segments[31].index==33 && !p.segments[31].fileBytes && p.segments[31].zeroBytes==0x900000);
    for(unsigned n=0;n<sizeof m;++n)assert(picasso_adsp_load_plan(m,n,pieces,34,&p)==-22);
    assert(picasso_adsp_load_plan(m,sizeof m,pieces,33,&p)==-22);
    const unsigned badHeader[]={0,4,16,20,28,40,44};
    for(unsigned at:badHeader){memcpy(m,good,sizeof m);put(at,0xffffffff);assert(parse()==-22);}
    const unsigned badSeg[]={0,4,12,16,20,24,28};
    for(unsigned field:badSeg){memcpy(m,good,sizeof m);put(116+field,0xffffffff);assert(parse()==-22);}
    memcpy(m,good,sizeof m);put(148+12,picassoAdspBase);assert(parse()==-22 && !strcmp(p.phase,"overlap"));
    memcpy(m,good,sizeof m);put(148+4,0x4000);assert(parse()==-22 && !strcmp(p.phase,"file"));
    memcpy(m,good,sizeof m);put(84+24,0x08000005);assert(parse()==-22 && !strcmp(p.phase,"metadata"));
    memcpy(m,good,sizeof m);put(24,picassoAdspBase+picassoAdspBytes);assert(parse()==-22);
    memcpy(m,good,sizeof m);put(52+33*32+20,0x8fffff);assert(parse()==-22);
    memcpy(m,good,sizeof m);pieces[33]=1;assert(parse()==-22);pieces[33]=0;
    memcpy(m,good,sizeof m);put(116+24,0x08000004);put(148+24,0x08000005);assert(parse()==-22); // Entry not executable.
    puts("PASS ELF load plan: truncation, headers, hash exclusion, overflow, relocation, overlaps, entry, missing BSS piece");
}
static void put16(size_t at, unsigned v) {disk[at]=(uint8_t)v;disk[at+1]=(uint8_t)(v>>8);}
static void put32(size_t at, unsigned v) {put16(at,v);put16(at+2,v>>16);}
static constexpr unsigned fat1=512, fatBytes=32*512, fat2=fat1+fatBytes, root=fat2+fatBytes, data=root+512;
static void setup() {
    calls=failRead=0;free(disk);diskBytes=8192*512;disk=(uint8_t *)calloc(1,diskBytes);assert(disk);
    put16(11,512);disk[13]=1;put16(14,1);disk[16]=2;put16(17,16);put16(19,8192);put16(22,32);put16(510,0xaa55);
    put16(fat1,0xfff8);put16(fat1+2,0xffff);put16(fat1+4,0xffff);
    memcpy(disk+fat2,disk+fat1,fatBytes);
    memcpy(disk+root,"FILE    BIN",11);put16(root+26,2);put32(root+28,3);
    memcpy(disk+data,"abc",3);
}
static void fatTests() {
    FatVolume v;FatFile f;uint8_t seen[8192]={},out[3];
    setup();assert(!picasso_fat_mount(&v,diskBytes,readDisk,nullptr));assert(!picasso_fat_open(&v,"file.bin",&f));
    assert(!picasso_fat_validate(&v,&f,seen));assert(!picasso_fat_read(&v,&f,0,out,3)&&!memcmp(out,"abc",3));
    assert(picasso_fat_validate(&v,&f,seen)==FatDamaged);assert(picasso_fat_read(&v,&f,2,out,2)==FatRange);
    setup();disk[fat2+4]^=1;assert(picasso_fat_mount(&v,diskBytes,readDisk,nullptr)==FatDamaged);
    setup();put16(22,0xffff);assert(picasso_fat_mount(&v,diskBytes,readDisk,nullptr)==FatNotFat);
    setup();assert(picasso_fat_mount(&v,1024,readDisk,nullptr)==FatNotFat);
    setup();assert(!picasso_fat_mount(&v,diskBytes,readDisk,nullptr));memcpy(disk+root+32,disk+root,32);
    assert(picasso_fat_open(&v,"file.bin",&f)==FatDamaged);
    setup();put16(fat1+4,2);memcpy(disk+fat2,disk+fat1,fatBytes);
    assert(!picasso_fat_mount(&v,diskBytes,readDisk,nullptr));assert(!picasso_fat_open(&v,"file.bin",&f));
    memset(seen,0,sizeof seen);assert(picasso_fat_validate(&v,&f,seen)==FatDamaged);
    // Malformed LFN ordinal zero following ordinal 1 previously underflowed.
    setup();memmove(disk+root+64,disk+root,32);memset(disk+root,0,64);
    disk[root]=0x41;disk[root+11]=15;disk[root+32]=0x20;disk[root+43]=15;
    assert(!picasso_fat_mount(&v,diskBytes,readDisk,nullptr));assert(!picasso_fat_open(&v,"file.bin",&f));
    setup();assert(!picasso_fat_mount(&v,diskBytes,readDisk,nullptr));failRead=calls+1;
    assert(picasso_fat_open(&v,"file.bin",&f)==FatReadError);
    puts("PASS firmware FAT: mirrors, cycles, aliases, duplicate names, LFN ordinal, bounds, read failure");
}
static void loaderTests() {
    int error=0;
    PicassoAdspImage *image=picasso_adsp_image_open(&error);assert(image && !error);
    auto *f=reinterpret_cast<Firmware *>(image);
    const size_t guard=4096,bytes=picassoAdspBytes;
    auto *allocation=static_cast<uint8_t *>(malloc(bytes+guard*2));
    auto *expected=static_cast<uint8_t *>(malloc(bytes));assert(allocation && expected);
    memset(allocation,0xa5,bytes+guard*2);memset(expected,0xa5,bytes);
    auto *to=allocation+guard;
    for(unsigned i=0;i<f->plan.count;++i){
        const auto &s=f->plan.segments[i];
        if(s.fileBytes)assert(!picasso_fat_read(&f->volume,&f->files[s.index],0,expected+s.memoryOffset,s.fileBytes));
        memset(expected+s.memoryOffset+s.fileBytes,0,s.zeroBytes);
    }
    calls=0;assert(!picasso_adsp_image_load(image,to,bytes));unsigned reads=calls;
    assert(!memcmp(to,expected,bytes));
    auto guards=[&](){for(size_t i=0;i<guard;++i)assert(allocation[i]==0xa5 && allocation[guard+bytes+i]==0xa5);};
    guards();assert(picasso_adsp_image_load(image,to,bytes-1)==-22);
    assert(picasso_adsp_image_load(nullptr,to,bytes)==-22);
    auto saved=f->plan;
    f->plan.count=33;assert(picasso_adsp_image_load(image,to,bytes)==-22);f->plan=saved;
    f->plan.segments[0].memoryOffset=UINT32_MAX;assert(picasso_adsp_image_load(image,to,bytes)==-22);f->plan=saved;
    const unsigned failAt[]={1,2,17,reads/2,reads};
    for(unsigned n:failAt){calls=0;failRead=n;assert(picasso_adsp_image_load(image,to,bytes)==FatReadError);guards();}
    failRead=0;
    uint64_t at=f->volume.data_offset+(uint64_t)(f->files[2].first_cluster-2)*f->volume.cluster_bytes;
    disk[at]^=1;assert(picasso_adsp_image_load(image,to,bytes)==-8);disk[at]^=1;guards();
    f->deadline=0;assert(picasso_adsp_image_load(image,to,bytes)==FatReadError);f->deadline=UINT64_MAX;
    // Exercise the separate BSS deadline, without doing a transport read first.
    f->plan.segments[0]=saved.segments[31];f->deadline=0;
    assert(picasso_adsp_image_load(image,to,bytes)==-60);f->plan=saved;guards();
    picasso_adsp_image_close(image);assert(allocated==freed);
    noPartition=true;assert(!picasso_adsp_image_open(&error)&&error==-2);noPartition=false;
    disk[at]^=1;assert(!picasso_adsp_image_open(&error)&&error==-8);disk[at]^=1;
    failRead=calls+1;assert(!picasso_adsp_image_open(&error)&&error);failRead=0;
    assert(allocated==freed);free(expected);free(allocation);
    printf("PASS actual stock loader: 32 segment copies/BSS, full 40MiB comparison and guards, %u reads, tamper-after-open, short/failed reads, bounds/deadlines and reopen cleanup\n",reads);
}
int main(int argc,char **argv) {
    fatTests();planTests();
    assert(argc>1);const char *mode=argv[1];
    if(!strcmp(mode,"stock") || !strcmp(mode,"stock-aop-fail") || !strcmp(mode,"stock-plan-fail") || !strcmp(mode,"stock-load")) {
        aopFailed=!strcmp(mode,"stock-aop-fail");
        alterAfterValidation=!strcmp(mode,"stock-plan-fail");
        assert(argc==3);FILE *in=fopen(argv[2],"rb");assert(in);
        fseek(in,0,SEEK_END);diskBytes=(size_t)ftell(in);rewind(in);free(disk);disk=(uint8_t *)malloc(diskBytes);assert(disk);
        assert(fread(disk,1,diskBytes,in)==diskBytes);fclose(in);calls=failRead=0;
        if(!strcmp(mode,"stock-load")){loaderTests();free(disk);return 0;}
        auto *f=(Firmware *)calloc(1,sizeof(Firmware));assert(f);unsigned failed;uint64_t checked;f->deadline=UINT64_MAX;
        assert(!validateFirmware(f,335544320,&failed,&checked));assert(failed==fileCount && checked>21000000);
        printf("PASS actual stock firmware: %u files, %llu bytes, %u reads\n",fileCount,(unsigned long long)checked,calls);
        calls=0;assert(!planFirmware(f));unsigned planReads=calls;
        assert(f->plan.count==32 && f->plan.imageBytes==21500076 && f->plan.alignment==0x100000);
        assert(f->plan.segments[31].zeroBytes==0x5a3000 && !f->plan.segments[31].fileBytes);
        printf("PASS stock load plan: copy=%u zero=%u segments=%u reads=%u\n",f->plan.fileBytes,f->plan.zeroBytes,f->plan.count,planReads);
        for(unsigned n=1;n<=planReads;++n){calls=0;failRead=n;assert(planFirmware(f));}failRead=0;
        metadataAt=f->volume.data_offset+(uint64_t)(f->files[fileCount-1].first_cluster-2)*f->volume.cluster_bytes;
        disk[metadataAt]^=1;assert(planFirmware(f)==-8 && !strcmp(f->plan.phase,"metadata-hash"));disk[metadataAt]^=1;
        // Corrupt an actual ADSP piece byte: CRC/FAT success cannot hide it.
        uint64_t at=f->volume.data_offset+(uint64_t)(f->files[0].first_cluster-2)*f->volume.cluster_bytes;
        disk[at]^=1;assert(planFirmware(f)==-8 && !strcmp(f->plan.phase,"metadata-split"));
        memset(f,0,sizeof(*f));f->deadline=UINT64_MAX;
        assert(validateFirmware(f,335544320,&failed,&checked)==-8 && failed==0);
        disk[at]^=1;memset(f,0,sizeof(*f));f->deadline=0;
        assert(validateFirmware(f,335544320,&failed,&checked)==FatReadError);
        free(f);
        picasso_adsp_firmware_start();assert(starts==1&&allocated==1&&freed==1&&lines==(alterAfterValidation?2U:3U));
        assert(aopProbes==!alterAfterValidation && smpStarts==(!alterAfterValidation&&!aopFailed));
        puts("PASS stock worker cleanup, altered firmware rejection and deadline");
    } else {
        neverReady=!strcmp(mode,"not-ready");pending=!strcmp(mode,"late-ready")?3:0;
        noPartition=!strcmp(mode,"partition");noMemory=!strcmp(mode,"alloc");noThread=!strcmp(mode,"thread");
        calls=sleepCount=0;picasso_adsp_firmware_start();picasso_adsp_firmware_start();assert(starts==1);
        assert(released==!noThread);assert(allocated==(!noThread&&!neverReady));
        assert(freed==(!noThread&&!noMemory&&!neverReady));assert(lines==1 && !aopProbes && !smpStarts);
        if(neverReady)assert(sleepCount==120);if(!strcmp(mode,"late-ready"))assert(sleepCount>=3);
    }
    free(disk);
}
