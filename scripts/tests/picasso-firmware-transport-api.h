// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile-only ARM64 API model for the production UFS firmware headers.
// It does not replace full NativeUfs SDK compilation/linking.
#include <stdint.h>
#include <stddef.h>
using IOReturn=unsigned;
constexpr unsigned kIOReturnSuccess=0,kUcdOffset=4096,kRspOffset=512,kDataOffset=8192;
struct FirmwareHostApi {void *lock;bool ready,poisoned;uint8_t *virt;};
extern FirmwareHostApi gHost;
struct FirmwareDiagnosticApi {unsigned sense;};
extern FirmwareDiagnosticApi gRootDiagnostic;
extern "C" {
void IOLockLock(void *);
void IOLockUnlock(void *);
void IOSleep(unsigned);
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
void *memcpy(void *,const void *,size_t);
unsigned native_ufs_store_ready(void);
}
bool hostClearUcd();
IOReturn hostSubmit(unsigned,unsigned,unsigned,unsigned *);
IOReturn hostResponseHeader(uint8_t,uint8_t,uint8_t);
IOReturn hostProtocolError(unsigned);
IOReturn hostScsi(uint8_t,const uint8_t *,unsigned,bool,unsigned,unsigned,unsigned *);
IOReturn hostRead4k(uint8_t,uint64_t,unsigned,void *);
uint32_t loadBe32(const uint8_t *);
uint32_t le32(const uint8_t *);
uint64_t le64(const uint8_t *);
uint32_t rootCrc32(const uint8_t *,size_t,bool=false);
bool rootEntryUsed(const uint8_t *);
