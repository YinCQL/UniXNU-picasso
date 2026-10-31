// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production IOKit method body; descriptor, crypto and transport stand-ins.
// The separate Python bridge uses real SHA256 to test the actual verifier.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
using UInt64 = unsigned long long;
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnIOError=2,
  kIOReturnNotPrivileged=3, kIOReturnNotWritable=4, kIOReturnNotReady=5;
constexpr uint64_t kDataMax=131072;
constexpr unsigned kIODirectionIn=1, VR_BLOCK=9;
#include "RootReadDiagnostics.h"
#include "RootWindowRead.h"
#include "RootDiagnostics.h"
#define NATIVE_VERIFIED_BLOCK 1
static bool held;
struct IOLock {};
static void IOLockLock (IOLock *) { assert (!held); held=true; }
static void IOLockUnlock (IOLock *) { assert (held); held=false; }
static char logs[16384];
static void IOLog (const char *format, ...) {
  assert (!held); // All new logging must happen after the production guard.
  char line[512]; va_list args; va_start (args,format);
  int n=vsnprintf (line,sizeof(line),format,args); va_end(args);
  assert(n>0 && n<256 && line[n-1]=='\n');
  size_t used=strlen(logs);
  assert(used+size_t(n)<sizeof(logs)); memcpy(logs+used,line,size_t(n)+1);
}
static unsigned faults, reads, completions, integrity;
static bool transportFail, verifyFail, shortCopy;
static UfsRootDiagnostics gRootDiagnostic;
static uint64_t native_ufs_read_ticks, native_ufs_verify_ticks, native_ufs_verified_blocks,
  native_ufs_block_reads, native_ufs_block_failures;
static uint32_t native_ufs_integrity_failures;
static uint64_t timerTicks () { static uint64_t tick; return ++tick; }
static void rootDiagnosticFailure (const char *) { assert(!held); ++faults; }
static void progress () {}
static void (*boot_progress_poll)()=progress;
struct vr_view {};
static bool verifyEmbeddedRecord (const vr_view *,uint32_t,uint64_t index,const void *,size_t length,
                                  UfsRecordFailure *failure) {
  assert(held); ++integrity;
  return verifyFail ? ufsRejectRecord(failure,"hash_mismatch",index,length,length) : true;
}
static IOReturn hostRead4k (unsigned,uint64_t,uint32_t blocks,void *data) {
  assert(held); ++reads; memset(data,0x5a,blocks*4096); return transportFail?kIOReturnIOError:0;
}
struct IOMemoryDescriptor {
  uint8_t output[4096]={}; unsigned direction=kIODirectionIn;
  uint64_t getLength () {return sizeof(output);}
  unsigned getDirection () {return direction;}
  uint64_t writeBytes (uint64_t offset,const void *data,uint64_t length) {
    assert(held && offset+length<=sizeof(output));
    if(shortCopy)length=17;
    memcpy(output+offset,data,size_t(length)); return length;
  }
};
struct IOStorageAttributes {};
struct IOStorageCompletion { void *target,*parameter; void (*action)(void *,void *,IOReturn,uint64_t); };
struct NativeUfsBlock {
  uint64_t absolute=0x410000ULL*4096,bytes=kDataMax;
  char label[32]="SystemOS";
  uint8_t bounce[kDataMax]={}; IOLock mutex,*windowLock=&mutex;
  bool online=true; uint32_t readFailureReports=0;
  vr_view verified; uint32_t verifiedRecordBytes=kDataMax; UfsVerifiedRecordCache verifiedCache;
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *);
};
#include "actual-read.inc"
static IOReturn completedStatus; static uint64_t completedBytes;
static void complete (void *,void *,IOReturn status,uint64_t bytes) {
  assert(!held); ++completions; completedStatus=status; completedBytes=bytes;
}
int main () {
  NativeUfsBlock device;
  IOMemoryDescriptor buffer; IOStorageCompletion callback{nullptr,nullptr,complete};
  verifyFail=true;
  for(unsigned i=0;i<6;++i) {
    assert(device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback)==0);
    assert(completedStatus==kIOReturnNotPrivileged && !completedBytes && !device.verifiedCache.valid);
  }
  assert(completions==6 && faults==6 && reads==6 && integrity==6 && device.readFailureReports==4);
  assert(strstr(logs,"window=SystemOS phase=verification block=0 count=8 done=0 result=00000003 lba=410000 blocks=32"));
  assert(strstr(logs,"reason=hash_mismatch"));
  for(uint8_t byte:buffer.output)assert(byte==0);
  puts("PASS failed authentication refuses bytes and completes outside the lock; reports saturate");
  verifyFail=false;
  assert(!device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback));
  assert(!completedStatus && completedBytes==4096 && device.verifiedCache.valid);
  unsigned before=reads;
  assert(!device.doAsyncReadWrite(&buffer,8,8,nullptr,&callback));
  assert(reads==before && !completedStatus && completedBytes==4096);
  puts("PASS valid reads and cache hits preserve original completion behavior");
  device.verifiedCache={};device.readFailureReports=0;logs[0]=0;transportFail=true;
  assert(!device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback));
  assert(completedStatus==kIOReturnIOError && !completedBytes && strstr(logs,"phase=transport"));
  assert(!strstr(logs,"read_failed") && !device.verifiedCache.valid);
  transportFail=false;shortCopy=true;logs[0]=0;
  assert(!device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback));
  assert(completedStatus==kIOReturnIOError && completedBytes==17 && strstr(logs,"phase=copy"));
  assert(!strstr(logs,"read_failed"));
  puts("PASS transport and short-copy failures remain distinct from authentication");
  before=completions;
  assert(device.doAsyncReadWrite(nullptr,0,8,nullptr,&callback)==kIOReturnBadArgument);
  buffer.direction=0;
  assert(device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback)==kIOReturnNotWritable);
  buffer.direction=kIODirectionIn;device.online=false;
  assert(device.doAsyncReadWrite(&buffer,0,8,nullptr,&callback)==kIOReturnNotReady);
  assert(completions==before);
  puts("PASS synchronous request refusals do not call completion");
}
