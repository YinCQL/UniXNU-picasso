// Production publication bodies with an in-memory HID sink; no IOKit or hardware.
#include <assert.h>
#include <stdio.h>
extern "C" {
#include "NvtTouchCore.h"
#include "TouchBoard.h"
#include "TouchPublish.h"
volatile uint32_t native_touch_seq, native_touch_count, native_touch_writer_lock;
volatile uint32_t native_touch_physical_active;   /* diagnostics counter in publish() */
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
}
static unsigned autoStop, notifications;
static void *touchContext;
static void sink(void *) { assert(native_touch_writer_lock==0 && !(native_touch_seq&1)); ++notifications; }
static void (*touchSink)(void *)=sink;
#include "producer-bodies.h"
int main() {
    nvt_contact finger={};finger.x=5400;finger.y=12000;finger.tracking_id=3;
    assert(publish(nullptr,0) && notifications==0 && autoStop==0);
    publishContact(1,20100);assert(native_touch_count==1 && notifications==1);
    assert(publish(nullptr,0) && native_touch_count==1 && notifications==1);
    native_touch_writer_lock=1;
    assert(!publish(&finger,1) && autoStop==2 && notifications==1);
    native_touch_writer_lock=0;
    assert(publish(&finger,1) && native_touch_ids[0]==3 && notifications==2);
    uint32_t sequence=native_touch_seq;
    publishContact(0,0);
    assert(native_touch_count==1 && native_touch_ids[0]==3 && native_touch_seq==sequence && notifications==2);
    native_touch_writer_lock=1;
    assert(!publish(nullptr,0) && native_touch_count==1 && notifications==2);
    native_touch_writer_lock=0;
    assert(publish(nullptr,0) && native_touch_count==0 && notifications==3);
    assert(publish(nullptr,0) && notifications==3);
    puts("PASS: idle polls preserve virtual input, physical takeover excludes stale virtual releases, busy press/lift retries");
}
