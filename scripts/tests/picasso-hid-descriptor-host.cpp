// SPDX-License-Identifier: BSD-2-Clause-Patent
// Parse the actual descriptor returned by production code. No IOKit model can
// qualify matching on hardware; this checks the wire layout and usage contract.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hid-descriptors.inc"
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNoMemory=-2,kIODirectionNone=0};
using IOReturn=int;
struct IOMemoryDescriptor {unsigned char bytes[2048];size_t length;};
static IOMemoryDescriptor buffer;
struct IOBufferMemoryDescriptor {
    static IOMemoryDescriptor *withBytes(const void *p,size_t n,int) {
        assert(n<=sizeof(buffer.bytes));memcpy(buffer.bytes,p,n);buffer.length=n;return &buffer;
    }
};
struct Device {
    bool touchOnly=false,touchEnabled=true;
#include "hid-descriptor-method.inc"
};
struct Field {uint32_t bit,size,count,flags,page,minimum,maximum,usageCount,usages[16],range[2];};
struct Report {unsigned bits=0,count=0;Field fields[128]={};};
struct Parsed {unsigned apps=0;uint32_t pages[4]={},usages[4]={};Report reports[4];};
static Parsed parse(const unsigned char *bytes,size_t length) {
    Parsed out={};uint32_t globals[16]={},locals[16]={},range[2]={};unsigned usageCount=0,depth=0;
    for(size_t at=0;at<length;) {
        unsigned prefix=bytes[at++],size=prefix&3,type=(prefix>>2)&3,tag=prefix>>4;
        if(size==3)size=4;assert(prefix!=0xfe && at+size<=length);
        uint32_t value=0;for(unsigned i=0;i<size;++i)value|=uint32_t(bytes[at++])<<(8*i);
        if(type==1){assert(tag<10);globals[tag]=value;}
        else if(type==2) {
            if(tag==0){assert(usageCount<16);locals[usageCount++]=value;}
            else if(tag==1 || tag==2)range[tag-1]=value;
            else assert(false);
        } else if(type==0) {
            if(tag==10) {
                if(value==1){assert(depth==0 && out.apps<4 && usageCount==1);out.pages[out.apps]=globals[0];out.usages[out.apps++]=locals[0];}
                ++depth;
            } else if(tag==12){assert(depth);--depth;}
            else if(tag==8) {
                unsigned id=globals[8];assert(depth && id>0 && id<4);
                Report &r=out.reports[id];assert(r.count<128);
                Field &f=r.fields[r.count++];f.bit=r.bits;f.size=globals[7];f.count=globals[9];f.flags=value;
                f.page=globals[0];f.minimum=globals[1];f.maximum=globals[2];f.usageCount=usageCount;
                memcpy(f.usages,locals,sizeof(locals));memcpy(f.range,range,sizeof(range));r.bits+=f.size*f.count;
            } else assert(false);
            usageCount=0;memset(locals,0,sizeof(locals));memset(range,0,sizeof(range));
        } else assert(false);
    }
    assert(!depth);return out;
}
int main() {
    Device d;assert(d.newReportDescriptor(nullptr)==kIOReturnBadArgument);
    IOMemoryDescriptor *md=nullptr;assert(d.newReportDescriptor(&md)==0 && md);
    Parsed combined=parse(md->bytes,md->length);
    // IOHIDDevice/IOHIDEventService derive primary usage from pair zero.
    assert(combined.apps==2 && combined.pages[0]==0xd && combined.usages[0]==4);
    // The retained keyboard personality matches ANY DeviceUsagePairs entry.
    bool keyboardMatch=false;for(unsigned i=0;i<combined.apps;++i)keyboardMatch|=combined.pages[i]==1 && combined.usages[i]==6;
    assert(keyboardMatch);
    unsigned char old[sizeof(descriptor)+sizeof(touchDescriptor)];
    memcpy(old,descriptor,sizeof(descriptor));memcpy(old+sizeof(descriptor),touchDescriptor,sizeof(touchDescriptor));
    Parsed before=parse(old,sizeof(old));assert(before.pages[0]==1 && before.usages[0]==6);
    // Compare every Input item's bit offset, globals, usages and flags, not just
    // total size. Collection order must not change any emitted report layout.
    for(unsigned id=1;id<=3;++id)assert(!memcmp(&combined.reports[id],&before.reports[id],sizeof(Report)));
    assert(combined.reports[1].bits==64 && combined.reports[2].bits==8 && combined.reports[3].bits==488);
    for(unsigned finger=0;finger<10;++finger) {
        const Field *f=combined.reports[3].fields+finger*6;
        assert(f[0].page==0xd && f[0].usages[0]==0x42 && f[0].bit==finger*48);
        assert(f[1].usages[0]==0x47 && f[3].usages[0]==0x51 && f[3].maximum==255);
        assert(f[4].page==1 && f[4].usages[0]==0x30 && f[4].maximum==32767 && f[5].usages[0]==0x31);
    }
    d.touchEnabled=false;assert(d.newReportDescriptor(&md)==0);Parsed keys=parse(md->bytes,md->length);
    assert(keys.apps==1 && keys.pages[0]==1 && keys.usages[0]==6 && !keys.reports[3].count);
    d.touchOnly=true;assert(d.newReportDescriptor(&md)==0);Parsed touch=parse(md->bytes,md->length);
    assert(touch.apps==1 && touch.pages[0]==0xd && touch.usages[0]==4 && !touch.reports[1].count);
    assert(!memcmp(&touch.reports[3],&combined.reports[3],sizeof(Report)));
    puts("PASS initial Touch Screen primary usage, keyboard matching pair, all report fields unchanged, ten fingers, keyboard-only and touch-only");
}
