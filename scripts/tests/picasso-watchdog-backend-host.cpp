/* Real extracted backend, with simulated MMIO/IRQ and OS helpers. */
#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "PicassoWatchdogBackend.h"

/* Kernel _Atomic syntax plus real host atomic operations; no fake lock success. */
#define _Atomic
static uint32_t *wdt_lock_address;
static thread_local bool current_wdt_owned;
static uint32_t atomic_load_host(const uint32_t *p) {
#ifdef _MSC_VER
 return (uint32_t)_InterlockedCompareExchange((volatile long *)p,0,0);
#else
 return __atomic_load_n(p,__ATOMIC_ACQUIRE);
#endif
}
static void atomic_store_host(uint32_t *p,uint32_t value) {
 if(p==wdt_lock_address && value==0) current_wdt_owned=false;
#ifdef _MSC_VER
 (void)_InterlockedExchange((volatile long *)p,(long)value);
#else
 __atomic_store_n(p,value,__ATOMIC_RELEASE);
#endif
}
static bool atomic_compare_host(uint32_t *p,uint32_t old_value,uint32_t value) {
#ifdef _MSC_VER
 bool result=(uint32_t)_InterlockedCompareExchange((volatile long *)p,(long)value,(long)old_value)==old_value;
#else
 bool result=__atomic_compare_exchange_n(p,&old_value,value,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
#endif
 if(result && p==wdt_lock_address) current_wdt_owned=true;
 return result;
}
#define os_atomic_load(p,o) atomic_load_host(p)
#define os_atomic_store(p,v,o) atomic_store_host(p,v)
#define os_atomic_cmpxchg(p,a,b,o) atomic_compare_host(p,a,b)
typedef bool boolean_t;
static thread_local bool irq_enabled=true;
static boolean_t ml_set_interrupts_enabled(bool enabled) {
 bool prior=irq_enabled;irq_enabled=enabled;return prior;
}
struct {
 bool watchdog,watchdog_armed,panic_mode,panic_pending,reset_pending,reboot_pending,hang_pending;
 bool cmd_pending,configured,host_open,live,hold;
 uint32_t poll_lock,reset_mode;
 uintptr_t wdt;
 unsigned long long commands,events,chars,rx_bytes;
 char cmd[80];
} ud;
static bool pur_live=true,pu_panic_active;
static uint32_t pur_dropped;
static volatile uint64_t armcpu_timer_interrupts;
#define NSEC_PER_MSEC 1000000ULL
#define PU_PANIC_RING_SIZE (128u * 1024u)
static uint32_t usb_rd(uintptr_t,unsigned);
static void usb_wr(uintptr_t,unsigned,uint32_t);
static void ud_poll(void);
static void ud_finish_reboot(void);
static void ud_finish_hang(void);
static void panic(const char *);
static void pup_puts(const char *,int);
void picasso_usb_panic_flush(void);
static int backend_log(const char *,...);
static uint64_t mach_absolute_time(void);
static void absolutetime_to_nanoseconds(uint64_t,uint64_t *);
#define printf backend_log
#include "backend-production.inc"
#undef printf

struct Write { unsigned off;uint32_t value; };
static uint32_t regs[6];
static std::vector<Write> writes;
static std::vector<std::string> logs;
static unsigned read_count,polls,reboots,hangs,panics,flushes,markers;
static uint64_t now_ns;
static int owner_a,owner_b;
enum Hook { NoHook,TerminalHook,ReenterHook,ForceHook };
static Hook hook;
static unsigned hook_offset;
static bool hook_on_write;
static int ignored_offset=-1;
static int corrupt_after_rst;
static bool reenter_logs,in_log;
static const char *next_command;
static void dispatch_hook(unsigned off,bool is_write) {
 if(hook==NoHook || hook_offset!=off || hook_on_write!=is_write) return;
 Hook action=hook;hook=NoHook;
 assert(!irq_enabled && atomic_load_host(&picasso_wdt_lock)==1);
 if(action==TerminalHook) picasso_usb_watchdog_terminal();
 if(action==ForceHook) assert(!picasso_usb_watchdog_force_bite());
 if(action==ReenterHook) {
  size_t count=writes.size();
  assert(!picasso_watchdog_backend_claim(&owner_b));
  assert(!picasso_watchdog_backend_enable(&owner_a));
  assert(!picasso_watchdog_backend_disable(&owner_a));
  assert(!picasso_watchdog_backend_pet(&owner_a));
  assert(!picasso_usb_watchdog_pet());
  assert(writes.size()==count && !irq_enabled);
 }
}
static void mmio_guard(uintptr_t base,unsigned off) {
 assert(base==0x17c10000 && !irq_enabled && current_wdt_owned && atomic_load_host(&picasso_wdt_lock)==1);
 assert(off==APSS_WDT_RST || off==APSS_WDT_EN || off==APSS_WDT_BARK || off==APSS_WDT_BITE);
}
static uint32_t usb_rd(uintptr_t base,unsigned off) {
 mmio_guard(base,off);assert(off!=APSS_WDT_RST);++read_count;
 dispatch_hook(off,false);return regs[off/4];
}
static void usb_wr(uintptr_t base,unsigned off,uint32_t value) {
 mmio_guard(base,off);writes.push_back({off,value});
 if(off==APSS_WDT_RST) {
  assert(value==1);
  if(corrupt_after_rst) regs[APSS_WDT_EN/4]=3;
 } else if((int)off!=ignored_offset) regs[off/4]=value;
 dispatch_hook(off,true);
}
static void ud_poll(void) {
 ++polls;
 if(next_command) {
  std::strcpy(ud.cmd,next_command);next_command=nullptr;
  ud.cmd_pending=true;ud_process_command();
 }
}
static void ud_finish_reboot(void) { assert(ud.poll_lock==0);++reboots; }
static void ud_finish_hang(void) { assert(ud.poll_lock==0);++hangs; }
static void panic(const char *) { ++panics;picasso_usb_panic_begin(); }
static void pup_puts(const char *text,int size) {
 assert(size>0 && std::strstr(text,"PANIC BEGIN"));++markers;
}
void picasso_usb_panic_flush(void) { ++flushes; }
static int backend_log(const char *format,...) {
 assert(!current_wdt_owned);
 char text[640];va_list args;va_start(args,format);
 int count=std::vsnprintf(text,sizeof(text),format,args);va_end(args);
 assert(count>0 && (size_t)count<sizeof(text));logs.emplace_back(text);
 if(reenter_logs && !in_log) {in_log=true;picasso_usb_console_drive_poll();in_log=false;}
 return count;
}
static uint64_t mach_absolute_time(void) {return now_ns;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *ns) {*ns=value;}
static void reset(void) {
 wdt_lock_address=&picasso_wdt_lock;
 ud={};ud.watchdog=true;ud.wdt=0x17c10000;
 regs[APSS_WDT_EN/4]=1;regs[APSS_WDT_BARK/4]=regs[APSS_WDT_BITE/4]=PICASSO_WDT_TICKS;
 ud.watchdog_armed=true;picasso_wdt_owner=nullptr;picasso_wdt_forced=false;
 atomic_store_host(&picasso_wdt_lock,0);atomic_store_host(&picasso_wdt_terminal,0);
 irq_enabled=true;pu_panic_active=false;pur_live=true;
 writes.clear();logs.clear();read_count=polls=reboots=hangs=panics=flushes=markers=0;
 hook=NoHook;ignored_offset=-1;corrupt_after_rst=0;reenter_logs=in_log=false;
 next_command=nullptr;now_ns=5000000000ULL;
}
static void owns(void) {assert(picasso_watchdog_backend_claim(&owner_a));writes.clear();}
static void no_ordinary_writes(void) {
 size_t before=writes.size();
 assert(!picasso_usb_watchdog_pet());picasso_usb_watchdog_arm();picasso_usb_watchdog_disable();
 for(const void *owner : {(const void *)nullptr,(const void *)&owner_a,(const void *)&owner_b}) {
  assert(!picasso_watchdog_backend_claim(owner));assert(!picasso_watchdog_backend_enable(owner));
  assert(!picasso_watchdog_backend_disable(owner));assert(!picasso_watchdog_backend_pet(owner));
 }
 assert(writes.size()==before);
}
static bool contains(const char *part) {
 for(const auto &line:logs) if(line.find(part)!=std::string::npos)return true;
 return false;
}
static void passed(const char *name) {std::printf("PASS: %s\n",name);std::fflush(stdout);}

int main(void) {
 reset();ud.watchdog_armed=false;regs[APSS_WDT_EN/4]=0;picasso_usb_watchdog_arm();
 assert(ud.watchdog_armed && writes.size()==5 && irq_enabled);
 assert(writes[0].off==APSS_WDT_EN && writes[0].value==0);
 assert(writes[1].off==APSS_WDT_RST && writes[2].off==APSS_WDT_BARK && writes[2].value==491519);
 assert(writes[3].off==APSS_WDT_BITE && writes[3].value==491519 && writes[4].value==1);
 assert(picasso_usb_watchdog_pet() && writes.size()==6);
 passed("boot arm retains the five-write 15-second sequence and checks real register values");

 for(unsigned mode=0;mode<3;++mode) {
  reset();ud.watchdog_armed=false;
  if(mode==0)ud.wdt=0;if(mode==1)ud.watchdog=false;if(mode==2)atomic_store_host(&picasso_wdt_lock,1);
  picasso_usb_watchdog_arm();assert(!ud.watchdog_armed && writes.empty() && irq_enabled);
 }
 passed("unmapped disabled-option or busy boot arm changes no hardware");

 for(unsigned off:{APSS_WDT_EN,APSS_WDT_BARK,APSS_WDT_BITE}) {
  reset();regs[off/4]^=2;
  assert(!picasso_watchdog_backend_claim(&owner_a) && !picasso_wdt_owner && writes.empty());
 }
 reset();ud.watchdog_armed=false;assert(!picasso_watchdog_backend_claim(&owner_a) && writes.empty());
 reset();ud.wdt=0;assert(!picasso_watchdog_backend_claim(&owner_a) && writes.empty());
 passed("claim rejects unarmed unmapped and every persistent-register mismatch without installing owner");

 reset();
 assert(!picasso_watchdog_backend_claim(nullptr));assert(!picasso_watchdog_backend_enable(nullptr));
 assert(!picasso_watchdog_backend_disable(nullptr));assert(!picasso_watchdog_backend_pet(nullptr));
 assert(writes.empty() && irq_enabled);
 passed("NULL identities cannot acquire or change the watchdog");

 reset();owns();assert(picasso_wdt_owner==&owner_a);
 assert(!picasso_usb_watchdog_pet());picasso_usb_watchdog_arm();picasso_usb_watchdog_disable();
 assert(writes.empty() && ud.watchdog_armed && picasso_wdt_owner==&owner_a);
 assert(picasso_watchdog_backend_pet(&owner_a) && writes.size()==1);
 passed("successful claim permanently retires boot pet arm and disable paths");

 reset();owns();
 assert(!picasso_watchdog_backend_claim(&owner_b));assert(!picasso_watchdog_backend_enable(&owner_b));
 assert(!picasso_watchdog_backend_disable(&owner_b));assert(!picasso_watchdog_backend_pet(&owner_b));
 assert(writes.empty() && picasso_wdt_owner==&owner_a);
 assert(picasso_watchdog_backend_claim(&owner_a) && writes.size()==1);
 passed("wrong identities preserve owner and registers while the same-owner claim pets idempotently");

 reset();owns();assert(picasso_watchdog_backend_disable(&owner_a));
 assert(!ud.watchdog_armed && regs[APSS_WDT_EN/4]==0 && picasso_wdt_owner==&owner_a);
 writes.clear();assert(picasso_watchdog_backend_disable(&owner_a) && writes.empty());
 assert(!picasso_watchdog_backend_claim(&owner_a) && !picasso_watchdog_backend_pet(&owner_a));
 assert(!picasso_usb_watchdog_pet() && writes.empty());
 assert(picasso_watchdog_backend_enable(&owner_a) && writes.size()==5 && ud.watchdog_armed);
 assert(regs[APSS_WDT_BARK/4]==491519 && regs[APSS_WDT_BITE/4]==491519);
 writes.clear();assert(picasso_watchdog_backend_enable(&owner_a) && writes.size()==1 && writes[0].off==APSS_WDT_RST);
 passed("disable retains identity without boot fallback and enable restores the identical hardware interval");

 for(bool claimed:{false,true}) {
  reset();if(claimed)owns();picasso_usb_watchdog_terminal();no_ordinary_writes();
  assert(atomic_load_host(&picasso_wdt_terminal)==1 && irq_enabled);
 }
 passed("terminal state rejects every ordinary API and every boot writer");

 reset();owns();assert(picasso_usb_watchdog_force_bite());
 assert(writes.size()==2 && writes[0].off==APSS_WDT_RST && writes[1].off==APSS_WDT_BITE && writes[1].value==4);
 no_ordinary_writes();assert(picasso_usb_watchdog_force_bite() && writes.size()==2);
 passed("armed forced-bite is pet then four ticks and neither later pets nor repeated reset reload it");

 reset();owns();assert(picasso_watchdog_backend_disable(&owner_a));writes.clear();
 assert(picasso_usb_watchdog_force_bite() && writes.size()==5);
 assert(writes[0].off==APSS_WDT_BARK && writes[1].off==APSS_WDT_BITE && writes[2].off==APSS_WDT_EN);
 assert(writes[3].off==APSS_WDT_RST && writes[4].off==APSS_WDT_BITE && writes[4].value==4);
 no_ordinary_writes();
 passed("forced-bite from disabled hardware preserves the existing vendor bootstrap sequence");

 reset();owns();atomic_store_host(&picasso_wdt_lock,1);
 assert(!picasso_usb_watchdog_force_bite() && writes.empty() && irq_enabled);
 assert(atomic_load_host(&picasso_wdt_terminal)==1);
 atomic_store_host(&picasso_wdt_lock,0);no_ordinary_writes();assert(picasso_usb_watchdog_force_bite());
 reset();ud.wdt=0;assert(!picasso_usb_watchdog_force_bite() && writes.empty());
 passed("busy or unmapped forced reset latches terminal without writing or deadlocking");

 for(bool initial_irq:{false,true}) {
  reset();owns();irq_enabled=initial_irq;atomic_store_host(&picasso_wdt_lock,1);
  assert(!picasso_watchdog_backend_claim(&owner_a));assert(!picasso_watchdog_backend_enable(&owner_a));
  assert(!picasso_watchdog_backend_disable(&owner_a));assert(!picasso_watchdog_backend_pet(&owner_a));
  assert(!picasso_usb_watchdog_pet());picasso_usb_watchdog_arm();picasso_usb_watchdog_disable();
  assert(irq_enabled==initial_irq && writes.empty() && atomic_load_host(&picasso_wdt_lock)==1);
  atomic_store_host(&picasso_wdt_lock,0);assert(picasso_watchdog_backend_pet(&owner_a));
  assert(irq_enabled==initial_irq);
 }
 passed("try-lock busy and successful paths preserve both originally enabled and disabled IRQ state");

 reset();owns();hook=ReenterHook;hook_offset=APSS_WDT_RST;hook_on_write=true;
 assert(picasso_watchdog_backend_pet(&owner_a) && writes.size()==1 && irq_enabled);
 passed("nested IRQ-context API attempts cannot steal the lock or write through an in-flight pet");

 reset();std::atomic<unsigned> ready{0};bool claim_a=false,claim_b=false;
 auto race=[&](const void *owner,bool &result) {
  ++ready;while(ready.load()!=2)std::this_thread::yield();
  result=picasso_watchdog_backend_claim(owner);assert(irq_enabled);
 };
 std::thread a(race,&owner_a,std::ref(claim_a)),b(race,&owner_b,std::ref(claim_b));a.join();b.join();
 assert(claim_a!=claim_b && writes.size()==1);
 assert(picasso_wdt_owner==(claim_a?(const void *)&owner_a:(const void *)&owner_b));
 passed("two real host threads cannot both claim or interleave MMIO ownership");

 reset();owns();corrupt_after_rst=1;
 assert(!picasso_watchdog_backend_pet(&owner_a) && writes.size()==1 && picasso_wdt_owner==&owner_a);
 reset();corrupt_after_rst=1;
 assert(!picasso_watchdog_backend_claim(&owner_a) && writes.size()==1 && picasso_wdt_owner==nullptr);
 passed("post-RST readback failure reports failure and never replaces or invents an owner");

 reset();owns();ignored_offset=APSS_WDT_EN;
 assert(!picasso_watchdog_backend_disable(&owner_a) && ud.watchdog_armed && picasso_wdt_owner==&owner_a);
 reset();owns();assert(picasso_watchdog_backend_disable(&owner_a));ignored_offset=APSS_WDT_EN;
 assert(!picasso_watchdog_backend_enable(&owner_a) && !ud.watchdog_armed && picasso_wdt_owner==&owner_a);
 passed("ignored EN writes cannot report successful disable or enable");

 reset();ud.watchdog_armed=false;regs[APSS_WDT_BITE/4]=99;ignored_offset=APSS_WDT_BITE;
 picasso_usb_watchdog_arm();assert(!ud.watchdog_armed && contains("arm failed"));
 writes.clear();assert(!picasso_watchdog_backend_claim(&owner_a) && writes.empty());
 passed("boot readback failure leaves the service unclaimable and reports actual failure");

 reset();owns();hook=TerminalHook;hook_on_write=false;hook_offset=APSS_WDT_BITE;
 assert(!picasso_watchdog_backend_pet(&owner_a) && writes.empty());no_ordinary_writes();
 reset();hook=TerminalHook;hook_on_write=true;hook_offset=APSS_WDT_RST;
 assert(!picasso_watchdog_backend_claim(&owner_a) && writes.size()==1 && picasso_wdt_owner==nullptr);
 no_ordinary_writes();
 passed("terminal request during validation blocks RST and a request during claim prevents owner commit");

 reset();owns();assert(picasso_watchdog_backend_disable(&owner_a));writes.clear();
 hook=TerminalHook;hook_on_write=true;hook_offset=APSS_WDT_EN;
 assert(picasso_watchdog_backend_enable(&owner_a) && regs[APSS_WDT_EN/4]==1);
 assert(writes.size()==5);no_ordinary_writes();
 passed("an arm already in progress finishes EN=1 before permanent terminal rejection");

 reset();owns();hook=ForceHook;hook_on_write=true;hook_offset=APSS_WDT_RST;
 assert(picasso_watchdog_backend_pet(&owner_a) && writes.size()==1);
 assert(atomic_load_host(&picasso_wdt_terminal)==1);no_ordinary_writes();
 assert(picasso_usb_watchdog_force_bite() && writes.back().value==4);
 no_ordinary_writes();
 passed("forced reset never programs its short count until the earlier pet has released the lock");

 reset();ud_poller_progress progress={};ud_background_poll(&progress);
 assert(writes.size()==1 && progress.have_pet && progress.last_pet_ns==now_ns);
 assert(contains("petted=1"));owns();size_t before=writes.size();uint64_t last=progress.last_pet_ns;
 now_ns+=1234567;ud_background_poll(&progress);
 assert(writes.size()==before && progress.last_pet_ns==last && progress.initial_reports==1);
 passed("USB handoff stops real background RST writes and preserves the last actual pet statistics");

 reset();owns();progress={};ud_background_poll(&progress);
 assert(writes.empty() && !progress.have_pet && contains("normal=1 petted=0"));
 reset();atomic_store_host(&picasso_wdt_lock,1);progress={};ud_background_poll(&progress);
 assert(writes.empty() && !progress.have_pet && contains("normal=1 petted=0"));
 passed("first background pass reports no fictitious pet after ownership transfer or WDT-lock contention");

 for(const char *command:{"panic-test","reset-test","hang-test","reboot-fastboot"}) {
  reset();owns();next_command=command;progress={};ud_background_poll(&progress);
  assert(writes.empty() && !progress.have_pet && atomic_load_host(&picasso_wdt_terminal)==1);
  assert(reboots+hangs+panics==1 && ud.poll_lock==0);
  no_ordinary_writes();
 }
 passed("all real USB terminal commands latch rejection even after serialized polling clears pending flags");

 reset();owns();atomic_store_host(&picasso_wdt_lock,1);picasso_usb_panic_begin();picasso_usb_panic_begin();
 assert(writes.empty() && markers==1 && flushes==1 && pu_panic_active && ud.panic_mode);
 assert(atomic_load_host(&picasso_wdt_terminal)==1);atomic_store_host(&picasso_wdt_lock,0);no_ordinary_writes();
 passed("panic entry latches terminal with a busy watchdog lock and preserves idempotent panic output");

 reset();atomic_store_host(&ud.poll_lock,1);progress={};ud_background_poll(&progress);
 assert(writes.empty() && !progress.have_pet && contains("normal=0 petted=0"));
 reset();for(unsigned i=0;i<20;++i)picasso_usb_console_drive_poll();assert(writes.empty());
 reenter_logs=true;progress={};ud_background_poll(&progress);assert(writes.size()==1 && polls>21);
 passed("poll contention and recursive console output do not masquerade as independent watchdog progress");

 std::puts("ALL BACKEND CHECKS PASSED");
}
