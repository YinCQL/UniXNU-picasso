#define _CRT_SECURE_NO_WARNINGS
// SPDX-License-Identifier: BSD-2-Clause-Patent
// The complete production restore method plus the original CHRP helpers.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#define require(c,l) do {if(!(c))goto l;}while(0)
#define DEBUG_INFO(...) ((void)0)
#define DEBUG_ERROR(...) ((void)0)
#define DEBUG_ALWAYS(...) ((void)0)
using IOReturn=int;using IOByteCount=size_t;
enum {kIOReturnSuccess=0,kIOReturnError=1,kIOReturnBadArgument=2,kIOReturnIOError=3,kIOReturnNoMemory=4};
enum NVRAMVersion {kNVRAMVersionUnknown,kNVRAMVersion1,kNVRAMVersion2,kNVRAMVersion3};
#define NVRAM_CHRP_APPLE_HEADER_NAME_V1 "nvram"
#define NVRAM_CHRP_APPLE_HEADER_NAME_V2 "2nvram"
struct chrp_nvram_header {uint8_t sig,cksum;uint16_t len;char name[12];};
struct apple_nvram_header {chrp_nvram_header chrp;uint32_t adler,generation;uint8_t padding[8];};
#include "nvram-helpers.inc"
static bool failImage=false,failDictionary=false;
#define IONewData(t,n) (failImage?nullptr:static_cast<t *>(malloc(n*sizeof(t))))
#define IODeleteData(p,t,n) free(p)
struct OSDictionary {
    static std::shared_ptr<OSDictionary> withCapacity(unsigned) {return failDictionary?nullptr:std::make_shared<OSDictionary>();}
};
class IONVRAMCHRPHandler {
public:
    bool _picassoRestoring=false,_newData=false;unsigned _commonUsed=5,_systemUsed=5,calls=0;
    uint32_t _bankSize=8192,_commonPartitionOffset=48,_commonPartitionSize=2048,_systemPartitionSize=0,_generation=99;
    uint8_t *_nvramImage=static_cast<uint8_t *>(malloc(8192));
    std::shared_ptr<OSDictionary> _varDict=OSDictionary::withCapacity(1);
    IOReturn conversion=0;
    ~IONVRAMCHRPHandler(){free(_nvramImage);}
    static bool isValidImage(const uint8_t *p,size_t n) {return validateNVRAMVersion(p,n,nullptr)!=kNVRAMVersionUnknown;}
    IOReturn unserializeVariables() {assert(_picassoRestoring && !_commonUsed && !_systemUsed);++calls;_newData=true;return conversion;}
    IOReturn restorePicasso(const uint8_t *,uint32_t);
};
#include "nvram-restore.inc"
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    uint8_t valid[8192],bad[8192];assert(fread(valid,1,8192,f)==8192);fclose(f);
    IONVRAMCHRPHandler handler;assert(!handler.restorePicasso(valid,8192) && handler.calls==1 && !handler._picassoRestoring);
    unsigned cases=1;
    for(unsigned i=0;i<8192;i+=17) {
        memcpy(bad,valid,8192);bad[i]^=1;handler.calls=0;
        assert(handler.restorePicasso(bad,8192) && !handler.calls);++cases;
    }
    // Recomputed Adler cannot authorize an alternate partition layout.
    memcpy(bad,valid,8192);auto *h=reinterpret_cast<chrp_nvram_header *>(bad+32);h->len=200;h->cksum=chrp_checksum(h);
    reinterpret_cast<apple_nvram_header *>(bad)->adler=adler32_with_version(bad,8192,1);
    assert(handler.restorePicasso(bad,8192));++cases;
    assert(handler.restorePicasso(valid,8191));assert(handler.restorePicasso(nullptr,8192));cases+=2;
    auto *oldImage=handler._nvramImage;auto oldDict=handler._varDict;
    failImage=true;assert(handler.restorePicasso(valid,8192)==kIOReturnNoMemory && handler._nvramImage==oldImage && handler._varDict==oldDict);failImage=false;++cases;
    failDictionary=true;assert(handler.restorePicasso(valid,8192)==kIOReturnNoMemory && handler._nvramImage==oldImage && handler._varDict==oldDict);failDictionary=false;++cases;
    handler.conversion=kIOReturnNoMemory;handler._commonUsed=42;handler._generation=987;handler._newData=false;
    assert(handler.restorePicasso(valid,8192)==kIOReturnNoMemory && handler._nvramImage==oldImage && handler._varDict==oldDict && handler._commonUsed==42 && handler._generation==987 && !handler._newData && !handler._picassoRestoring);++cases;
    printf("PASS NVRAM: %u CHRP validation and locked restore cases\n",cases);
}
