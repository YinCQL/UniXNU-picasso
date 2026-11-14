// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static int gWillShutdown,gSleepOrShutdownPending;
static unsigned services,lights,sleeps;
static bool OSCompareAndSwap(int old,int value,int *target){if(*target!=old)return false;*target=value;return true;}
struct IOService {static void willShutdown(){assert(gWillShutdown==1);++services;}};
static void light(){assert(gWillShutdown==1&&services==1&&!sleeps);++lights;}
static void (*picasso_backlight_shutdown)()=light;
static void IOSleep(unsigned ms){assert(ms==100);assert(!EXPECT_HOOK||!picasso_backlight_shutdown||lights==1);++sleeps;}
#include ROOT_BODY
static void reset(){gWillShutdown=0;gSleepOrShutdownPending=0;services=lights=sleeps=0;picasso_backlight_shutdown=light;}
int main(){
 reset();assert(!lights);IOPMRootDomainWillShutdown();assert(services==1&&lights==EXPECT_HOOK&&!sleeps&&gSleepOrShutdownPending==1);
 IOPMRootDomainWillShutdown();assert(services==1&&lights==EXPECT_HOOK&&!sleeps);
 reset();gSleepOrShutdownPending=1;IOPMRootDomainWillShutdown();assert(services==1&&lights==EXPECT_HOOK&&sleeps==100);
 reset();picasso_backlight_shutdown=nullptr;IOPMRootDomainWillShutdown();assert(services==1&&!lights&&!sleeps);
 reset();gWillShutdown=1;IOPMRootDomainWillShutdown();assert(!services&&!lights&&!sleeps);
 puts(EXPECT_HOOK?"PASS committed backlight callback precedes waits, once only, null hook and prior commit preserve OS control flow":"PASS old root-domain body leaves backlight untouched throughout commit and wait (baseline reproduced)");
}
