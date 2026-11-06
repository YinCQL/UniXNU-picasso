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
static uint8_t *disk;
static size_t diskBytes;
static unsigned calls, failRead, sleepCount, allocated, freed, starts, released, lines;
static uint64_t ticks;
static bool noPartition, noMemory, noThread, neverReady;
static unsigned pending;
static int readDisk(void *, uint64_t at, void *to, size_t n) {
    if (++calls == failRead || at > diskBytes || n > diskBytes - at) return -1;
    memcpy(to, disk + at, n); return 0;
}
extern "C" int native_ufs_modem_size(uint64_t *n) { *n = 335544320; if (neverReady || pending) {if(pending)--pending;return -11;} return noPartition ? -2 : 0; }
extern "C" int native_ufs_modem_read(uint64_t at, void *to, uint32_t n, void *) {return readDisk(nullptr, at, to, n);}
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
int main(int argc,char **argv) {
    fatTests();
    assert(argc>1);const char *mode=argv[1];
    if(!strcmp(mode,"stock")) {
        assert(argc==3);FILE *in=fopen(argv[2],"rb");assert(in);
        fseek(in,0,SEEK_END);diskBytes=(size_t)ftell(in);rewind(in);free(disk);disk=(uint8_t *)malloc(diskBytes);assert(disk);
        assert(fread(disk,1,diskBytes,in)==diskBytes);fclose(in);calls=failRead=0;
        auto *f=(Firmware *)calloc(1,sizeof(Firmware));assert(f);unsigned failed;uint64_t checked;f->deadline=UINT64_MAX;
        assert(!validateFirmware(f,335544320,&failed,&checked));assert(failed==fileCount && checked>21000000);
        printf("PASS actual stock firmware: %u files, %llu bytes, %u reads\n",fileCount,(unsigned long long)checked,calls);
        // Corrupt an actual ADSP piece byte: CRC/FAT success cannot hide it.
        uint64_t at=f->volume.data_offset+(uint64_t)(f->files[0].first_cluster-2)*f->volume.cluster_bytes;
        disk[at]^=1;memset(f,0,sizeof(*f));f->deadline=UINT64_MAX;
        assert(validateFirmware(f,335544320,&failed,&checked)==-8 && failed==0);
        disk[at]^=1;memset(f,0,sizeof(*f));f->deadline=0;
        assert(validateFirmware(f,335544320,&failed,&checked)==FatReadError);
        free(f);
        picasso_adsp_firmware_start();assert(starts==1&&allocated==1&&freed==1&&lines==2);
        puts("PASS stock worker cleanup, altered firmware rejection and deadline");
    } else {
        neverReady=!strcmp(mode,"not-ready");pending=!strcmp(mode,"late-ready")?3:0;
        noPartition=!strcmp(mode,"partition");noMemory=!strcmp(mode,"alloc");noThread=!strcmp(mode,"thread");
        calls=sleepCount=0;picasso_adsp_firmware_start();picasso_adsp_firmware_start();assert(starts==1);
        assert(released==!noThread);assert(allocated==(!noThread&&!noPartition&&!neverReady));
        assert(freed==(!noThread&&!noPartition&&!noMemory&&!neverReady));assert(lines==1);
        if(neverReady)assert(sleepCount==120);if(!strcmp(mode,"late-ready"))assert(sleepCount>=3);
    }
    free(disk);
}
