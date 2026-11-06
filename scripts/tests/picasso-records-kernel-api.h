// Compile-only IOKit/UFS API stand-ins; no Apple SDK qualification.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
using IOReturn=int;
using wait_result_t=int;
using thread_t=void *;
struct IOLock;
constexpr int kIOReturnSuccess=0,KERN_SUCCESS=0;
#define THREAD_NULL nullptr
struct Geometry {bool valid;uint64_t first,last,imageBytes;uint32_t headerCrc,entriesCrc;};
extern Geometry gStoreGeometry;
struct Host {IOLock *lock;};
extern Host gHost;
extern uint64_t gAuxRegionFirst,gAuxRegionBlocks,gStoreRegionFirst,gStoreRegionBlocks;
constexpr uint64_t kLogReserveBlocks=8192,kExportBlocks=65536;
IOReturn hostRead4k(unsigned,uint64_t,uint32_t,void *);
IOReturn hostWrite4k(unsigned,uint64_t,uint32_t,const void *,bool);
IOReturn hostSyncCache();
bool hostLogOpen(uint64_t,uint64_t);bool hostAuxOpen(uint64_t,uint64_t);
unsigned storeMode();unsigned logMode();uint64_t reserveBlocks();
extern "C" unsigned native_ufs_store_gpt_check();
void *IOMalloc(size_t);void *IOMallocZero(size_t);void IOFree(void *,size_t);
IOLock *IOLockAlloc();void IOLockLock(IOLock *);void IOLockUnlock(IOLock *);
int kernel_thread_start(void (*)(void *,int),void *,thread_t *);
void thread_deallocate(thread_t);void thread_terminate(thread_t);thread_t current_thread();
void IOSleep(unsigned);
bool PE_parse_boot_argn(const char *,void *,int);
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
