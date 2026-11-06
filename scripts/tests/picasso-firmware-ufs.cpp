// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
static unsigned ready, calls, fail;
static uint64_t gModemFirst, gModemLast;
static uint8_t storage[4*4096];
static unsigned native_ufs_store_ready() {return ready;}
static constexpr unsigned kIOReturnSuccess=0;
static unsigned hostRead4k(uint8_t lun,uint64_t lba,uint32_t blocks,void *out) {
    assert(lun==0 && blocks==1 && lba>=100 && lba<104);
    if(++calls==fail)return 1;
    memcpy(out,storage+(lba-100)*4096,4096);return 0;
}
#include "FirmwareRead.h"
int main() {
    uint64_t size=123;uint8_t bounce[4096],out[8192];
    assert(native_ufs_modem_size(nullptr)==-22);
    assert(native_ufs_modem_size(&size)==-11 && !size);
    ready=1;assert(native_ufs_modem_size(&size)==-2);
    gModemFirst=100;gModemLast=99;assert(native_ufs_modem_size(&size)==-2);
    gModemLast=UINT64_MAX;assert(native_ufs_modem_size(&size)==-2);
    gModemLast=103;assert(!native_ufs_modem_size(&size)&&size==sizeof storage);
    for(unsigned i=0;i<sizeof storage;++i)storage[i]=(uint8_t)(i*13+i/4096);
    assert(native_ufs_modem_read(UINT64_MAX,out,2,bounce)==-22);
    assert(native_ufs_modem_read(size-1,out,2,bounce)==-22);
    assert(native_ufs_modem_read(0,out,65537,bounce)==-22);
    assert(native_ufs_modem_read(0,bounce,1,bounce)==-22);
    assert(!calls);assert(!native_ufs_modem_read(4093,out,8192,bounce));
    assert(calls==3&&!memcmp(out,storage+4093,8192));
    fail=calls+2;assert(native_ufs_modem_read(4093,out,8192,bounce)==-5);
    assert(calls==fail);ready=0;assert(native_ufs_modem_read(0,out,1,bounce)==-11);
    puts("PASS actual modem reader: readiness, bounds, unaligned spanning reads, transport failure");
}
