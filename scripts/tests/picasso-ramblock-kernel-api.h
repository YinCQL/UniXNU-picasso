// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile-only API declarations. This does not replace XNU/IOStorage ABI checks.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
using UInt64=uint64_t;using UInt32=uint32_t;using IOOptionBits=uint32_t;
using IOReturn=int;using dev_t=int;using vm_offset_t=uintptr_t;using vm_size_t=uint64_t;
using wait_result_t=int;using thread_t=void *;
constexpr int kIOReturnSuccess=0,kIOReturnUnsupported=1,kIOReturnBadArgument=2,kIOReturnNotReady=3,kIOReturnIOError=4,kIOReturnNoMemory=5,KERN_SUCCESS=0,kIODirectionIn=1,kIODirectionOut=2;
#define THREAD_NULL nullptr
#define NODEV (-1)
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,B)
#define OSDynamicCast(T,p) static_cast<T *>(p)
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();p=nullptr;}while(0)
#define makedev(a,b) (((a)<<24)|(b))
extern const char *gIODTPlane,*kIOBSDNameKey,*kIOBSDMajorKey,*kIOBSDMinorKey;
constexpr uintptr_t VM_MIN_KERNEL_ADDRESS=0xffff000000000000ULL;
class OSObject {public:void retain();void release();};
class OSString:public OSObject {public:static OSString *withCString(const char *);const char *getCStringNoCopy();unsigned getLength();};
class OSData:public OSObject {public:unsigned getLength();const void *getBytesNoCopy();};
class OSNumber:public OSObject {public:uint64_t unsigned64BitValue();uint32_t unsigned32BitValue();};
class OSDictionary:public OSObject {public:static OSDictionary *withCapacity(unsigned);bool setObject(const char *,OSObject *);};
class OSIterator:public OSObject {public:OSObject *getNextObject();};
class IORegistryEntry:public OSObject {public:static IORegistryEntry *fromPath(const char *,const char *);OSObject *getProperty(const char *);};
class IOService:public IORegistryEntry {
public:
 static OSDictionary *serviceMatching(const char *);static OSIterator *getMatchingServices(OSDictionary *);
 static IOService *waitForMatchingService(OSDictionary *,uint64_t);
 IOService *getProvider();void registerService();
 virtual bool start(IOService *);virtual void stop(IOService *);virtual void free();
};
class IOMemoryDescriptor {public:uint64_t getLength();int getDirection();uint64_t readBytes(uint64_t,void *,uint64_t);uint64_t writeBytes(uint64_t,const void *,uint64_t);};
struct IOStorageAttributes {};
struct IOStorageCompletion {void (*action)(void *,void *,IOReturn,uint64_t);void *target,*parameter;};
class IOBlockStorageDevice:public IOService {
public:
 virtual IOReturn doEjectMedia();virtual IOReturn doFormatMedia(UInt64);virtual UInt32 doGetFormatCapacities(UInt64 *,UInt32) const;
 virtual char *getVendorString();virtual char *getProductString();virtual char *getRevisionString();virtual char *getAdditionalDeviceInfoString();
 virtual IOReturn reportBlockSize(UInt64 *);virtual IOReturn reportEjectability(bool *);virtual IOReturn reportMaxValidBlock(UInt64 *);
 virtual IOReturn reportMediaState(bool *,bool *);virtual IOReturn reportRemovability(bool *);virtual IOReturn reportWriteProtection(bool *);
 virtual IOReturn doSynchronizeCache();virtual IOReturn doSynchronize(UInt64,UInt64,IOOptionBits);
 virtual IOReturn getWriteCacheState(bool *);virtual IOReturn setWriteCacheState(bool);
 virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *);
};
struct IOLock;IOLock *IOLockAlloc();void IOLockLock(IOLock *);void IOLockUnlock(IOLock *);void IOLockFree(IOLock *);
void *IOMallocZero(uint64_t);void IOFree(void *,uint64_t);uintptr_t ml_static_ptovirt(uint64_t);
void IOSleep(unsigned);bool PE_parse_boot_argn(const char *,void *,int);
int kernel_thread_start(void (*)(void *,int),void *,thread_t *);void thread_deallocate(thread_t);void thread_terminate(thread_t);thread_t current_thread();
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
int snprintf(char *,size_t,const char *,...);size_t strlcpy(char *,const char *,size_t);
struct SHA256_CTX {char bytes[256];};void SHA256_Init(SHA256_CTX *);void SHA256_Update(SHA256_CTX *,const void *,size_t);void SHA256_Final(uint8_t *,SHA256_CTX *);
