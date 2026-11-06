// SPDX-License-Identifier: BSD-2-Clause-Patent
// API model only. Full SDK/link validation remains part of Compose.
#include <stdint.h>
#include <stddef.h>
extern "C" {
void *IOMalloc(size_t);
void IOFree(void *,size_t);
void bzero(void *,size_t);
int memcmp(const void *,const void *,size_t);
void IOSleep(unsigned);
void IOLog(const char *,...) __attribute__((format(printf,1,2)));
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(unsigned,unsigned,uint64_t *);
typedef struct {uint8_t bytes[256];} SHA256_CTX;
void SHA256_Init(SHA256_CTX *);
void SHA256_Update(SHA256_CTX *,const void *,size_t);
void SHA256_Final(void *,SHA256_CTX *);
}
using wait_result_t=int;
using kern_return_t=int;
using thread_t=void *;
enum {KERN_SUCCESS=0,kSecondScale=1};
extern "C" {
thread_t current_thread(void);
void thread_set_thread_name(thread_t,const char *);
kern_return_t kernel_thread_start(void (*)(void *,wait_result_t),void *,thread_t *);
void thread_deallocate(thread_t);
}
