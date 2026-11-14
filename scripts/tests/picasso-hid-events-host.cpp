// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
using IOOptionBits=unsigned;
struct OSMetaClass {const char *name; const char *getClassName()const{return name;}};
struct IOService {uintptr_t *table; OSMetaClass *meta; const OSMetaClass *getMetaClass()const{return meta;}
    unsigned long long getRegistryEntryID()const{return 123;}bool isOpen()const{return true;}};
using IORegistryEntry=IOService;
struct OSArray;
struct OSObject {
    uintptr_t *table; uint64_t padding[2]; OSArray *children;
    uint32_t type,index,mask,touch,page,usage,down;int32_t x,y;
    bool metaCast(const char *){return true;}
};
struct OSArray {OSObject *entries[2];unsigned count;unsigned getCount(){return count;}OSObject *getObject(unsigned n){return entries[n];}};
#define OSDynamicCast(type,p) static_cast<type *>(p)
static uintptr_t originalDispatch;
static uintptr_t kOriginalEventDriverMethods[220];
static bool fromEdge[256];
static unsigned native_touch_edge_stats[4],consumerReports,allocated,forwarded,logs,digitizerDispatches;
enum:uint16_t {kDivGetType=0,kDivGetInteger=0,kDivGetFixed=0,kDivSetInteger=0};
static void *IOMalloc(size_t n){allocated=unsigned(n);return malloc(n);}
static int trace(const char *format,...) {if(strstr(format,"PICASSO_HID consumer"))++logs;return 0;}
#define printf trace
#include "hid-events.inc"
#undef printf
static uint32_t getType(void *p,IOOptionBits){return static_cast<OSObject *>(p)->type;}
static int32_t getInt(void *p,uint32_t field,IOOptionBits){
    auto *e=static_cast<OSObject *>(p);
    switch(field){case 0x30000:return e->page;case 0x30001:return e->usage;case 0x30002:return e->down;
        case kIndex:return e->index;case kMask:return e->mask;default:return e->touch;}
}
static int32_t getFixed(void *p,uint32_t field,IOOptionBits){auto *e=static_cast<OSObject *>(p);return field==kX?e->x:e->y;}
static void setInt(void *p,uint32_t field,int32_t value,IOOptionBits){assert(field==kMask);static_cast<OSObject *>(p)->mask=uint32_t(value);}
static IOService *expectedService;static OSObject *expectedEvent;static IOOptionBits expectedOptions;
static void original(IOService *d,OSObject *e,IOOptionBits o){assert(d==expectedService && e==expectedEvent && o==expectedOptions);++forwarded;}
int main(){
    for(unsigned i=0;i<220;++i)kOriginalEventDriverMethods[i]=i%2?0:0x10000+i*8;
    kOriginalEventDriverMethods[198]=0x12300;
    auto *table=static_cast<uintptr_t *>(malloc(220*sizeof(uintptr_t)));
    for(unsigned i=0;i<220;++i)table[i]=kOriginalEventDriverMethods[i]?kOriginalEventDriverMethods[i]:0x50000;
    OSMetaClass meta={"IOHIDEventDriver"};IOService d={table,&meta};
    table[198]^=8;install(&d);assert(!originalDispatch && !allocated);table[198]^=8;
    table[1]=0;install(&d);assert(!originalDispatch && !allocated);table[1]=0x50000;
    meta.name="Subclass";install(&d);assert(!originalDispatch && !allocated);meta.name="IOHIDEventDriver";
    install(&d);assert(allocated==220*sizeof(uintptr_t) && originalDispatch==table[198]);
    for(unsigned i=0;i<220;++i)assert(d.table[i]==(i==198?(uintptr_t)&dispatch:table[i]));
    free(d.table);free(table);originalDispatch=(uintptr_t)&original;
    uintptr_t vt[37]={};vt[18]=(uintptr_t)&getType;vt[23]=(uintptr_t)&getInt;vt[24]=(uintptr_t)&setInt;vt[25]=(uintptr_t)&getFixed;
    OSObject e={};e.table=vt;e.type=3;e.page=0xc;
    expectedService=&d;expectedEvent=&e;expectedOptions=0x42;
    const unsigned usages[]={0x30,0xe9,0xea};
    for(unsigned i=0;i<90;++i){e.usage=usages[i%3];e.down=i%2;auto before=e;dispatch(&d,&e,0x42);assert(!memcmp(&e,&before,sizeof e));}
    assert(consumerReports==64 && logs==64 && forwarded==90);
    consumerReports=0;e.page=7;dispatch(&d,&e,0x42);assert(!consumerReports);
    e.page=0xc;e.usage=0x40;dispatch(&d,&e,0x42);assert(!consumerReports);
    e.type=11;e.index=1;e.mask=2;e.touch=1;e.x=32000;e.y=65000;
    dispatch(&d,&e,0x42);assert(e.mask&0x800);
    e.mask=2;e.touch=0;dispatch(&d,&e,0x42);assert((e.mask&0x800) && !fromEdge[1]);
    e.mask=2;e.touch=1;e.y=32000;dispatch(&d,&e,0x42);assert(!(e.mask&0x800));
    expectedEvent=nullptr;dispatch(&d,nullptr,0x42);assert(forwarded==96);
    // Distinct synthetic/physical IDs preserve independent edge lifetimes.
    expectedEvent=&e;e.index=255;e.mask=2;e.touch=1;e.y=65000;
    dispatch(&d,&e,0x42);assert(fromEdge[255]);
    OSObject finger=e;finger.index=1;finger.mask=2;finger.y=32000;
    e.mask=2;e.touch=0;
    OSArray children={{&finger,&e},2};OSObject collection={};collection.table=vt;collection.type=11;collection.children=&children;
    expectedEvent=&collection;dispatch(&d,&collection,0x42);
    assert(!(finger.mask&0x800) && (e.mask&0x800) && !fromEdge[255] && !fromEdge[1]);
    assert(collection.mask&0x800);
    assert(digitizerDispatches==5 && forwarded==98);
    puts("PASS actual HID wrapper, 220-slot extent, unchanged Consumer events and bounded diagnostics");
}
