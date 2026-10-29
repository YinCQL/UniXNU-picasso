// IOKit/policy/backend stand-ins for the unchanged production lifecycle methods.
// No MMIO, original Apple code, real timer thread or platform ABI is simulated.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
using IOReturn=int;
using UInt32=uint32_t;
using task_t=void*;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=2, kIOTimerEventSourceOptionsPriorityKernelHigh=1;
#define LIBKERN_RETURNS_RETAINED
#define OSDeclareDefaultStructors(C)
#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#endif
#define __atomic_fetch_add host_fetch_add
static uint32_t host_fetch_add(uint32_t *p,uint32_t amount,int) { auto old=*p; *p+=amount; return old; }
static std::vector<std::string> events;
static int failure, schedules, pets, checks, frees, timerReleases, loopReleases, ucResult, clientLogs;
static void *claimed;
static bool policyTraps;
[[noreturn]] static void panic(const char *message) { throw std::runtime_error(message); }
static void IOLog(const char *format,...) {
  if (std::string(format).find("newUserClient")!=std::string::npos) ++clientLogs;
}
struct OSObject { virtual ~OSObject() = default; };
struct OSDictionary : OSObject {};
struct IOUserClient : OSObject {};
struct IOWorkLoop;
struct IOService : OSObject {
  virtual bool start(IOService *) { return true; }
  virtual void free() { ++frees; }
  virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
  virtual IOReturn newUserClient(task_t,void*,UInt32,OSDictionary*,IOUserClient**) { return ucResult; }
  OSObject *getProperty(const char *) { return nullptr; }
};
struct IOWatchdog : IOService {
protected:
  virtual void enableWatchdogHardware()=0;
  virtual void disableWatchdogHardware()=0;
  virtual void petWatchdogHardware()=0;
public:
  void enableForTest() { enableWatchdogHardware(); }
  void stop(IOService *) { disableWatchdogHardware(); }
  IOReturn newUserClient(task_t,void*,UInt32,OSDictionary*,IOUserClient** output) override {
    events.push_back("original-client");
    if (!ucResult && output) *output=reinterpret_cast<IOUserClient*>(uintptr_t(42));
    return ucResult;
  }
};
struct IOTimerEventSource : OSObject {
  using Action=void(*)(OSObject*,IOTimerEventSource*);
  static IOTimerEventSource *last;
  Action action; OSObject *owner; bool enabled=true;
  IOTimerEventSource(OSObject *o,Action a):action(a),owner(o) {}
  static IOTimerEventSource *timerEventSource(int options,OSObject *o,Action a) {
    assert(options==kIOTimerEventSourceOptionsPriorityKernelHigh);
    if (failure==2) return nullptr;
    return last=new IOTimerEventSource(o,a);
  }
  IOReturn setTimeoutMS(unsigned ms) { assert(ms==1000); ++schedules; events.push_back("schedule"); return failure==7?1:0; }
  void enable() { enabled=true; }
  void disable() { enabled=false; }
  void cancelTimeout() { events.push_back("cancel"); }
  void release() { ++timerReleases; delete this; }
};
IOTimerEventSource *IOTimerEventSource::last=nullptr;
struct IOWorkLoop : OSObject {
  static IOWorkLoop *last;
  int gate=0;
  static IOWorkLoop *workLoop() { if(failure==1)return nullptr; return last=new IOWorkLoop; }
  IOReturn addEventSource(IOTimerEventSource*) { events.push_back("add-timer"); return failure==3?1:0; }
  void removeEventSource(IOTimerEventSource*) { events.push_back("remove-timer"); }
  void release() { ++loopReleases; delete this; }
  // Declarations AND access specifiers extracted from the fixed XNU header.
  #include "workloop-access.inc"
};
IOWorkLoop *IOWorkLoop::last=nullptr;
void IOWorkLoop::closeGate() { ++gate; }
void IOWorkLoop::openGate() { assert(gate>0); --gate; }
IOReturn IOWorkLoop::runAction(Action action, OSObject *target,
                              void *arg0, void *arg1, void *arg2, void *arg3) {
  closeGate();
  auto result=action(target,arg0,arg1,arg2,arg3);
  openGate();
  return result;
}
#ifdef TEST_ILLEGAL_WORKLOOP_ACCESS
void illegalWorkloopAccess(IOWorkLoop *loop) {
  loop->closeGate();
  loop->openGate();
}
#endif
static int PE_i_can_has_debugger(void *) { return 1; }
extern "C" bool picasso_watchdog_backend_claim(const void *owner) {
  assert(IOWorkLoop::last->gate>0 && IOTimerEventSource::last && schedules==0);
  events.push_back("claim"); if(failure==4)return false; claimed=const_cast<void*>(owner); return true;
}
extern "C" bool picasso_watchdog_backend_enable(const void *owner) {
  assert(owner==claimed && IOWorkLoop::last->gate>0);
  events.push_back("enable"); return failure!=6;
}
extern "C" bool picasso_watchdog_backend_disable(const void *owner) {
  assert(owner==claimed && IOWorkLoop::last->gate>0);
  events.push_back("disable"); return failure!=9;
}
extern "C" bool picasso_watchdog_backend_pet(const void *owner) {
  assert(owner==claimed && IOWorkLoop::last->gate>0);
  events.push_back("pet"); if(failure==8)return false; ++pets; return true;
}
extern "C" bool picasso_original_iowatchdog_policy_start(IOWatchdog *self,IOService*,uintptr_t zero,uintptr_t flags) {
  assert(claimed==self && zero==0 && flags==3 && schedules==0);
  events.push_back("policy-start");
  if(failure==5)return false;
  self->enableForTest(); return true;
}
extern "C" void picasso_original_iowatchdog_policy_check(IOWatchdog*) {
  assert(IOWorkLoop::last->gate>0); events.push_back("policy-check"); ++checks;
  if(policyTraps)panic("original policy timeout");
}
#include "watchdog-lifecycle-production.inc"
static void fire(IOTimerEventSource *timer) {
  IOWorkLoop::last->runAction([](OSObject *, void *arg0, void *, void *, void *) -> IOReturn {
    auto fired=static_cast<IOTimerEventSource*>(arg0);
    fired->action(fired->owner,fired);
    return kIOReturnSuccess;
  }, timer->owner, timer);
}
int main(int argc,char **argv) {
  assert(argc==2); failure=std::atoi(argv[1]);
  PicassoWatchdogDriver service; IOService provider;
  bool trapped=false, started=false;
  try { started=service.start(&provider); }
  catch(const std::runtime_error&) { trapped=true; }
  if(failure>=1 && failure<=4) {
    assert(!trapped && !started && !claimed && schedules==0);
    service.free();
    assert(frees==1 && loopReleases==(failure==1?0:1) && timerReleases==(failure<3?0:1));
    std::printf("PASS: pre-claim failure %d releases resources without policy or pet\n",failure); return 0;
  }
  if(failure>=5 && failure<=7) {
    assert(trapped && claimed==&service && pets==0);
    trapped=false; try { service.free(); } catch(const std::runtime_error&) { trapped=true; }
    assert(trapped && frees==0);
    std::printf("PASS: post-claim failure %d panics and retains permanent owner\n",failure); return 0;
  }
  assert(started && !trapped && schedules==1 && pets==0);
  assert(events==std::vector<std::string>({"add-timer","claim","policy-start","enable","schedule"}));
  assert(service.getWorkLoop()==IOWorkLoop::last && IOWorkLoop::last->gate==0);
  auto timer=IOTimerEventSource::last;
  if(failure==8) {
    try { fire(timer); } catch(const std::runtime_error&) { trapped=true; }
    assert(trapped && checks==1 && pets==0 && schedules==1);
    std::puts("PASS: rejected hardware pet cannot reschedule a successful check"); return 0;
  }
  if(failure==9) {
    try { service.stop(&provider); } catch(const std::runtime_error&) { trapped=true; }
    assert(trapped && !timer->enabled && pets==0);
    std::puts("PASS: failed hardware disable is terminal"); return 0;
  }
  fire(timer); assert(checks==1 && pets==1 && schedules==2);
  assert(events[events.size()-3]=="policy-check" && events[events.size()-2]=="pet" && events.back()=="schedule");
  std::puts("PASS: resources before permanent claim, original policy before hardware pet");
  IOTimerEventSource wrong(&service,timer->action);
  fire(&wrong); assert(checks==1 && pets==1 && schedules==2);
  service.stop(&provider); assert(!timer->enabled);
  fire(timer); assert(checks==1 && pets==1 && schedules==2);
  service.enableForTest(); assert(timer->enabled && schedules==3);
  std::puts("PASS: wrong sender ignored, stop cancels checks, reenable rearms timer");
  for(int i=0;i<12;++i) {
    IOUserClient *client=nullptr;
    ucResult=i%2?77:0;
    assert(service.newUserClient(nullptr,nullptr,1,nullptr,&client)==ucResult);
    assert(client==(ucResult?nullptr:reinterpret_cast<IOUserClient*>(uintptr_t(42))));
  }
  assert(clientLogs==8);
  std::puts("PASS: original client result/handler preserved, eight diagnostic records maximum");
  policyTraps=true;
  try { fire(timer); } catch(const std::runtime_error&) { trapped=true; }
  assert(trapped && checks==2 && pets==1 && schedules==3);
  std::puts("PASS: original policy panic cannot pet hardware or schedule another check");
  return 0;
}
