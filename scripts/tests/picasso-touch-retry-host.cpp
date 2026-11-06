// Real touch consumer with failing report and descriptor stand-ins.
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum {TOUCH_FINGERS=10,kIOReturnSuccess=0,kIOReturnNoMemory=-1,kIODirectionNone=0};
using IOReturn=int;
static uint32_t native_touch_seq;static unsigned native_touch_count;
static int native_touch_ids[10],native_touch_xs[10],native_touch_ys[10];
static unsigned native_touch_hid_reports,submits,live;static bool failAllocation,tear;
namespace edge {static unsigned native_touch_hid_multi[4];}
static void host_fence(){if(tear){++native_touch_seq;tear=false;}}
static void native_hid_input_observed(int kind,int){assert(kind==6);}
struct IOBufferMemoryDescriptor {
    unsigned char bytes[62];
    static IOBufferMemoryDescriptor *withBytes(const void *p,size_t n,int dir){
        assert(n==62 && dir==0);if(failAllocation)return nullptr;
        auto *b=new IOBufferMemoryDescriptor;memcpy(b->bytes,p,n);++live;return b;
    }
    void release(){--live;delete this;}
};
#define OSSafeReleaseNULL(p) do{if(p){p->release();p=nullptr;}}while(0)
struct NativeHidInput {
    bool ready=true;int result=0;unsigned char accepted[62]={};
    bool hasEventDriverCached(){return ready;}
    int submitReport(IOBufferMemoryDescriptor *b){++submits;if(!result)memcpy(accepted,b->bytes,62);return result;}
#include "touch.inc"
};
static void frame(unsigned n,int first){native_touch_seq+=2;native_touch_count=n;for(unsigned i=0;i<n;++i){native_touch_ids[i]=first+(int)i;native_touch_xs[i]=100;native_touch_ys[i]=200;}}
int main(){
    NativeHidInput h;frame(1,4);h.result=-9;h.touchSend();
    assert(h.lastSeq==0 && h.prevIds[0]==-1 && h.touchRetry && !live);
    h.result=0;h.touchSend();assert(h.lastSeq==native_touch_seq && h.prevIds[0]==4 && !h.touchRetry);
    unsigned count=submits;h.touchSend();assert(submits==count);
    frame(0,0);h.result=-8;h.touchSend();assert(h.prevIds[0]==4 && h.touchRetry);
    h.result=0;h.touchSend();assert(h.prevIds[0]==-1 && h.accepted[1]==2 && h.accepted[2]==4 && h.accepted[61]==1);
    frame(1,8);failAllocation=true;h.touchSend();assert(h.touchRetry && h.prevIds[0]==-1);
    failAllocation=false;h.ready=false;h.touchSend();assert(h.touchRetry && h.prevIds[0]==-1);
    h.ready=true;h.touchSend();assert(!h.touchRetry && h.prevIds[0]==8);
    frame(2,8);h.touchSend();frame(1,9);h.touchSend();assert(h.accepted[1]==3 && h.accepted[2]==9 && h.accepted[7]==2 && h.accepted[8]==8);
    frame(1,2);tear=true;count=submits;h.touchSend();assert(h.touchRetry && submits==count && h.prevIds[0]==9);
    h.touchSend();assert(submits==count);++native_touch_seq;h.touchSend();assert(h.prevIds[0]==2 && !h.touchRetry);
    assert(!live);puts("PASS down/lift failure retries, allocation/not-ready, duplicate and torn snapshot, finger identity");
}
