#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual bounded USB PHY detection lease and timer sleep."""
from pathlib import Path
import argparse,re,subprocess,json,hashlib
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--xnu',type=Path,required=True);args=p.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=False)
patch=R/'patches/picasso-usb-console-xnu.patch';text=patch.read_text()
a=text.index('@@ -0,0 +1,');b=text.index('\ndiff --git ',a);lines=text[a:b].splitlines()
assert len(lines)-1==int(re.search(r'\+1,(\d+)',lines[0])[1]);source=(args.xnu/'bsd/dev/picasso_usb.c').read_text()
def function(name):
 a=source.index('\n'+name+'(');a=source.rfind('\n',0,a-1)+1;b=source.index('{',a)+1;n=1
 while n:
  n+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
constants=source[source.index('static uintptr_t charger_phy,charger_eud;'):source.index('static int\nud_charger_restore')]
(out/'production.h').write_text('static bool pmic_vbus_present = true;\nstatic bool pmic_detach_pending;\n'+constants+function('ud_charger_restore')+'\n'+function('picasso_usb_vbus_update')+'\n'+function('picasso_usb_charger_detect')+'\n'+function('ud_sleep_ms'))
f=r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#define NSEC_PER_MSEC 1000000ULL
#define QSCRATCH_OFFSET 0x1000u
#define QSCRATCH_HS_PHY_CTRL 0x10u
#define UTMI_OTG_VBUS_VALID (1u<<20)
#define GEVNTCOUNT 0xc40cu
#define UD_EVT_SIZE 4096u
#define EP_RX 2
#define EP_TX 3
#define EP_NOTIFY 5
#define DEPCMD_ENDTRANSFER 8
#define DEPCMD_FORCE_RM 0x800
#define DEPCMD_CMDIOC 0x100
#define UD_EPCMD_NO_RESOURCE -2
#define THREAD_UNINT 1
#define THREAD_CONTINUE_NULL nullptr
enum {acquire,release};
static bool pur_live=true;
static struct {unsigned poll_lock=0;bool panic_mode=false,configured=false,host_open=false;unsigned sleep_chan=0;uintptr_t dwc=3;
bool cmd_pending=false,tx_busy=false,rx_busy=false,notify_busy=false,rx_transfer_started=false,ep0_busy=false,ep0_ready=false;
unsigned cmd_len=0,resource[6]={},ep0_active_ep=0,evt_pos=0;}ud;
static uint32_t hs=UTMI_OTG_VBUS_VALID|0x40000000u,eventCount=0;static int epError=0,rebuildError=0;
static bool setupError=false;static unsigned epCalls=0,rebuilds=0;
static uint32_t dwc_rd(unsigned reg){assert(ud.poll_lock&&reg==GEVNTCOUNT);return eventCount;}
static void dwc_wr(unsigned reg,uint32_t v){assert(ud.poll_lock&&reg==GEVNTCOUNT&&v==eventCount);eventCount=0;}
static int ud_epcmd(unsigned ep,uint32_t cmd,unsigned,unsigned,unsigned){assert(ud.poll_lock&&ep<6&&(cmd&255)==DEPCMD_ENDTRANSFER);++epCalls;return epError;}
static int ud_ep0_rebuild(const char *){assert(ud.poll_lock&&!ud.configured&&!ud.host_open);++rebuilds;return rebuildError;}
static void ud_start_setup(){assert(ud.poll_lock);ud.ep0_busy=!setupError;}
static uint32_t regs[256]={};static uint32_t eud=0;static uint64_t now=1000;
static unsigned writes=0,ignore=0,blocks=0,waits=0;static uint64_t waited=0;
static std::vector<unsigned> order;
static bool os_atomic_cmpxchg(unsigned *p,unsigned old,unsigned next,int){if(*p!=old)return false;*p=next;return true;}
static void os_atomic_store(unsigned *p,unsigned value,int){*p=value;}
static uint32_t usb_rd(uintptr_t base,unsigned reg){assert(ud.poll_lock==1);if(base==ud.dwc+QSCRATCH_OFFSET){assert(reg==QSCRATCH_HS_PHY_CTRL);return hs;}assert(base==1||base==2);return base==2?eud:regs[reg];}
static void usb_wr(uintptr_t base,unsigned reg,uint32_t v){assert(ud.poll_lock==1);++writes;order.push_back(reg);if(writes==ignore)return;if(base==ud.dwc+QSCRATCH_OFFSET){assert(reg==QSCRATCH_HS_PHY_CTRL);hs=v;return;}assert(base==1);regs[reg]=v;}
static void clock_interval_to_deadline(unsigned ms,uint64_t scale,uint64_t *d){assert(scale==NSEC_PER_MSEC);*d=now+ms*scale;}
static int assert_wait_deadline(unsigned *event,int mode,uint64_t d){assert(!ud.poll_lock&&event==&ud.sleep_chan&&mode==THREAD_UNINT);++waits;waited=d;return 0;}
static void thread_block(void *continuation){assert(!ud.poll_lock&&continuation==nullptr&&waits==blocks+1);++blocks;now=waited;}
#include "production.h"
static void reset(){ud={};pmic_vbus_present=true;pmic_detach_pending=false;hs=UTMI_OTG_VBUS_VALID|0x40000000u;eventCount=0;epError=rebuildError=0;setupError=false;epCalls=rebuilds=0;pur_live=true;charger_phy=1;charger_eud=2;eud=0;charger_deadline=0;writes=ignore=0;order.clear();
 regs[0x64]=0xf3;regs[0x3c]=0xe7;regs[0x94]=0xa0;}
int main(){
 reset();assert(!picasso_usb_charger_detect(1));assert(charger_deadline==now+2000*NSEC_PER_MSEC);
 assert((regs[0x64]&12)==12&&(regs[0x3c]&24)==8&&(regs[0x94]&1)==1);
 assert(order==std::vector<unsigned>({0x64,0x3c,0x94}));
 assert(picasso_usb_charger_detect(1)==-57);assert(!ud.poll_lock);
 // Unrelated changes survive restoration; only the three owned masks revert.
 regs[0x64]^=0x80;assert(!picasso_usb_charger_detect(0));assert(!charger_deadline);
 assert(regs[0x64]==0x73&&regs[0x3c]==0xe7&&regs[0x94]==0xa0);
 unsigned old=writes;assert(!picasso_usb_charger_detect(0)&&writes==old);
 for(unsigned mode=0;mode<7;++mode){reset();if(mode==0)ud.configured=true;if(mode==1)ud.host_open=true;
  if(mode==2)ud.panic_mode=true;if(mode==3)charger_phy=0;if(mode==4)pur_live=false;if(mode==5)eud=1;if(mode==6)charger_eud=0;
  assert(picasso_usb_charger_detect(1)<0&&!writes&&!charger_deadline&&!ud.poll_lock);}
 reset();ud.poll_lock=1;assert(picasso_usb_charger_detect(1)==-53&&!writes&&ud.poll_lock==1);
 for(unsigned n=1;n<=3;++n){reset();ignore=n;assert(picasso_usb_charger_detect(1)==-54);
  assert(!charger_deadline&&!ud.poll_lock&&regs[0x64]==0xf3&&regs[0x3c]==0xe7&&regs[0x94]==0xa0);}
 reset();assert(!picasso_usb_charger_detect(1));ignore=writes+1;assert(picasso_usb_charger_detect(0)==-54&&charger_deadline);
 // Failed restoration keeps its lease live for the normal poller to retry.
 ud.poll_lock=1;assert(!ud_charger_restore()&&!charger_deadline);ud.poll_lock=0;
 // A real sampled detach releases a stale configured host without a DWC event.
 reset();ud.configured=ud.host_open=true;ud.cmd_pending=true;ud.cmd_len=12;ud.resource[0]=1;ud.resource[EP_RX]=2;ud.resource[EP_TX]=3;
 assert(!picasso_usb_vbus_update(0));assert(!pmic_vbus_present&&!pmic_detach_pending&&!ud.configured&&!ud.host_open&&!ud.cmd_pending);
 assert(epCalls==3&&hs==0x40000000u&&!ud.resource[EP_RX]);assert(picasso_usb_charger_detect(1)==-58);
 eventCount=16;assert(!picasso_usb_vbus_update(1));assert(pmic_vbus_present&&ud.ep0_busy&&rebuilds==1&&eventCount==0&&ud.evt_pos==16);
 assert(hs==(UTMI_OTG_VBUS_VALID|0x40000000u));assert(!picasso_usb_charger_detect(1));assert(!picasso_usb_charger_detect(0));
 // Repeated attached reports leave a current data session undisturbed.
 ud.configured=ud.host_open=true;old=writes;assert(!picasso_usb_vbus_update(1)&&writes==old&&ud.configured&&ud.host_open);
 assert(picasso_usb_charger_detect(1)==-55);
 reset();ud.configured=true;ud.poll_lock=1;assert(picasso_usb_vbus_update(0)==-53&&ud.configured&&!writes);ud.poll_lock=0;
 reset();ignore=1;assert(picasso_usb_vbus_update(0)==-54&&pmic_detach_pending&&!pmic_vbus_present);
 assert(!picasso_usb_vbus_update(0)&&!pmic_detach_pending);
 reset();ud.resource[EP_RX]=2;epError=-1;assert(picasso_usb_vbus_update(0)==-59&&pmic_detach_pending&&ud.resource[EP_RX]==2);
 assert(picasso_usb_vbus_update(1)==-59&&!pmic_vbus_present);epError=0;assert(!picasso_usb_vbus_update(0)&&!ud.resource[EP_RX]);
 rebuildError=-1;assert(picasso_usb_vbus_update(1)==-60&&!pmic_vbus_present);rebuildError=0;setupError=true;
 assert(picasso_usb_vbus_update(1)==-60&&!pmic_vbus_present);setupError=false;assert(!picasso_usb_vbus_update(1));
 reset();ud.resource[EP_TX]=3;epError=UD_EPCMD_NO_RESOURCE;assert(!picasso_usb_vbus_update(0)&&!ud.resource[EP_TX]);
 reset();assert(!picasso_usb_charger_detect(1));assert(!picasso_usb_vbus_update(0)&&!charger_deadline);
 now=1000;ud_sleep_ms(1);assert(now==1001000&&waits==1&&blocks==1);
 puts("PASS actual USB detection API: configured-host exclusion, lock ownership, masked PHY handoff/restore, readback failures, retry and blocking timer sleep");
}
"""
(out/'test.cpp').write_text(f);runs=[]
assert 'if (charger_deadline && mach_absolute_time() >= charger_deadline)\n\t\t(void)ud_charger_restore();\n\tud_poll();' in source
assert 'if (!pmic_vbus_present) return;' in function('ud_poll')
thread=function('ud_thread');assert 'ud_sleep_ms(1);' in thread and 'busy_wait' not in thread
for variant,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
 for phase,cmd in [('compile',['clang++-18','-std=c++17','-Wall','-Wextra','-Werror','-O1',*flags,'-I',str(out),str(out/'test.cpp'),'-o',str(out/variant)]),('run',[str(out/variant)])]:
  q=subprocess.run(cmd,capture_output=True,text=True);(out/f'{variant}-{phase}.log').write_text(q.stdout+q.stderr)
  assert not q.returncode,q.stdout+q.stderr;runs.append(dict(variant=variant,phase=phase,passed=True,output=q.stdout))
(out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,inputs={str(p.relative_to(R)) if p.is_relative_to(R) else str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [patch,Path(__file__),args.xnu/'bsd/dev/picasso_usb.c']}),indent=2)+'\n')
print('PASS USB charger lease and timer sleep; host peers only')
