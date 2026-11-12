/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include "surface_hash.h"
__attribute__((noinline)) uint64_t legacy(const void *data,size_t n){const uint8_t *p=data;uint64_t sum=n,w;for(size_t at=0;at+8<=n;at+=8){memcpy(&w,p+at,8);sum=(sum^w)*UINT64_C(0x9e3779b97f4a7c15);sum^=sum>>29;}return sum;}
__attribute__((noinline)) uint64_t faster(const void *p,size_t n){return nativegl_surface_hash(p,n);}
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){size_t page=sysconf(_SC_PAGESIZE);uint8_t *map=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(map!=MAP_FAILED);assert(!mprotect(map+page,page,PROT_READ|PROT_WRITE));
for(size_t n=0;n<=513;n++){uint8_t *p=map+page*2-n;for(size_t i=0;i<n;i++)p[i]=(i*17+n)^0xa5;uint64_t h=faster(p,n);assert(h==faster(p,n));for(size_t i=0;i<n;i++){p[i]^=1;assert(h!=faster(p,n));p[i]^=1;}}
assert(!munmap(map,page*3));puts("PASS: empty, unaligned and guard-page buffers; every byte and tail mutation detected");
size_t sizes[]={652800,7062736,10368000};volatile uint64_t sink=0;
for(unsigned k=0;k<3;k++){size_t n=sizes[k];uint8_t *p=malloc(n);assert(p);for(size_t i=0;i<n;i++)p[i]=i*37+11;uint64_t old=0,fast=0;
for(unsigned round=0;round<5;round++){uint64_t a=ns();for(unsigned j=0;j<25;j++){p[(j*4093)%n]^=1;sink^=legacy(p,n);}old+=ns()-a;a=ns();for(unsigned j=0;j<25;j++){p[(j*4093)%n]^=1;sink^=faster(p,n);}fast+=ns()-a;}
printf("BENCH bytes=%zu legacy_ns=%llu parallel_ns=%llu speedup=%.3f\n",n,(unsigned long long)old,(unsigned long long)fast,(double)old/fast);free(p);}
(void)sink;}
