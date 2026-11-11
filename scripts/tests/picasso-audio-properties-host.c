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
#include <unistd.h>
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
typedef struct {double mSampleTime;uint64_t mHostTime;double mRateScalar;uint64_t mWordClockTime;unsigned char mSMPTETime[24];UInt32 mFlags,mReserved;} AudioTimeStamp;
typedef struct {UInt64 mIOCycleCounter;UInt32 mNominalIOBufferFrameSize;AudioTimeStamp mCurrentTime,mInputTime,mOutputTime;Float64 mMasterHostTicksPerFrame,mDeviceHostTicksPerFrame;} AudioServerPlugInIOCycleInfo;
_Static_assert(sizeof(AudioTimeStamp)==64,"SDK AudioTimeStamp ABI");
_Static_assert(offsetof(AudioServerPlugInIOCycleInfo,mInputTime)==80,"SDK input timestamp ABI");
_Static_assert(offsetof(AudioServerPlugInIOCycleInfo,mOutputTime)==144,"SDK output timestamp ABI");
struct Host;typedef const struct Host *AudioServerPlugInHostRef;
struct Host {OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress *);};
typedef unsigned io_connect_t,io_service_t;typedef int kern_return_t;
typedef struct {unsigned numer,denom;} mach_timebase_info_data_t;
static uint64_t fakeTime=1000;
static uint64_t mach_absolute_time(void){return fakeTime;}
static int mach_timebase_info(mach_timebase_info_data_t *t){t->numer=t->denom=1;return 0;}
#define MACH_PORT_NULL 0
static unsigned mach_task_self(void){return 1;}
static void *IOServiceMatching(const char *n){return (void *)n;}
static io_service_t IOServiceGetMatchingService(unsigned p,void *m){(void)p;(void)m;return 1;}
static int IOServiceOpen(unsigned s,unsigned t,unsigned k,unsigned *c){(void)s;(void)t;(void)k;*c=1;return 0;}
static void IOObjectRelease(unsigned o){(void)o;}
static int IOServiceClose(unsigned o){(void)o;return 0;}
static unsigned notes,changed;
static int writeResult;static bool *workerStop;static int16_t accepted[2048];
static PicassoAudioStatus fakeStatus={.version=1,.state=PicassoAudioRunning};
static unsigned statusCalls,sleeps,runningAfter,statusSize=sizeof(PicassoAudioStatus),writeCalls;
static int statusResult;static bool inCallback;
static void (*sleepHook)(unsigned);static int (*writeHook)(const int16_t *);
static int fakeSleep(unsigned us){++sleeps;if(sleepHook)sleepHook(us);return 0;}
#define usleep fakeSleep
static int IOConnectCallStructMethod(unsigned c,unsigned s,const void *in,size_t n,void *out,size_t *z){
    (void)c;assert(!inCallback);
    if(s==PicassoAudioGetStatus){
        ++statusCalls;if(runningAfter&&statusCalls>=runningAfter)fakeStatus.state=PicassoAudioRunning;
        assert(out&&z&&*z==sizeof fakeStatus);memcpy(out,&fakeStatus,sizeof fakeStatus);*z=statusSize;return statusResult;
    }
    if(s==PicassoAudioWrite){++writeCalls;assert(n==sizeof accepted);
        if(writeHook)return writeHook(in);
        if(!writeResult)memcpy(accepted,in,n);if(workerStop)*workerStop=true;return writeResult;
    }
    if(s==PicassoAudioNote){assert(in&&n==sizeof(PicassoAudioClientNote));++notes;}return 0;
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
// Only registered fixture bytes may be read; bad pointers never reach memcpy.
static uintptr_t readableBase;static size_t readableSize;static bool shortRead;
static int vm_read_overwrite(unsigned task,vm_address_t address,vm_size_t bytes,vm_address_t target,vm_size_t *copied){
    assert(task==1);*copied=0;
    if(address<readableBase || bytes>readableSize || address-readableBase>readableSize-bytes)return 1;
    *copied=shortRead&&bytes?bytes-1:bytes;memcpy((void *)target,(const void *)address,*copied);return 0;
}
typedef struct {int protection;} vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 1
#define VM_REGION_BASIC_INFO_64 9
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_COPY 16
typedef struct {const char *dli_fname;} Dl_info;
#define RTLD_DEFAULT NULL
static unsigned policyBits;static int policyClass=1;static bool missingPolicy;
static bool fakeFeature(const char *domain,const char *key){assert(!strcmp(domain,"VirtualAudio"));return policyBits&(!strcmp(key,"core_speech_requires_tap_stream")?1:8);}
static bool fakeAnswer(CFStringRef key){return policyBits&(!strcmp(key,"DeviceSupportsSiriBargeIn")?2:4);}
static int fakeNumber(CFStringRef key,int fallback){assert(!strcmp(key,"DeviceClassNumber")&&!fallback);return policyClass;}
static uint64_t fakeProduct(void){return UINT64_C(0x669eebc5);}
static int protectError,protectCalls,regionError,resolvedWrong;static void *resolves[5];
static int dladdr(const void *p,Dl_info *i){(void)p;i->dli_fname="/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio";return 1;}
static void *dlsym(void *h,const char *s){(void)h;if(!strcmp(s,"_os_feature_enabled_impl"))return missingPolicy?NULL:(void *)fakeFeature;if(!strcmp(s,"MGGetBoolAnswer"))return (void *)fakeAnswer;if(!strcmp(s,"MGGetSInt32Answer"))return (void *)fakeNumber;if(!strcmp(s,"MGGetProductType"))return (void *)fakeProduct;const char *n[]={"AudioObjectGetPropertyData","AudioObjectGetPropertyDataSize","AudioObjectHasProperty","AudioObjectSetPropertyData","__cxa_throw"};for(int i=0;i<5;i++)if(!strcmp(s,n[i]))return resolvedWrong?NULL:resolves[i];return NULL;}
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
    gIoUsers=1;AudioServerPlugInIOCycleInfo info={.mOutputTime={.mSampleTime=10}};
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
static void speakerReference(void){
    // Exhaust all original policy combinations, including stereo on non-phone.
    for(policyClass=0;policyClass<3;++policyClass)for(policyBits=0;policyBits<16;++policyBits){
        assert(!configureTap());assert(gTapEnabled==!!((policyBits&11)||((policyBits&4)&&policyClass==1)));
    }
    missingPolicy=true;assert(configureTap()==-1);missingPolicy=false;
    policyBits=2;assert(!configureTap()&&gTapEnabled);
    AudioObjectPropertyAddress a={kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0};UInt32 n,v;CFStringRef uid;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof uid,&n,&uid)&&!strcmp(uid,"Speaker"));
    a.mSelector=kAudioPlugInPropertyTranslateUIDToDevice;
    assert(!GetPropertyData(NULL,kPlugIn,0,&a,sizeof uid,&uid,sizeof v,&n,&v)&&v==kDevice);
    a.mSelector=kAudioDevicePropertyTransportType;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&n,&v)&&v==kAudioDeviceTransportTypeBuiltIn);
    UInt32 list[4];a.mSelector=kAudioObjectPropertyOwnedObjects;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof list,&n,list)&&n==16);
    a.mSelector=kAudioDevicePropertyStreams;a.mScope=kAudioObjectPropertyScopeInput;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof list,&n,list)&&n==4&&list[0]==kTapStream);
    a.mSelector=kAudioDevicePropertyDeviceCanBeDefaultDevice;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&n,&v)&&!v);
    a=(AudioObjectPropertyAddress){'taps',kAudioObjectPropertyScopeGlobal,0};Boolean set=true;
    assert(!GetPropertyDataSize(NULL,kTapStream,0,&a,0,NULL,&n)&&n==4);
    assert(!GetPropertyData(NULL,kTapStream,0,&a,0,NULL,4,&n,&v)&&v==1);
    assert(!IsPropertySettable(NULL,kTapStream,0,&a,&set)&&!set);
    assert(GetPropertyData(NULL,kTapStream,0,&a,0,NULL,1,&n,&v)==(OSStatus)kAudioHardwareBadPropertySizeError);
    assert(!GetPropertyData(NULL,kOutStream,0,&a,0,NULL,4,&n,&v)&&v==0);
    Boolean will=false,inPlace=false;
    assert(!WillDoIOOperation(NULL,kDevice,0,kAudioServerPlugInIOOperationReadInput,&will,&inPlace)&&will&&inPlace);
    memset(&gMix,0,sizeof gMix);memset(&gTap,0,sizeof gTap);gSent=gReservedEnd=0;gIoUsers=1;gFault=0;gStop=false;gVolume=1;gMuted=false;
    float pcm[]={0.5f,-0.25f};AudioServerPlugInIOCycleInfo info={.mInputTime={.mSampleTime=0},.mOutputTime={.mSampleTime=0}};
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    float got[4]={9,9,9,9};
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)&&!got[0]&&!got[1]);
    // Failed kernel submission must not become accepted playback history.
    workerStop=&gStop;writeResult=kIOReturnNoSpace;gHostTicksPerFrame=1;gOrigin=1000;worker(NULL);gStop=false;
    assert(!gSent&&picasso_tap_read(&gTap,0,1,got)==1);
    writeResult=0;worker(NULL);gStop=false;workerStop=NULL;
    assert(gSent==1024&&accepted[0]>0&&accepted[1]<0);
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL));
    assert(got[0]==accepted[0]/32768.0f&&got[1]==accepted[1]/32768.0f);
    // Gain and mute propagate through the accepted PCM, not a second gain path.
    a=(AudioObjectPropertyAddress){kDeviceVolumeScalar,kAudioObjectPropertyScopeOutput,0};float level=0.5f;
    assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof level,&level));
    info.mOutputTime.mSampleTime=1024;
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    workerStop=&gStop;worker(NULL);gStop=false;workerStop=NULL;
    info.mInputTime.mSampleTime=1024;
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL));
    assert(got[0]>0 && got[0]<0.25f && got[0]==accepted[0]/32768.0f);
    a.mSelector=kDeviceMute;v=1;assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&v));
    info.mOutputTime.mSampleTime=2048;
    assert(!DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,1,&info,pcm,NULL));
    workerStop=&gStop;worker(NULL);gStop=false;workerStop=NULL;
    info.mInputTime.mSampleTime=2048;
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)&&!got[0]&&!got[1]);
    v=0;assert(!SetPropertyData(NULL,kDevice,0,&a,0,NULL,sizeof v,&v));
    assert(gVolume==0.5f);
    // A disabled reference must not discard queued speaker samples.
    a=(AudioObjectPropertyAddress){kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal,0};v=0;
    assert(!SetPropertyData(NULL,kTapStream,0,&a,0,NULL,4,&v)&&!gTapActive);
    info.mInputTime.mSampleTime=0;
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)&&!got[0]);
    v=1;assert(!SetPropertyData(NULL,kTapStream,0,&a,0,NULL,4,&v)&&gTapActive);
    // Input and output timestamps differ; no fabricated data from output time.
    info.mInputTime.mSampleTime=4096;
    assert(!DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)&&!got[0]&&!got[1]);
    int16_t clipped[4]={32767,-32768,123,-456};
    picasso_tap_store(&gTap,PicassoMixFrames-1,2,clipped);
    assert(!picasso_tap_read(&gTap,PicassoMixFrames-1,2,got)&&got[0]>0.999f&&got[1]==-1&&got[2]==123/32768.0f);
    assert(picasso_tap_read(&gTap,0,1,got)==1&&got[0]==0);
    info.mInputTime.mSampleTime=NAN;
    assert(DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)==(OSStatus)kAudioHardwareIllegalOperationError);
    info.mInputTime.mSampleTime=0;
    assert(DoIOOperation(NULL,kDevice,kTapStream,0,kAudioServerPlugInIOOperationReadInput,8193,&info,got,NULL)==(OSStatus)kAudioHardwareIllegalOperationError);
    assert(DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationReadInput,1,&info,got,NULL)==(OSStatus)kAudioHardwareIllegalOperationError);
    gTapEnabled=false;a.mSelector='taps';
    assert(!HasProperty(NULL,kTapStream,0,&a));
    assert(!WillDoIOOperation(NULL,kDevice,0,kAudioServerPlugInIOOperationReadInput,&will,&inPlace)&&!will);
    gClients[0]=1;gThreadLive=false;assert(!StopIO(NULL,kDevice,0)&&!gIoUsers);
    assert(picasso_tap_read(&gTap,PicassoMixFrames-1,2,got)==2);
}
static void ownedFilters(void){
    AudioObjectPropertyAddress a={kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,0};
    UInt32 n,list[4],cls;gTapEnabled=true;
    const struct {UInt32 cls,count,first;} cases[]={
        {kAudioObjectClassID,4,kOutStream},{kAudioStreamClassID,2,kOutStream},
        {kAudioControlClassID,2,kVolume},{kAudioLevelControlClassID,1,kVolume},
        {kAudioVolumeControlClassID,1,kVolume},{kAudioBooleanControlClassID,1,kMute},
        {kAudioMuteControlClassID,1,kMute},{kAudioDeviceClassID,0,0},{0xffffffff,0,0}
    };
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i){
        cls=cases[i].cls;
        assert(!GetPropertyDataSize(NULL,kDevice,0,&a,4,&cls,&n)&&n==cases[i].count*4);
        memset(list,0xcc,sizeof list);
        assert(!GetPropertyData(NULL,kDevice,0,&a,4,&cls,sizeof list,&n,list)&&n==cases[i].count*4);
        if(n)assert(list[0]==cases[i].first);
    }
    UInt32 filters[]={kAudioControlClassID,kAudioVolumeControlClassID,kAudioStreamClassID,kAudioControlClassID};
    assert(!GetPropertyData(NULL,kDevice,0,&a,sizeof filters,filters,sizeof list,&n,list)&&n==16);
    assert(list[0]==kOutStream&&list[1]==kTapStream&&list[2]==kVolume&&list[3]==kMute);
    a.mScope=kAudioObjectPropertyScopeInput;
    assert(!GetPropertyData(NULL,kDevice,0,&a,sizeof filters,filters,sizeof list,&n,list)&&n==4&&list[0]==kTapStream);
    a.mScope=kAudioObjectPropertyScopeOutput;
    assert(!GetPropertyData(NULL,kDevice,0,&a,sizeof filters,filters,sizeof list,&n,list)&&n==12&&list[0]==kOutStream&&list[1]==kVolume);
    a.mScope=kAudioObjectPropertyScopeGlobal;cls=kAudioControlClassID;
    assert(!GetPropertyData(NULL,kDevice,0,&a,4,&cls,7,&n,list)&&n==4&&list[0]==kVolume);
    assert(!GetPropertyData(NULL,kDevice,0,&a,4,&cls,0,&n,NULL)&&n==0);
    uint8_t unaligned[5];memcpy(unaligned+1,&cls,4);
    assert(!GetPropertyData(NULL,kDevice,0,&a,4,unaligned+1,sizeof list,&n,list)&&n==8);
    assert(GetPropertyDataSize(NULL,kDevice,0,&a,3,&cls,&n)==(OSStatus)kAudioHardwareBadPropertySizeError&&n==0);
    assert(GetPropertyDataSize(NULL,kDevice,0,&a,4,NULL,&n)==(OSStatus)kAudioHardwareBadPropertySizeError&&n==0);
    cls=kAudioDeviceClassID;
    assert(!GetPropertyData(NULL,kPlugIn,0,&a,4,&cls,sizeof list,&n,list)&&n==4&&list[0]==kDevice);
    cls=kAudioStreamClassID;
    assert(!GetPropertyData(NULL,kPlugIn,0,&a,4,&cls,sizeof list,&n,list)&&n==0);
    assert(!GetPropertyData(NULL,kOutStream,0,&a,0,NULL,sizeof list,&n,list)&&n==0);
    assert(!GetPropertyData(NULL,kVolume,0,&a,0,NULL,sizeof list,&n,list)&&n==0);
    gTapEnabled=false;
    assert(!GetPropertyData(NULL,kDevice,0,&a,4,&cls,sizeof list,&n,list)&&n==4&&list[0]==kOutStream);
    assert(GetPropertyData(NULL,kTapStream,0,&a,0,NULL,sizeof list,&n,list)==(OSStatus)kAudioHardwareBadObjectError);
}
static void registryReset(void){gRouteCount=gRouteFlushed=0;atomic_store(&gRoutePortSamples,0);}
static void registrySummary(int result,unsigned count){
    assert(gRouteCount);PicassoAudioClientNote *n=&gRouteNotes[gRouteCount-1];
    assert(n->kind==14&&n->selector==6&&n->result==result&&n->value==count&&n->missingFrames==6);
}
static void registry(unsigned char *image){
    uintptr_t base=(uintptr_t)image,manager=base+0x640000,head=manager+0x58;
    uintptr_t *nodes=(void *)(image+0x641000);uint8_t *port=image+0x642000;
    *(uintptr_t *)(image+0x63e9d0)=manager;*(uintptr_t *)head=head;
    image[0x63f538]=1;registryReset();routePorts();registrySummary(0,0);
    assert(gRouteNotes[0].calls==1);
    *(uintptr_t *)head=(uintptr_t)nodes;nodes[0]=head;nodes[1]=head;nodes[2]=(uintptr_t)port;
    *(uintptr_t *)port=base+0x629848;*(UInt32 *)(port+0x38)='pspk';*(UInt32 *)(port+0x3c)='pcbi';*(uint16_t *)(port+0x60)=5;
    *(uintptr_t *)(port+0x68)=123;port[0x1b0]=1;
    registryReset();routePorts();registrySummary(0,1);assert(gRouteCount==4);
    assert(gRouteNotes[0].selector==7&&gRouteNotes[0].value=='pspk'&&gRouteNotes[0].calls==5&&gRouteNotes[0].sentFrames==1);
    assert(gRouteNotes[1].selector==10&&gRouteNotes[1].calls=='pcbi');
    assert(gRouteNotes[2].selector==8&&gRouteNotes[2].calls==0x629848&&gRouteNotes[2].sentFrames==1);
    *(uintptr_t *)port=base+0x611780;registryReset();routePorts();registrySummary(0,1);
    assert(gRouteNotes[0].sentFrames==UINT32_MAX);*(uintptr_t *)port=base+0x629848;
    nodes[0]=(uintptr_t)nodes;registryReset();routePorts();registrySummary(-3,1);nodes[0]=head;
    nodes[2]=UINTPTR_MAX-8;registryReset();routePorts();registrySummary(-4,1);nodes[2]=(uintptr_t)port;
    *(uintptr_t *)port=base;registryReset();routePorts();registrySummary(-5,'pspk');*(uintptr_t *)port=base+0x629848;
    for(unsigned i=0;i<17;++i){nodes[i*3]=i==16?head:(uintptr_t)&nodes[(i+1)*3];nodes[i*3+2]=(uintptr_t)port;}
    registryReset();routePorts();registrySummary(-6,16);
    nodes[0]=head;shortRead=true;registryReset();routePorts();registrySummary(-1,0);shortRead=false;
    *(uintptr_t *)(image+0x63e9d0)=UINTPTR_MAX;registryReset();routePorts();registrySummary(-1,0);
    *(uintptr_t *)(image+0x63e9d0)=base+readableSize-0x60;registryReset();routePorts();registrySummary(-4,1);
    *(uintptr_t *)(image+0x63e9d0)=base+readableSize;registryReset();routePorts();registrySummary(-2,0);
    *(uintptr_t *)(image+0x63e9d0)=manager;registryReset();gRoutePortLayout=false;routePorts();assert(!gRouteCount);gRoutePortLayout=true;
    struct {uint64_t vtable;int32_t status;uint32_t pad;} ca={0,0x6e6f7274,0};const char *type[2]={NULL,"11CAException"};
    int before=routeCalls;routeThrow(&ca,type,NULL);assert(routeCalls==before+1);registrySummary(0,1);
    routePorts();routePorts();unsigned count=gRouteCount;
    for(unsigned i=0;i<100;++i)routePorts();assert(gRouteCount==count&&atomic_load(&gRoutePortSamples)==3);
    UInt32 v;assert(!routeRead(0,&v,4)&&!routeRead(UINTPTR_MAX-1,&v,4));
    registryReset();
}
static void routing(void){
    gRouteCount=gRouteFlushed=0;
    unsigned char *image=aligned_alloc(16384,0x664000);assert(image);memset(image,0,0x664000);struct mach_header_64 *h=(void *)image;readableBase=(uintptr_t)image;readableSize=0x664000;
    *h=(struct mach_header_64){MH_MAGIC_64,CPU_TYPE_ARM64,CPU_SUBTYPE_ARM64_ALL,MH_DYLIB,36,4040,0,0};
    struct uuid_command *u=(void *)(h+1);*u=(struct uuid_command){LC_UUID,24,{0x31,0x96,0x33,0x3f,0x5a,0x48,0x37,0x3b,0xb4,0x27,0x01,0x46,0xdc,0x11,0x77,0xfb}};
    struct segment_command_64 *s=(void *)(u+1);s->cmd=LC_SEGMENT_64;s->cmdsize=sizeof *s+sizeof(struct section_64);strcpy(s->segname,"__DATA_CONST");s->vmaddr=0x60c000;s->vmsize=0x28000;s->nsects=1;
    struct section_64 *sec=(void *)(s+1);strcpy(sec->sectname,"__got");sec->addr=0x60c000;sec->size=0x1300;sec->flags=S_NON_LAZY_SYMBOL_POINTERS;
    struct segment_command_64 *data=(void *)(sec+1);*data=(struct segment_command_64){.cmd=LC_SEGMENT_64,.cmdsize=sizeof *data,.vmaddr=0x634000,.vmsize=0x30000,.filesize=0x4000};strcpy(data->segname,"__DATA");
    const uintptr_t wordOffsets[]={0x120d0,0x120d4,0xa6bec,0xa6bf0,0x3e664,0x1b70e4};
    const UInt32 words[]={0x90003168,0xf944e900,0xf9400288,0xf9400918,0xb9403a6a,0x3906c268};
    for(unsigned i=0;i<6;++i)memcpy(image+wordOffsets[i],&words[i],4);
    unsigned char *at=(void *)(data+1);for(unsigned i=0;i<33;++i){struct load_command *lc=(void *)at;lc->cmd=0;lc->cmdsize=i==32?4040-(unsigned)(at-(unsigned char *)(h+1)):8;at+=lc->cmdsize;}
    assert(routeImageMatches(h));assert(routePortLayoutMatches(h));
    const UInt32 routeWords[]={0x52800161,0x52800003,0x528c2ee4,0x72aecd04,0x528c2ee5,
        0x72ac6d05,0x528c8cc6,0x72ad2da6,0x52800007,0x940015bb};
    memcpy(image+0x32aeec,routeWords,sizeof routeWords);
    assert(routeOptionalVariant((uintptr_t)h)==0);
    image[0x32aef0]=0x23;assert(routeOptionalVariant((uintptr_t)h)==1);
    for(unsigned i=0;i<10;++i){image[0x32aeec+i*4]^=1;assert(routeOptionalVariant((uintptr_t)h)==-1);image[0x32aeec+i*4]^=1;}
    shortRead=true;assert(routeOptionalVariant((uintptr_t)h)==-1);shortRead=false;
    data->filesize=0;assert(!routePortLayoutMatches(h));data->filesize=0x4000;
    for(unsigned i=0;i<6;++i){image[wordOffsets[i]]^=1;assert(!routePortLayoutMatches(h));image[wordOffsets[i]]^=1;}
    shortRead=true;assert(!routePortLayoutMatches(h));shortRead=false;
    u->uuid[0]^=1;assert(!routeImageMatches(h));u->uuid[0]^=1;
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
    registry(image);free(image);readableBase=readableSize=0;
}

// Drive the actual worker and DoIOOperation for seconds of periodic IO,
// including wraparound, callback jitter and a DSP pause that fills the ring.
// This models callback schedules; it does not emulate the iOS audio server.
static uint64_t due,firstOutput,modelNow,modelDspDue,modelReturned,modelAccepted,modelStop,pauseUntil;
static unsigned modelFrames,modelJitter,modelNoSpace,modelCallbacks;static bool modelPaused;
static float waveform(uint64_t frame){return frame>=100000&&frame<102000?0:(float)((int)(frame%97)-48)/96;}
static void modelSleep(unsigned us){
    modelNow+=(uint64_t)us*48;fakeTime=gOrigin+modelNow;
    uint64_t frame=modelNow/1000;
    if(!modelPaused && frame>=96000){modelPaused=true;pauseUntil=frame+40000;modelDspDue+=40000;}
    while(frame>=modelDspDue && frame>=pauseUntil && modelAccepted-modelReturned>=4096){
        modelReturned+=4096;modelDspDue+=4096;
    }
    fakeStatus.submittedBytes=modelAccepted*4;fakeStatus.returnedBytes=modelReturned*4;
    fakeStatus.queuedBytes=(UInt32)((modelAccepted-modelReturned)*4);
    while(frame>=due+modelJitter){
        float samples[16384];uint64_t at=due+kSafetyFrames;
        for(unsigned i=0;i<modelFrames;++i)samples[i*2]=samples[i*2+1]=waveform(at+i);
        AudioServerPlugInIOCycleInfo info={.mIOCycleCounter=++modelCallbacks,.mNominalIOBufferFrameSize=modelFrames,
            .mCurrentTime={.mSampleTime=(double)frame},.mOutputTime={.mSampleTime=(double)at}};
        inCallback=true;int rc=DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,modelFrames,&info,samples,NULL);inCallback=false;
        assert(!rc);due+=modelFrames;
    }
    if(gSent>=modelStop)gStop=true;
}
static int modelWrite(const int16_t *pcm){
    assert(gReservedEnd==gSent+1024);
    if(modelAccepted-modelReturned+1024>32768){++modelNoSpace;return kIOReturnNoSpace;}
    for(unsigned i=0;i<1024;++i){
        uint64_t at=gSent+i;int16_t expected=at<firstOutput?0:(int16_t)(waveform(at)*32767);
        assert(pcm[2*i]==expected&&pcm[2*i+1]==expected);
    }
    modelAccepted+=1024;return 0;
}
static void continuousTimeline(unsigned frames,unsigned jitter){
    memset(&gMix,0,sizeof gMix);memset(&gTap,0,sizeof gTap);
    gIoUsers=1;gFault=0;gStop=false;gStreamActive=true;gVolume=1;gMuted=false;
    gOrigin=1000;gHostTicksPerFrame=1000;fakeTime=1000;gSent=gMissing=gReservedEnd=gCalls=0;
    gTimingCount=gTimingFlushed=gTimingTotal=gLastFrames=0;
    due=22528;firstOutput=due+kSafetyFrames;modelNow=modelReturned=modelAccepted=0;
    modelStop=240640;modelFrames=frames;modelJitter=jitter;modelNoSpace=modelCallbacks=0;
    modelDspDue=4096;modelPaused=false;pauseUntil=0;writeHook=modelWrite;sleepHook=modelSleep;
    worker(NULL);writeHook=NULL;sleepHook=NULL;
    assert(!gFault&&gMissing==firstOutput&&modelNoSpace>0&&gSent==modelAccepted);
    assert(modelAccepted-modelReturned<=32768 && gTimingTotal<=12);
    float got[2048];assert(!picasso_tap_read(&gTap,gSent-1024,1024,got));
    for(unsigned i=0;i<1024;++i)assert(got[2*i]==(int16_t)(waveform(gSent-1024+i)*32767)/32768.0f);
    printf("PASS continuous IO frames=%u jitter=%u callbacks=%u missing_initial=%llu backpressure=%u\n",
        frames,jitter,modelCallbacks,(unsigned long long)gMissing,modelNoSpace);
    gStop=false;
}
static void timeline(void){
    // Exercise the production readiness wait, including refusal and bounded timeout.
    statusCalls=sleeps=0;runningAfter=4;fakeStatus.state=PicassoAudioStarting;
    assert(!waitRunning()&&statusCalls==4&&sleeps==3);
    runningAfter=0;statusCalls=sleeps=0;fakeStatus.state=PicassoAudioStarting;
    assert(waitRunning()==kIOReturnBusy&&statusCalls==200&&sleeps==200);
    fakeStatus.state=PicassoAudioDraining;assert(waitRunning()==kIOReturnNotReady);
    fakeStatus.state=PicassoAudioIdle;assert(waitRunning()==kIOReturnNotReady);
    fakeStatus.state=PicassoAudioFailed;assert(waitRunning()==kIOReturnNotReady);
    fakeStatus.state=PicassoAudioRunning;fakeStatus.version=2;assert(waitRunning()==kIOReturnBadArgument);
    fakeStatus.version=1;statusSize=4;assert(waitRunning()==kIOReturnBadArgument);statusSize=sizeof fakeStatus;
    statusResult=kIOReturnNotReady;assert(waitRunning()==kIOReturnNotReady);statusResult=0;

    // Keep the continuous-playback clock period and independent device latency.
    gOrigin=1000;gHostTicksPerFrame=2;gSeed=17;double sample;UInt64 host,seed;
    fakeTime=1000+32767*2;assert(!GetZeroTimeStamp(NULL,kDevice,0,&sample,&host,&seed));
    assert(sample==0&&host==1000&&seed==17);
    fakeTime=1000+65537*2;assert(!GetZeroTimeStamp(NULL,kDevice,0,&sample,&host,&seed));
    assert(sample==65536&&host==132072&&seed==17);
    AudioObjectPropertyAddress a={kAudioDevicePropertySafetyOffset,kAudioObjectPropertyScopeOutput,0};UInt32 n,v;
    assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&v)&&v==8192);
    a.mSelector=kAudioDevicePropertyLatency;assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&v)&&v==16384);
    a.mSelector=kAudioDevicePropertyZeroTimeStampPeriod;assert(!GetPropertyData(NULL,kDevice,0,&a,0,NULL,4,&n,&v)&&v==32768);
    continuousTimeline(256,0);continuousTimeline(384,1536);continuousTimeline(1024,2048);continuousTimeline(8192,2048);
}

int main(int argc,char **argv){
    // Match the timebase initialization performed by the plugin factory.
    gOrigin=1000;gHostTicksPerFrame=1;
    properties();speakerReference();ownedFilters();routing();timeline();
    if(argc==2){
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0x634000);rewind(f);
        unsigned char *file=malloc((size_t)bytes);assert(file);assert(fread(file,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
        readableBase=(uintptr_t)file;readableSize=(size_t)bytes;
        assert(routeImageMatches((const void *)file)&&routePortLayoutMatches((const void *)file));
        assert(routeOptionalVariant((uintptr_t)file)>=0);
        free(file);readableBase=readableSize=0;
    }
    puts("PASS actual HAL scope, activation, volume, bounded trace and guarded forwarding");return 0;
}
