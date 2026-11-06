// SPDX-License-Identifier: BSD-2-Clause-Patent

// Host-owned API stand-ins: this is not an Apple SDK build.
#include <stdint.h>
#include <stddef.h>
using kern_return_t=int;
using IOReturn=int;
enum {kIOReturnBadArgument=5,kIOReturnNotFound=4,kIOReturnUnsupported=3,kIOReturnBusy=6,kIOReturnNotReady=1,kIOReturnIOError=2,kIOReturnSuccess=0};
class OSSymbol;
class OSMetaClass {
public:
 static const OSMetaClass *getMetaClassWithName(const OSSymbol *);
 unsigned getClassSize() const;
 static void *preModLoad(const char *);
 static int postModLoad(void *);
};
class OSObject {public: void retain(); void release();};
class OSIterator:public OSObject {public:OSObject *getNextObject();};
class OSSymbol:public OSObject {public:static const OSSymbol *withCString(const char *);void release() const;};
class OSDictionary:public OSObject {};
extern OSObject *kOSBooleanTrue;
extern const char *gIOServicePlane;
class IORegistryEntry:public OSObject {
public:
 void *metaCast(const char *);
 OSIterator *getChildIterator(const char *);
 OSObject *getProperty(const char *);
 bool setProperty(const char *,OSObject *);
 bool setProperty(const char *,unsigned long long,unsigned);
 IORegistryEntry *getParentEntry(const char *);
};
class IOService:public IORegistryEntry {
public:
 static OSDictionary *serviceMatching(const char *);
 static IOService *waitForMatchingService(OSDictionary *,uint64_t);
 bool init();bool attach(IOService *);bool start(IOService *);void detach(IOService *);void stop(IOService *);
};
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
#define OSDynamicCast(t,p) static_cast<t *>(p)
enum {kOSKextExcludeNone=0};
class OSKext {public:static int loadKextWithIdentifier(const char *,bool,bool,int,int,void *);};
using thread_call_param_t=void *;
using thread_call_t=void *;
extern "C" {
struct kmod_info_t {char name[64];};
uint64_t mach_absolute_time();
void absolutetime_to_nanoseconds(uint64_t,uint64_t *);
void clock_interval_to_deadline(unsigned,unsigned,uint64_t *);
thread_call_t thread_call_allocate(void (*)(thread_call_param_t,thread_call_param_t),void *);
bool thread_call_enter(thread_call_t);
bool thread_call_enter_delayed(thread_call_t,uint64_t);
bool PE_parse_boot_argn(const char *,void *,int);
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
}
enum {kMillisecondScale=1000000};

class IOMemoryMap:public OSObject {public: uintptr_t getVirtualAddress();};
class IODeviceMemory:public OSObject {public:static IODeviceMemory *withRange(uint64_t,uint64_t); IOMemoryMap *map(unsigned);};
struct IOLock;
IOLock *IOLockAlloc(); void IOLockLock(IOLock *); void IOLockUnlock(IOLock *);
void IOLockFree(IOLock *);
using wait_result_t=int; using thread_t=void *;
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,KERN_SUCCESS=0};
void IODelay(unsigned); void IOSleep(unsigned);
thread_t current_thread(); void thread_set_thread_name(thread_t,const char *);
int kernel_thread_start(void (*)(void *,wait_result_t),void *,thread_t *);
void thread_deallocate(thread_t);

// IOLocks API shapes for compiling the shared controller implementation.
struct IOSimpleLock;
using IOInterruptState=int;
IOSimpleLock *IOSimpleLockAlloc();
void IOSimpleLockFree(IOSimpleLock *);
IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *);
void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *,IOInterruptState);
