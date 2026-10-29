// Host-only IOKit stand-ins. The entire production BootState source is included
// after replacing includes and the Mach-O initializer label, not its behavior.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using IOReturn = int;
using OSReturn = int;
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnUnsupported=2;
constexpr int kIOReturnNoMemory=3, kOSReturnSuccess=0, kIOServiceSynchronous=1;
constexpr int kIOClientPrivilegeAdministrator=7;
static int scenario, stageLogs, initializations, publications, baseStops;
[[noreturn]] static void panic(const char *message) { throw std::runtime_error(message); }
static void IOLog(const char *message) {
  if (std::string(message).find("stage=255") != std::string::npos) ++stageLogs;
}
#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#endif
#define __atomic_compare_exchange_n host_compare_exchange
static bool host_compare_exchange(uint32_t *p, uint32_t *expected, uint32_t value,
                                        bool, int, int) {
  if (*p != *expected) { *expected = *p; return false; }
  *p=value; return true;
}
struct OSObject {
  int references=1;
  virtual ~OSObject() = default;
  void retain() { ++references; }
  void release() { if (--references == 0) delete this; }
};
struct OSNumber : OSObject {
  uint64_t value;
  bool mutateOnRead=false;
  explicit OSNumber(uint64_t v):value(v) {}
  uint64_t unsigned64BitValue() { uint64_t v=value; if (mutateOnRead) value=999; return v; }
  static OSNumber *withNumber(uint64_t v, unsigned) { return new OSNumber(v); }
};
struct OSString : OSObject {
  std::string value;
  explicit OSString(const char *v):value(v) {}
};
struct OSDictionary : OSObject {
  std::map<std::string,OSObject*> items;
  ~OSDictionary() override { for (auto &entry:items) entry.second->release(); }
  static OSDictionary *withCapacity(unsigned) { return scenario==10 ? nullptr : new OSDictionary; }
  unsigned getCount() { return static_cast<unsigned>(items.size()); }
  OSObject *getObject(const char *name) { auto at=items.find(name); return at==items.end()?nullptr:at->second; }
  bool setObject(const char *name, OSObject *value) {
    value->retain();
    auto at=items.find(name); if (at!=items.end()) at->second->release();
    items[name]=value; return true;
  }
};
#define OSDynamicCast(T,x) dynamic_cast<T*>(x)
struct OSSymbol : OSString {
  using OSString::OSString;
  static const OSSymbol *withCString(const char *v) { return new OSSymbol(v); }
  void release() const { const_cast<OSSymbol*>(this)->OSObject::release(); }
};
struct OSMetaClass {
  const OSMetaClass *parent;
  static const OSMetaClass *registered;
  explicit OSMetaClass(const OSMetaClass *p=nullptr):parent(p) {}
  const OSMetaClass *getSuperClass() const { return parent; }
  static const OSMetaClass *getMetaClassWithName(const OSSymbol *) { return registered; }
  static void *preModLoad(const char *) { return scenario==3 ? nullptr : reinterpret_cast<void*>(1); }
  static OSReturn postModLoad(void *) { return scenario==4 ? 1 : 0; }
};
const OSMetaClass *OSMetaClass::registered=nullptr;
#define OSDeclareDefaultStructors(C) public: static OSMetaClass gMetaClass;
#define OSDefineMetaClassAndStructors(C,B) OSMetaClass C::gMetaClass(&B::gMetaClass);
static const void *current_task() { return reinterpret_cast<void*>(42); }
struct IOUserClient {
  static bool admin;
  static IOReturn clientHasPrivilege(const void *task, int privilege) {
    assert(task==current_task() && privilege==kIOClientPrivilegeAdministrator);
    return admin?0:17;
  }
};
bool IOUserClient::admin=true;
struct IOService : OSObject {
  static OSMetaClass gMetaClass;
  static IOService *platform;
  static IOService *published;
  static bool rejectStore;
  OSDictionary registry;
  virtual bool start(IOService *) { return scenario!=8; }
  virtual IOReturn setProperties(OSObject *) { return kIOReturnUnsupported; }
  bool init() { return scenario!=6; }
  bool attach(IOService *) { return scenario!=7; }
  void stop(IOService *) { ++baseStops; }
  void setName(const char *) {}
  bool store(const char *name, OSObject *object) {
    bool ok=!rejectStore && scenario!=9;
    if (ok) registry.setObject(name,object);
    object->release(); return ok;
  }
  bool setProperty(const char *n,uint64_t v,unsigned) { return store(n,new OSNumber(v)); }
  bool setProperty(const char *n,const char *v) { return store(n,new OSString(v)); }
  bool setProperty(const char *n,bool v) { return store(n,new OSNumber(v)); }
  OSObject *getProperty(const char *n) { return registry.getObject(n); }
  void registerService(int options) {
    assert(options==kIOServiceSynchronous);
    published=this; ++publications;
  }
  static IOService *getPlatform() { return scenario==5?nullptr:platform; }
  static OSDictionary *serviceMatching(const char *name) {
    assert(std::string(name)=="IOService"); return OSDictionary::withCapacity(1);
  }
  static IOService *copyMatchingService(OSDictionary *matching) {
    auto props=OSDynamicCast(OSDictionary,matching->getObject("IOPropertyMatch"));
    auto primary=props?OSDynamicCast(OSNumber,props->getObject("IOPMUPrimary")):nullptr;
    assert(primary && primary->value==1);
    if (scenario==11 && publications) return nullptr;
    if (published) published->retain();
    return published;
  }
};
OSMetaClass IOService::gMetaClass;
IOService *IOService::platform=nullptr, *IOService::published=nullptr;
bool IOService::rejectStore=false;
struct kmod_info_t { const char *name; };
kmod_info_t g_kernel_kmod_info={"kernel"};
#include "boot-state-production.inc"
extern "C" void picasso_boot_state_static_init(void) {
  ++initializations; OSMetaClass::registered=&PicassoBootState::gMetaClass;
}
static uint64_t stored(IOService *s) {
  auto n=OSDynamicCast(OSNumber,s->getProperty("IOPMUBootStage")); assert(n); return n->value;
}
static IOReturn setStage(PicassoBootState *s, uint64_t value) {
  OSDictionary d; auto n=new OSNumber(value); d.setObject("IOPMUBootStage",n); n->release();
  return s->setProperties(&d);
}
int main(int argc,char **argv) {
  assert(argc==2); scenario=std::atoi(argv[1]);
  IOService provider; IOService::platform=&provider;
  if (scenario==1) IOService::published=&provider;
  if (scenario==2) OSMetaClass::registered=&PicassoBootState::gMetaClass;
  bool trapped=false;
  try { picasso_boot_state_run(); }
  catch (const std::runtime_error &) { trapped=true; }
  if (scenario>=2) {
    assert(trapped);
    if (scenario==9) assert(baseStops==1 && publications==0);
    std::printf("PASS: boot-state rejection %d\n",scenario); return 0;
  }
  assert(!trapped);
  picasso_boot_state_run();  // A second call cannot publish/register again.
  if (scenario==1) {
    assert(initializations==0 && publications==0 && IOService::published==&provider);
    std::puts("PASS: existing primary service and run-once"); return 0;
  }
  assert(publications==1 && initializations==1);
  auto service=dynamic_cast<PicassoBootState*>(IOService::published); assert(service);
  assert(stored(service)==0);
  auto primary=OSDynamicCast(OSNumber,service->getProperty("IOPMUPrimary"));
  auto hardware=OSDynamicCast(OSNumber,service->getProperty("HardwarePMUImplemented"));
  auto storage=OSDynamicCast(OSString,service->getProperty("PicassoBootStageStorage"));
  assert(primary && primary->value==1 && hardware && hardware->value==0);
  assert(storage && storage->value=="volatile-software");
  std::puts("PASS: initialized volatile state and synchronous property matching");

  IOUserClient::admin=false;
  assert(setStage(service,255)==17 && stored(service)==0);
  IOUserClient::admin=true;
  OSString text("255");
  assert(service->setProperties(&text)==kIOReturnBadArgument);
  OSDictionary empty;
  assert(service->setProperties(&empty)==kIOReturnUnsupported);
  OSDictionary wrong; wrong.setObject("Other",&text);
  assert(service->setProperties(&wrong)==kIOReturnUnsupported);
  OSDictionary malformed; malformed.setObject("IOPMUBootStage",&text);
  assert(service->setProperties(&malformed)==kIOReturnBadArgument);
  malformed.setObject("Other",&text);
  assert(service->setProperties(&malformed)==kIOReturnUnsupported);
  assert(setStage(service,256)==kIOReturnBadArgument && setStage(service,UINT64_MAX)==kIOReturnBadArgument);
  assert(stored(service)==0);
  std::puts("PASS: privilege, shape, type, unknown operations and bounds");

  assert(setStage(service,42)==0 && stored(service)==42);
  IOService::rejectStore=true;
  assert(setStage(service,255)==kIOReturnNoMemory && stored(service)==42 && stageLogs==0);
  IOService::rejectStore=false;
  OSDictionary changing; auto number=new OSNumber(77); number->mutateOnRead=true;
  changing.setObject("IOPMUBootStage",number);
  assert(service->setProperties(&changing)==0 && number->value==999 && stored(service)==77);
  number->value=3; assert(stored(service)==77); number->release();
  assert(setStage(service,255)==0 && stored(service)==255 && stageLogs==1);
  assert(setStage(service,0)==0 && setStage(service,255)==0 && stageLogs==1);
  std::puts("PASS: failed-store preservation, one snapshot, independent storage and bounded logging");
  service->release(); IOService::published=nullptr;
  return 0;
}
