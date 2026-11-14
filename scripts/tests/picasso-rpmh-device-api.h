// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit API stand-ins for compiling the production RPMh owner on ARM64.
#include <stdint.h>
#include <stddef.h>
#include "PicassoRpmh.h"
extern "C" int printf(const char *,...) __attribute__((format(printf,1,2)));
class IOMemoryMap { public: void release(); };
struct Mapping { IOMemoryMap *keep; volatile uint8_t *base; uint64_t bytes,physical; };
bool mapPhysical(uint64_t,uint64_t,Mapping &);
uint32_t read32(const Mapping &,uint64_t);
void write32(const Mapping &,uint64_t,uint32_t);
void barrier();
void IOSleep(unsigned);
void IODelay(unsigned);
void *IOMalloc(size_t);
void IOFree(void *,size_t);
