// Production HID delivery and timer bodies; IOKit submission and timer are models.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
using IOReturn = int;
static constexpr IOReturn kIOReturnSuccess=0,kIOReturnNoMemory=-1;
static constexpr unsigned TOUCH_FINGERS=10,kIODirectionNone=0;
static volatile uint32_t native_touch_seq,native_touch_count,native_touch_hid_reports;
static volatile int native_touch_xs[12],native_touch_ys[12],native_touch_ids[12];
namespace edge {static uint32_t native_touch_hid_multi[3];}
static bool allocationFails;
static int submissionResult,results[8];
static unsigned reports[8],failures[8],failureLogs;
static unsigned submissions;
static unsigned char submitted[62];
struct OSObject {};
struct IOBufferMemoryDescriptor {
    unsigned char bytes[62];
    static IOBufferMemoryDescriptor *withBytes(const void *p,unsigned n,unsigned) {
        assert(n==62);
        if(allocationFails)return nullptr;
        auto *buffer=new IOBufferMemoryDescriptor;
        memcpy(buffer->bytes,p,n);return buffer;
    }
};
struct IOTimerEventSource {unsigned delay=0;void setTimeoutMS(unsigned ms){delay=ms;}};
#define OSDynamicCast(type,object) static_cast<type *>(object)
#define OSSafeReleaseNULL(pointer) do {delete pointer;pointer=nullptr;} while(0)
static void IOLog(const char *format,...){(void)format;++failureLogs;}
#include "observer-bodies.h"
static void native_hid_live_checkpoint(OSObject *){}
static bool native_hid_input_requested(){return false;}
struct NativeHidInput:OSObject {
    bool ready=true,touchOnly=false,down=false;
    unsigned buttons=0;
    bool hasEventDriverCached(){return ready;}
    static bool hasEventDriver(NativeHidInput *,unsigned){return true;}
    IOReturn submitReport(IOBufferMemoryDescriptor *buffer) {
        ++submissions;memcpy(submitted,buffer->bytes,sizeof(submitted));return submissionResult;
    }
    #include "delivery-bodies.h"
};
static void snapshot(unsigned count,int id=1,int x=15240,int y=10160) {
    native_touch_seq+=2;native_touch_count=count;
    if(count){native_touch_ids[0]=id;native_touch_xs[0]=x;native_touch_ys[0]=y;}
}
int main() {
    NativeHidInput device;
    IOTimerEventSource timer;
    snapshot(1);
    allocationFails=true;device.touchSend();
    assert(device.lastSeq==0 && device.prevIds[0]==-1 && device.touchRetry && results[6]==kIOReturnNoMemory);
    allocationFails=false;submissionResult=-2;device.touchSend();
    assert(device.lastSeq==0 && device.prevIds[0]==-1 && device.touchRetry);
    // The existing one-second timer retries a stationary failed press without a new snapshot.
    submissionResult=0;NativeHidInput::tick(&device,&timer);
    assert(device.lastSeq==native_touch_seq && device.prevIds[0]==1 && !device.touchRetry && timer.delay==1000);
    unsigned attempts=submissions;NativeHidInput::tick(&device,&timer);
    assert(submissions==attempts); // no recurring delivery work after success
    // A failed lift retains the down finger; retry sends its explicit tip-up.
    snapshot(0);submissionResult=-2;device.touchSend();
    uint32_t liftSeq=native_touch_seq;
    assert(device.prevIds[0]==1 && device.lastSeq!=liftSeq && device.touchRetry);
    submissionResult=0;device.touchOnly=true;NativeHidInput::tick(&device,&timer);
    assert(device.lastSeq==liftSeq && device.prevIds[0]==-1 && !device.touchRetry);
    assert(submitted[1]==2 && submitted[2]==1 && submitted[61]==1);
    // A newer snapshot supersedes a failed press rather than replaying stale coordinates.
    snapshot(1,2,0,0);submissionResult=-2;device.touchSend();
    snapshot(1,3,30479,20319);submissionResult=0;NativeHidInput::tick(&device,&timer);
    assert(device.prevIds[0]==3 && submitted[2]==3 && submitted[3]==255 && submitted[4]==127);
    // A failed press followed by a lift must not manufacture a finger that was never accepted.
    NativeHidInput neverSent;submissionResult=-2;snapshot(1,4);neverSent.touchSend();
    snapshot(0);attempts=submissions;NativeHidInput::tick(&neverSent,&timer);
    assert(submissions==attempts && !neverSent.touchRetry && neverSent.lastSeq==native_touch_seq);
    // Late failures remain visible after many successful reports; log at most eight.
    failureLogs=reports[6]=failures[6]=0;
    for(unsigned i=0;i<1000;++i)native_hid_input_observed(6,0);
    assert(failureLogs==0);
    for(unsigned i=0;i<9;++i)native_hid_input_observed(6,-2);
    assert(failureLogs==8 && failures[6]==9 && reports[6]==1009);
    puts("PASS: allocation/submit failure preserves delivery state, stationary press/lift timer retries, latest snapshot and idle suppression");
}
