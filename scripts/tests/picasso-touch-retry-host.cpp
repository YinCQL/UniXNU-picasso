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
namespace edge {static unsigned native_touch_hid_multi[4],digitizerDispatches;}
static void host_fence(){if(tear){++native_touch_seq;tear=false;}}
static int results[8];static unsigned native_hid_input_events[8],reportErrors[8],errorLogs;
static void IOLog(const char *,...){++errorLogs;}
#include "touch-observed.inc"
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
    unsigned char history[128][62]={};unsigned acceptedCount=0;
    bool hasEventDriverCached(){return ready;}
    int submitReport(IOBufferMemoryDescriptor *b){++submits;if(!result){assert(acceptedCount<128);memcpy(history[acceptedCount++],b->bytes,62);memcpy(accepted,b->bytes,62);}return result;}
#include "touch.inc"
};
static void frame(unsigned n,int first){native_touch_seq+=2;native_touch_count=n;for(unsigned i=0;i<n;++i){native_touch_ids[i]=first+(int)i;native_touch_xs[i]=100;native_touch_ys[i]=200;}}
int main(){
    for(unsigned i=0;i<60;++i)native_hid_input_observed(6,0);
    assert(!errorLogs);
    for(unsigned i=0;i<12;++i)native_hid_input_observed(6,-8);
    assert(errorLogs==8 && results[6]==-8 && native_hid_input_events[6]==72);
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
    // A lift that failed must reach the family before a newer down reuses its ID.
    // Re-reading only the shared latest snapshot silently loses this boundary.
    NativeHidInput retry;frame(1,1);retry.touchSend();
    frame(0,0);retry.result=-8;retry.touchSend();
    frame(1,1);retry.result=0;retry.touchSend();
    assert(retry.acceptedCount==3 && retry.history[1][1]==2 && retry.history[1][2]==1);
    assert(retry.history[2][1]==3 && retry.history[2][2]==1 && !retry.touchRetry);
    // An allocation failure has the same ordering obligation, including a new ID.
    frame(0,0);failAllocation=true;retry.touchSend();failAllocation=false;
    frame(1,2);retry.touchSend();
    assert(retry.acceptedCount==5 && retry.history[3][1]==2 && retry.history[3][2]==1);
    assert(retry.history[4][1]==3 && retry.history[4][2]==2);
    // Virtual ID 255 must be released when a physical ID 1 takes over mid-swipe.
    NativeHidInput takeover;frame(1,255);takeover.touchSend();frame(1,1);takeover.touchSend();
    assert(takeover.accepted[1]==3 && takeover.accepted[2]==1);
    assert(takeover.accepted[7]==2 && takeover.accepted[8]==255);
    assert(!live);puts("PASS down/lift retries, frozen failed lift before same/new ID press, allocation/not-ready, torn snapshot and virtual/physical identity");
}
