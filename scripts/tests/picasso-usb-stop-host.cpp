// Stock-like event acknowledgement is required for the fake core to halt.
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
using vm_offset_t=uintptr_t;
struct {vm_offset_t dwc;} ud;
enum {DEVTEN=0,DCTL=1,DSTS=2,GEVNTCOUNT=3,GUSB2PHYCFG0=4};
enum : uint32_t {DCTL_RUN_STOP=1u<<31,DSTS_DEVCTRLHLT=1,DCTL_CSFTRST=1u<<30,GUSB2PHYCFG_SUSPHY=64,GUSB2PHYCFG_ENBLSLPM=256};
static uint32_t regs[5];static unsigned waits,acks,writes;static bool stuck,resetStuck;
static uint32_t dwc_rd(unsigned n){assert(ud.dwc);return regs[n];}
static void dwc_wr(unsigned n,uint32_t v){assert(ud.dwc);++writes;if(n==GEVNTCOUNT){assert(v==regs[n]);regs[n]=0;++acks;}else regs[n]=v;}
static void picasso_usb_busy_wait(unsigned us){assert(us==1000);++waits;if(!stuck && !regs[GEVNTCOUNT] && !(regs[DCTL]&DCTL_RUN_STOP))regs[DSTS]|=DSTS_DEVCTRLHLT;if(!resetStuck)regs[DCTL]&=~DCTL_CSFTRST;}
#include "usb-stop.inc"
static void setup(){ud.dwc=1;waits=acks=writes=0;stuck=resetStuck=false;regs[DEVTEN]=~0u;regs[DCTL]=DCTL_RUN_STOP;regs[DSTS]=0;regs[GEVNTCOUNT]=16;regs[GUSB2PHYCFG0]=~0u;}
int main(){
    picasso_usb_disconnect_for_reset();assert(!writes);
    setup();picasso_usb_stop_controller();assert(acks==1 && waits==1 && (regs[DSTS]&1) && !regs[DEVTEN]);
    setup();picasso_usb_disconnect_for_reset();assert(acks==1 && waits==2 && !(regs[DCTL]&DCTL_CSFTRST) && !(regs[GUSB2PHYCFG0]&(64|256)));
    setup();stuck=resetStuck=true;picasso_usb_disconnect_for_reset();assert(waits==200 && acks==1);
    puts("PASS USB event acknowledgement, disconnect/reset ordering, bounded halt/reset timeout, unmapped core");
}
