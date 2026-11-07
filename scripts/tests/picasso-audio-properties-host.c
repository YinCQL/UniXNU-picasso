// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "PicassoAudioAbi.h"
#include "sdk-constants.h"
typedef uint32_t UInt32,AudioObjectID,AudioClassID,AudioObjectPropertySelector,AudioObjectPropertyScope,AudioObjectPropertyElement;
typedef uint64_t UInt64;typedef float Float32;typedef double Float64;typedef int32_t OSStatus;
typedef bool Boolean;typedef unsigned ULONG;typedef void *AudioServerPlugInDriverRef;
typedef const char *CFStringRef;typedef void *CFDictionaryRef;
#define CFSTR(x) (x)
#define kCFCompareEqualTo 0
static int CFStringCompare(CFStringRef a,CFStringRef b,int flags){(void)flags;return strcmp(a,b);}
typedef struct {UInt32 mSelector,mScope,mElement;} AudioObjectPropertyAddress;
typedef struct {Float64 mMinimum,mMaximum;} AudioValueRange;
typedef struct {Float64 mSampleRate;UInt32 mFormatID,mFormatFlags,mBytesPerPacket,mFramesPerPacket,mBytesPerFrame,mChannelsPerFrame,mBitsPerChannel,mReserved;} AudioStreamBasicDescription;
typedef struct {AudioStreamBasicDescription mFormat;AudioValueRange mSampleRateRange;} AudioStreamRangedDescription;
typedef struct {UInt32 mChannelLabel,mChannelFlags;float mCoordinates[3];} AudioChannelDescription;
typedef struct {UInt32 mChannelLayoutTag,mChannelBitmap,mNumberChannelDescriptions;AudioChannelDescription mChannelDescriptions[1];} AudioChannelLayout;
typedef struct {UInt32 mClientID;} AudioServerPlugInClientInfo;
typedef struct {struct {double mSampleTime;} mOutputTime;} AudioServerPlugInIOCycleInfo;
struct Host;typedef const struct Host *AudioServerPlugInHostRef;
struct Host {OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress *);};
typedef unsigned io_connect_t,io_service_t;typedef int kern_return_t;
typedef struct {unsigned numer,denom;} mach_timebase_info_data_t;
static uint64_t mach_absolute_time(void){return 1000;}
static int mach_timebase_info(mach_timebase_info_data_t *t){t->numer=t->denom=1;return 0;}
#define MACH_PORT_NULL 0
static unsigned mach_task_self(void){return 1;}
static void *IOServiceMatching(const char *n){return (void *)n;}
static io_service_t IOServiceGetMatchingService(unsigned p,void *m){(void)p;(void)m;return 1;}
static int IOServiceOpen(unsigned s,unsigned t,unsigned k,unsigned *c){(void)s;(void)t;(void)k;*c=1;return 0;}
static void IOObjectRelease(unsigned o){(void)o;}
static int IOServiceClose(unsigned o){(void)o;return 0;}
static unsigned notes,changed;
static int IOConnectCallStructMethod(unsigned c,unsigned s,const void *in,size_t n,void *out,size_t *z){
    (void)c;(void)out;(void)z;if(s==PicassoAudioNote){assert(in&&n==sizeof(PicassoAudioClientNote));++notes;}return 0;
}
#define MH_MAGIC_64 0xfeedfacfU
#define CPU_TYPE_ARM64 0x100000c
#define CPU_SUBTYPE_ARM64_ALL 0
#define MH_DYLIB 6
#define LC_SEGMENT_64 0x19
#define LC_UUID 0x1b
#define SECTION_TYPE 255
#define S_NON_LAZY_SYMBOL_POINTERS 6
struct mach_header {uint32_t magic;};
struct mach_header_64 {uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct load_command {uint32_t cmd,cmdsize;};
struct uuid_command {uint32_t cmd,cmdsize;uint8_t uuid[16];};
struct segment_command_64 {uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;int32_t maxprot,initprot;uint32_t nsects,flags;};
struct section_64 {char sectname[16],segname[16];uint64_t addr,size;uint32_t offset,align,reloff,nreloc,flags,reserved1,reserved2,reserved3;};
typedef uintptr_t vm_address_t;typedef size_t vm_size_t;typedef unsigned mach_msg_type_number_t,mach_port_t;typedef int *vm_region_info_t;
typedef struct {int protection;} vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 1
#define VM_REGION_BASIC_INFO_64 9
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_COPY 16
typedef struct {const char *dli_fname;} Dl_info;
#define RTLD_DEFAULT NULL
static uint64_t fakeProduct(void){return UINT64_C(0x669eebc5);}
static int protectError,protectCalls,regionError,resolvedWrong;static void *resolves[5];
static int dladdr(const void *p,Dl_info *i){(void)p;i->dli_fname="/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio";return 1;}
static void *dlsym(void *h,const char *s){(void)h;if(!strcmp(s,"MGGetProductType"))return (void *)fakeProduct;const char *n[]={"AudioObjectGetPropertyData","AudioObjectGetPropertyDataSize","AudioObjectHasProperty","AudioObjectSetPropertyData","__cxa_throw"};for(int i=0;i<5;i++)if(!strcmp(s,n[i]))return resolvedWrong?NULL:resolves[i];return NULL;}
static int vm_region_64(unsigned t,vm_address_t *a,vm_size_t *s,int f,vm_region_info_t i,unsigned *n,unsigned *o){(void)t;(void)a;(void)f;(void)n;*s=4096;*i=VM_PROT_READ;*o=0;return regionError;}
static int vm_protect(unsigned t,vm_address_t a,vm_size_t n,bool b,int p){(void)t;(void)a;(void)n;(void)b;(void)p;++protectCalls;return protectError;}
static void mach_port_deallocate(unsigned t,unsigned o){(void)t;(void)o;}
static void _dyld_register_func_for_add_image(void (*f)(const struct mach_header *,intptr_t)){(void)f;}
#include "plugin-body.inc"
static OSStatus change(AudioServerPlugInHostRef h,AudioObjectID o,UInt32 n,const AudioObjectPropertyAddress *a){(void)h;assert(o&&n&&a);++changed;return 0;}
static int routeCalls;
static OSStatus fakeGet(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *n,void *v){(void)o;(void)a;assert(qs==12&&q==(void *)42&&*n==4);*(UInt32 *)v=17;*n=4;++routeCalls;return -100;}
static OSStatus fakeSize(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *n){(void)o;(void)a;(void)qs;(void)q;*n=12;++routeCalls;return -101;}
static Boolean fakeHas(AudioObjectID o,const AudioObjectPropertyAddress *a){(void)o;(void)a;++routeCalls;return false;}
static OSStatus fakeSet(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,const void *v){(void)o;(void)a;(void)qs;(void)q;assert(n==4&&*(const UInt32 *)v==17);++routeCalls;return -102;}
static void fakeThrow(void *a,void *b,void (*c)(void *)){(void)a;(void)b;(void)c;++routeCalls;}
static void properties(void){
    static const struct Host host={change};gHost=&host;gConnect=1;
    AudioObjectPropertyAddress a={kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput,0};UInt32 n=0,v=99;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&n,&v)&&n==0);
    a.mScope=kAudioObjectPropertyScopeOutput;assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&n,&v)&&n==4&&v==kOutStream);
    // The old 64-bit hash collided for class and base class on the same object.
    gPropertyNotes=0;a=(AudioObjectPropertyAddress){kAudioObjectPropertyClass,kAudioObjectPropertyScopeGlobal,0};
    assert(HasProperty(NULL,kDevice,0,&a));assert(!GetPropertyDataSize(NULL,kDevice,0,&a,0,NULL,&n));
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&v));assert(gPropertyNotes==3);
    a.mScope=kAudioObjectPropertyScopeInput;assert(HasProperty(NULL,kDevice,0,&a));assert(gPropertyNotes==4);
    assert(HasProperty(NULL,kDevice,0,&a)&&gPropertyNotes==4);
    a=(AudioObjectPropertyAddress){kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal,0};Boolean set=false;
    assert(!IsPropertySettable(NULL,kOutStream,0,&a,&set)&&set);
    float pcm[2]={0.5f,-0.5f};assert(!picasso_mix_add(&gMix,0,10,1,pcm,1));v=0;
    assert(!SetPropertyData(NULL,kOutStream,0,&a,0,NULL,4,&v)&&!gStreamActive&&changed==1);
    int16_t output[2];assert(picasso_mix_peek(&gMix,10,1,output)==1&&output[0]==0);
    gIoUsers=1;AudioServerPlugInIOCycleInfo info={{10}};
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    assert(picasso_mix_peek(&gMix,10,1,output)==1);
    v=1;assert(!SetPropertyData(NULL,kOutStream,0,&a,0,NULL,4,&v)&&gStreamActive&&changed==2);
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    assert(!picasso_mix_peek(&gMix,10,1,output)&&output[0]>0&&output[1]<0);
    assert(SetPropertyData(NULL,kOutStream,0,&a,0,NULL,1,&v)==(OSStatus)kAudioHardwareBadPropertySizeError);
    assert(!GetPropertyData(NULL,kOutStream,0,&a,0,NULL,4,&n,&v)&&v==1);
    a.mSelector=kAudioLevelControlPropertyScalarValue;float volume=0.25f;
    assert(!SetPropertyData(NULL,kVolume,0,&a,0,NULL,4,&volume)&&gVolume==0.25f);
    volume=NAN;assert(SetPropertyData(NULL,kVolume,0,&a,0,NULL,4,&volume)==(OSStatus)kAudioHardwareIllegalOperationError);
    // Device aliases and control objects must share one real output state.
    a=(AudioObjectPropertyAddress){kAudioObjectPropertyControlList,kAudioObjectPropertyScopeGlobal,0};UInt32 controls[3]={0};
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof controls,&n,controls)&&n==8&&controls[0]==kVolume&&controls[1]==kMute);
    a=(AudioObjectPropertyAddress){kDeviceVolumeScalar,kAudioObjectPropertyScopeOutput,0};
    assert(!IsPropertySettable(NULL,kDevice,0,&a,&set)&&set);volume=0.75f;
    assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&volume)&&gVolume==0.75f);
    float got=0;assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&got)&&got==volume);
    a.mSelector=kDeviceVolumeDecibels;volume=-9.0f;
    assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&volume)&&fabsf(toDb(gVolume)+9)<0.001f);
    a.mSelector=kDeviceMute;v=1;
    assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&v)&&gMuted&&gain()==0);
    assert(picasso_mix_peek(&gMix,10,1,output)==1&&output[0]==0);
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    assert(!picasso_mix_peek(&gMix,10,1,output)&&output[0]==0&&output[1]==0);
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&v)&&v==1);
    assert(SetPropertyData(NULL,kDevice,0,&a,0,NULL,1,&v)==(OSStatus)kAudioHardwareBadPropertySizeError);
    a.mScope=kAudioObjectPropertyScopeInput;assert(!HasProperty(NULL,kDevice,0,&a));
    assert(SetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&v)==(OSStatus)kAudioHardwareUnknownPropertyError);
    a.mScope=kAudioObjectPropertyScopeOutput;a.mElement=1;assert(!HasProperty(NULL,kDevice,0,&a));
    a=(AudioObjectPropertyAddress){kAudioBooleanControlPropertyValue,kAudioObjectPropertyScopeGlobal,0};v=0;
    assert(!SetPropertyData(NULL,kMute,0,&a,0,NULL,4,&v)&&!gMuted&&gain()>0);
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    assert(!picasso_mix_peek(&gMix,10,1,output)&&output[0]>0&&output[1]<0);
    for(unsigned i=0;i<200;++i){a.mSelector=0xf0000000+i;(void)HasProperty(NULL,kDevice,0,&a);}assert(gPropertyNotes==128);
}
static void routing(void){
    unsigned char *image=aligned_alloc(16384,0x610000);assert(image);memset(image,0,0x610000);struct mach_header_64 *h=(void *)image;
    *h=(struct mach_header_64){MH_MAGIC_64,CPU_TYPE_ARM64,CPU_SUBTYPE_ARM64_ALL,MH_DYLIB,36,4040,0,0};
    struct uuid_command *u=(void *)(h+1);*u=(struct uuid_command){LC_UUID,24,{0x31,0x96,0x33,0x3f,0x5a,0x48,0x37,0x3b,0xb4,0x27,0x01,0x46,0xdc,0x11,0x77,0xfb}};
    struct segment_command_64 *s=(void *)(u+1);s->cmd=LC_SEGMENT_64;s->cmdsize=sizeof *s+sizeof(struct section_64);strcpy(s->segname,"__DATA_CONST");s->vmaddr=0x60c000;s->vmsize=0x28000;s->nsects=1;
    struct section_64 *sec=(void *)(s+1);strcpy(sec->sectname,"__got");sec->addr=0x60c000;sec->size=0x1300;sec->flags=S_NON_LAZY_SYMBOL_POINTERS;
    unsigned char *at=(void *)(sec+1);for(unsigned i=0;i<34;++i){struct load_command *lc=(void *)at;lc->cmd=0;lc->cmdsize=i==33?4040-(unsigned)(at-(unsigned char *)(h+1)):8;at+=lc->cmdsize;}
    assert(routeImageMatches(h));u->uuid[0]^=1;assert(!routeImageMatches(h));u->uuid[0]^=1;
    s->nsects=500;assert(!routeImageMatches(h));s->nsects=1;
    sec->size=0x100;assert(!routeImageMatches(h));sec->size=0x1300;
    h->cpusubtype=2;assert(!routeImageMatches(h));h->cpusubtype=0;
    resolves[0]=(void *)fakeGet;resolves[1]=(void *)fakeSize;resolves[2]=(void *)fakeHas;resolves[3]=(void *)fakeSet;resolves[4]=(void *)fakeThrow;
    uintptr_t offsets[]={0x60c100,0x60c108,0x60c110,0x60c138,0x60cc00};
    for(unsigned i=0;i<5;++i)*(void **)(image+offsets[i])=resolves[i];
    resolvedWrong=1;routeImageAdded((void *)h,(intptr_t)h);assert(!protectCalls);resolvedWrong=0;
    regionError=5;routeImageAdded((void *)h,(intptr_t)h);assert(!protectCalls);regionError=0;
    protectError=7;routeImageAdded((void *)h,(intptr_t)h);assert(protectCalls==1&&*(void **)(image+offsets[0])==resolves[0]);protectError=0;
    routeImageAdded((void *)h,(intptr_t)h);assert(protectCalls==3&&*(void **)(image+offsets[0])==(void *)routeGet);
    AudioObjectPropertyAddress a={'test','outp',2};UInt32 v=0,n=4;
    assert(routeGet(33,&a,12,(void *)42,&n,&v)==-100&&v==17&&n==4);
    assert(routeSize(33,&a,0,NULL,&n)==-101&&n==12);assert(!routeHas(33,&a));
    assert(routeSet(33,&a,0,NULL,4,&v)==-102);
    struct {uint64_t vtable;int32_t status;uint32_t pad;} ca={UINT64_C(0x1234567887654321),17,0};
    const char *caType[2]={NULL,"11CAException"};routeThrow(&ca,caType,NULL);assert(routeCalls==5);
    assert(gRouteNotes[gRouteCount-1].result==17&&gRouteNotes[gRouteCount-1].value==1);
    const char *stdType[2]={NULL,"St13runtime_error"};routeThrow(&v,stdType,NULL);
    assert(gRouteNotes[gRouteCount-1].result==0&&gRouteNotes[gRouteCount-1].value==3);
    unsigned before=gRouteCount;assert(routeSet(33,&a,0,NULL,4,&v)==-102&&gRouteCount==before);
    // Route logs stay queued until a connection exists, then drain separately.
    gConnect=0;flushRouteNotes();assert(!gRouteFlushed);
    assert(gRouteNotes[gRouteCount-1].selector==3&&gRouteNotes[gRouteCount-1].value==0x669eebc5);gConnect=1;flushRouteNotes();assert(gRouteFlushed==gRouteCount);
    for(unsigned i=0;i<200;++i)routeNote(9,-1,i,0,0,0,0);assert(gRouteCount==128);
    free(image);
}
int main(int argc,char **argv){
    properties();routing();
    if(argc==2){
        unsigned char header[4072];FILE *f=fopen(argv[1],"rb");assert(f);
        assert(fread(header,1,sizeof header,f)==sizeof header);fclose(f);
        assert(routeImageMatches((const void *)header));
    }
    puts("PASS actual HAL scope, activation, volume, bounded trace and guarded forwarding");return 0;
}
