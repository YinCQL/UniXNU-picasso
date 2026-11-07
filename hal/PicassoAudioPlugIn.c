// SPDX-License-Identifier: BSD-2-Clause-Patent
// Public HAL object/property pattern adapted from UniXNU-sheng 4601795.
// Picasso speaker and software playback reference; no microphone capture.
// No shared kernel memory or clock patching.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include "PicassoAudioAbi.h"
#include "PicassoAudioMix.h"
enum {kPlugIn=kAudioObjectPlugInObject,kDevice=2,kOutStream=3,kVolume=4,kMute=5,kTapStream=6};
// Legacy device aliases from CoreAudio/AudioHardware.h (not included by
// AudioServerPlugIn.h). The host check verifies these against that SDK header.
enum {kDeviceVolumeScalar='volm',kDeviceVolumeDecibels='vold',
      kDeviceVolumeRangeDecibels='vdb#',kDeviceMute='mute'};
// 20H392 VirtualAudio dispatches built-in routes by UID, not display name.
#define kDeviceUID "Speaker"
#define kModelUID "PicassoAudioModel"
static const Float64 kRate=48000;
static const UInt32 kZeroPeriod=32768;
static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs=1;
static io_connect_t gConnect;
static UInt32 gIoUsers;
static uint32_t gClients[32];
static Float32 gVolume=1.0f;
static bool gMuted;
static Float64 gHostTicksPerFrame;
static uint64_t gOrigin,gSeed;
static bool gStop,gThreadLive;
static int gFault;
static pthread_t gThread,gWatch;
static bool gWatchLive;
static PicassoAudioMix gMix;
static PicassoAudioTap gTap;
static bool gTapEnabled,gTapActive=true;
static uint64_t gTapReads,gTapMissing;
static uint64_t gSent,gMissing,gReservedEnd;
static uint32_t gCalls,gPropertyNotes;
static bool gStreamActive=true;
static struct {AudioObjectID object;AudioObjectPropertyAddress address;unsigned operation;OSStatus result;} gPropertySeen[128];
static int16_t gChunk[2048];
static PicassoAudioFormat gFormat={1,48000,2,16};
static const Float32 kMinDb=-36;
static Float32 toDb(Float32 v){v=v<0?0:v>1?1:v;return kMinDb*(1-v)*(1-v);}
static Float32 toScalar(Float32 d){return d<=kMinDb?0:d>=0?1:1-sqrtf(d/kMinDb);}
static Float32 gain(void){return gMuted||gVolume<=0?0:powf(10,toDb(gVolume)/20);}
// Conservative scheduling lead; not a measurement of speaker render position.
static UInt32 latencyFrames(void){return 16384;}
static kern_return_t method(unsigned s,const void *in,size_t n,void *out,size_t *z){
    return IOConnectCallStructMethod(gConnect,s,in,n,out,z);
}
static void note(unsigned kind,int result,unsigned selector,unsigned value){
    if(!gConnect)return;
    PicassoAudioClientNote n={1,kind,result,selector,value,0,gCalls,gSent,gMissing};
    (void)method(PicassoAudioNote,&n,sizeof n,NULL,NULL);
}
#include "PicassoAudioRouteTrace.h"
static int connectService(void){
    if(gConnect)return 0;
    io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching(PICASSO_AUDIO_CLASS));
    if(!service)return -1;
    kern_return_t rc=IOServiceOpen(service,mach_task_self(),0,&gConnect);IOObjectRelease(service);
    if(rc){gConnect=0;return (int)rc;}
    PicassoAudioStatus s;size_t z=sizeof s;
    rc=method(PicassoAudioGetStatus,NULL,0,&s,&z);
    if(rc || z!=sizeof s || s.version!=1){IOServiceClose(gConnect);gConnect=0;return -2;}
    note(1,0,0,0);return 0;
}
static AudioStreamBasicDescription format(UInt32 channels){
    AudioStreamBasicDescription f={0};f.mSampleRate=kRate;f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked;
    f.mBytesPerPacket=f.mBytesPerFrame=4*channels;f.mFramesPerPacket=1;f.mChannelsPerFrame=channels;f.mBitsPerChannel=32;return f;
}
#define REPLY(type,value) do {if(inDataSize<sizeof(type))return kAudioHardwareBadPropertySizeError;*(type *)outData=(value);*outDataSize=sizeof(type);return 0;} while(0)
#define REPLY_STR(s) REPLY(CFStringRef,CFSTR(s))
static HRESULT QueryInterface(void *,REFIID,LPVOID *);
static ULONG AddRef(void *self){(void)self;return atomic_fetch_add(&gRefs,1)+1;}
static ULONG Release(void *self){(void)self;unsigned n=atomic_load(&gRefs);while(n && !atomic_compare_exchange_weak(&gRefs,&n,n-1)){}return n?n-1:0;}
// The audio server can load this plugin before the DSP selftests publish the
// service. Retry connection and notify the host when this real device is ready.
static void *watchService(void *arg){
    (void)arg;for(;;){
        pthread_mutex_lock(&gLock);bool before=gConnect!=0;
        if(!gStop&&!gConnect)(void)connectService();bool changed=!before&&gConnect;
        pthread_mutex_unlock(&gLock);
        if(changed&&gHost){
            // LoadingConditions normally defers initialization until the service
            // exists. Keep recovery notification for hosts that load early.
            AudioObjectPropertyAddress a={kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMaster};
            OSStatus rc=gHost->PropertiesChanged(gHost,kPlugIn,1,&a);
            pthread_mutex_lock(&gLock);note(8,rc,a.mSelector,kPlugIn);pthread_mutex_unlock(&gLock);
            a.mSelector=kAudioDevicePropertyDeviceIsAlive;
            rc=gHost->PropertiesChanged(gHost,kDevice,1,&a);
            pthread_mutex_lock(&gLock);note(8,rc,a.mSelector,kDevice);pthread_mutex_unlock(&gLock);
        }
        flushRouteNotes();
        usleep(1000000);
    }return NULL;
}
// Match the original 20H392 Speaker policy (VA 0x3cfa1c). Do not change
// MobileGestalt or feature flags. This only decides whether our real software
// reference stream is published; it never advertises a microphone.
static bool tapPolicy(bool speech,bool barge,bool stereo,int deviceClass,bool expanse){
    return speech || barge || (stereo && deviceClass==1) || expanse;
}
static int configureTap(void){
    bool (*feature)(const char *,const char *)=(void *)dlsym(RTLD_DEFAULT,"_os_feature_enabled_impl");
    bool (*answer)(CFStringRef)=(void *)dlsym(RTLD_DEFAULT,"MGGetBoolAnswer");
    int (*number)(CFStringRef,int)=(void *)dlsym(RTLD_DEFAULT,"MGGetSInt32Answer");
    if(!feature || !answer || !number)return -1;
    bool speech=feature("VirtualAudio","core_speech_requires_tap_stream");
    bool barge=answer(CFSTR("DeviceSupportsSiriBargeIn"));
    bool stereo=answer(CFSTR("DeviceSupportsStereoAudioRecording"));
    int deviceClass=number(CFSTR("DeviceClassNumber"),0);
    bool expanse=feature("VirtualAudio","expanseEnabled");
    gTapEnabled=tapPolicy(speech,barge,stereo,deviceClass,expanse);
    routeNote(14,0,4,gTapEnabled,speech|(barge<<1)|(stereo<<2)|(expanse<<3),(unsigned)deviceClass,5);
    return 0;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d,AudioServerPlugInHostRef host){
    (void)d;if(configureTap())return kAudioHardwareUnspecifiedError;gHost=host;pthread_mutex_lock(&gLock);(void)connectService();
    if(!gWatchLive){if(!pthread_create(&gWatch,NULL,watchService,NULL)){gWatchLive=true;pthread_detach(gWatch);}}
    note(8,0,0,gConnect!=0);
    pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d,CFDictionaryRef a,const AudioServerPlugInClientInfo *b,AudioObjectID *c){(void)d;(void)a;(void)b;(void)c;return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d,AudioObjectID o){(void)d;(void)o;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c){(void)d;(void)c;pthread_mutex_lock(&gLock);note(8,0,1,o);pthread_mutex_unlock(&gLock);return o==kDevice?0:kAudioHardwareBadObjectError;}
static OSStatus StopIO(AudioServerPlugInDriverRef,AudioObjectID,UInt32);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c){return c?StopIO(d,o,c->mClientID):kAudioHardwareIllegalOperationError;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 a,void *i){(void)d;(void)o;(void)a;(void)i;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 a,void *i){(void)d;(void)o;(void)a;(void)i;return kAudioHardwareUnsupportedOperationError;}
static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (gTapEnabled && scope != kAudioObjectPropertyScopeOutput) list[n++] = kTapStream;
        if (scope != kAudioObjectPropertyScopeInput) {list[n++] = kVolume;list[n++] = kMute;}
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    if(o==kTapStream && !gTapEnabled)return kAudioHardwareBadObjectError;
    if (o >= kPlugIn && o <= kTapStream && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = q && qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        // Master output aliases used by VirtualAudio's physical port controls.
        // The two PCM channels share the one real speaker gain/mute state.
        case kDeviceVolumeScalar:
        case kDeviceVolumeDecibels:
        case kDeviceVolumeRangeDecibels:
        case kDeviceMute:
            if(a->mScope!=kAudioObjectPropertyScopeOutput || a->mElement!=kAudioObjectPropertyElementMaster)
                return kAudioHardwareUnknownPropertyError;
            if(s==kDeviceVolumeScalar) {REPLY(Float32,gVolume);}
            if(s==kDeviceVolumeDecibels) {REPLY(Float32,toDb(gVolume));}
            if(s==kDeviceMute) {REPLY(UInt32,gMuted);}
            REPLY(AudioValueRange,((AudioValueRange){kMinDb,0.0}));
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, gConnect != 0);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0 && !gFault);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 4096);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 0);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput && !gTapEnabled ? 0 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput && !gTapEnabled ? 0 : 2;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            if (channels) l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[4];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && list[i] != kOutStream && list[i] != kTapStream) continue;
                if (s == kAudioObjectPropertyControlList && (list[i] == kOutStream || list[i] == kTapStream)) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kTapStream:
    {
        const UInt32 channels = 2;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if(o==kTapStream){REPLY_STR("Picasso Playback Reference");} REPLY_STR("Picasso Speaker");
        // Native UInt32 stream property in iOS16 CoreAudio (not a custom CF value).
        case 'taps':
            if(a->mScope!=kAudioObjectPropertyScopeGlobal || a->mElement!=0)return kAudioHardwareUnknownPropertyError;
            REPLY(UInt32,o==kTapStream);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, o==kTapStream?gTapActive:gStreamActive);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o==kTapStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o==kTapStream?kAudioStreamTerminalTypeLine:kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kMute:
        switch(s){
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID,kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID,kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID,kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize=0;return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope,kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement,kAudioObjectPropertyElementMaster);
        case kAudioBooleanControlPropertyValue: REPLY(UInt32,gMuted);
        default:return kAudioHardwareUnknownPropertyError;
        }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMaster);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}


// Exact keys retain input/output scope and distinguish Has/Size/Get/Set. A hash
// bit previously hid unrelated selectors and merged all three query operations.
static void propertyNote(AudioObjectID o,const AudioObjectPropertyAddress *a,unsigned operation,OSStatus rc){
    if(!gConnect || gPropertyNotes>=128)return;
    for(unsigned i=0;i<gPropertyNotes;++i){
        if(gPropertySeen[i].object==o && gPropertySeen[i].address.mSelector==a->mSelector &&
           gPropertySeen[i].address.mScope==a->mScope && gPropertySeen[i].address.mElement==a->mElement &&
           gPropertySeen[i].operation==operation && gPropertySeen[i].result==rc)return;
    }
    gPropertySeen[gPropertyNotes].object=o;gPropertySeen[gPropertyNotes].address=*a;
    gPropertySeen[gPropertyNotes].operation=operation;gPropertySeen[gPropertyNotes++].result=rc;
    PicassoAudioClientNote n={1,7,rc,a->mSelector,o,0,a->mScope,a->mElement,operation};
    (void)method(PicassoAudioNote,&n,sizeof n,NULL,NULL);
}
static OSStatus queryProperty(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,UInt32 *z,void *v,unsigned operation){
    if(!a||!z||(!v&&n))return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);*z=0;OSStatus rc=propertyData(o,a,qs,q,n,z,v);
    propertyNote(o,a,operation,rc);pthread_mutex_unlock(&gLock);return rc;
}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,UInt32 *z,void *v){
    (void)d;(void)pid;return queryProperty(o,a,qs,q,n,z,v,0);
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *z){
    (void)d;(void)pid;union {uint64_t align;uint8_t b[1024];} v={0};return queryProperty(o,a,qs,q,sizeof v,z,&v,1);
}
static Boolean HasProperty(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a){
    (void)d;(void)pid;UInt32 n;union {uint64_t align;uint8_t b[1024];} v={0};return !queryProperty(o,a,0,NULL,sizeof v,&n,&v,2);
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,Boolean *set){
    if(!a||!set)return kAudioHardwareIllegalOperationError;
    bool outputMaster=o==kDevice && a->mScope==kAudioObjectPropertyScopeOutput && a->mElement==kAudioObjectPropertyElementMaster;
    *set=(outputMaster&&(a->mSelector==kDeviceVolumeScalar||a->mSelector==kDeviceVolumeDecibels||a->mSelector==kDeviceMute))||
        (o==kMute&&a->mSelector==kAudioBooleanControlPropertyValue)||
        (o==kVolume&&(a->mSelector==kAudioLevelControlPropertyScalarValue||a->mSelector==kAudioLevelControlPropertyDecibelValue))||
        ((o==kOutStream||(o==kTapStream&&gTapEnabled))&&(a->mSelector==kAudioStreamPropertyVirtualFormat||a->mSelector==kAudioStreamPropertyPhysicalFormat||a->mSelector==kAudioStreamPropertyIsActive))||
        (o==kDevice&&a->mSelector==kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d,o,pid,a)?0:kAudioHardwareUnknownPropertyError;
}
static OSStatus setPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,const void *v){
    (void)d;(void)pid;(void)qs;(void)q;if(!a||!v)return kAudioHardwareIllegalOperationError;
    bool outputMaster=o==kDevice && a->mScope==kAudioObjectPropertyScopeOutput && a->mElement==kAudioObjectPropertyElementMaster;
    if((o==kMute&&a->mSelector==kAudioBooleanControlPropertyValue)||(outputMaster&&a->mSelector==kDeviceMute)){
        if(n!=sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
        pthread_mutex_lock(&gLock);gMuted=*(const UInt32 *)v!=0;
        // Drop software mixes prepared before mute; in-flight DMA still retires
        // normally. Future writes retain timing and contain actual zero samples.
        if(gMuted)memset(&gMix,0,sizeof gMix);
        note(6,0,kDeviceMute,gMuted);pthread_mutex_unlock(&gLock);
        AudioObjectPropertyAddress changed={kAudioBooleanControlPropertyValue,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMaster};
        if(gHost){gHost->PropertiesChanged(gHost,kMute,1,&changed);changed=(AudioObjectPropertyAddress){kDeviceMute,kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMaster};gHost->PropertiesChanged(gHost,kDevice,1,&changed);}
        return 0;
    }
    if((o==kVolume&&(a->mSelector==kAudioLevelControlPropertyScalarValue||a->mSelector==kAudioLevelControlPropertyDecibelValue))||
       (outputMaster&&(a->mSelector==kDeviceVolumeScalar||a->mSelector==kDeviceVolumeDecibels))){
        if(n!=sizeof(Float32))return kAudioHardwareBadPropertySizeError;float x=*(const Float32 *)v;if(!isfinite(x))return kAudioHardwareIllegalOperationError;
        if(a->mSelector==kAudioLevelControlPropertyDecibelValue||a->mSelector==kDeviceVolumeDecibels)x=toScalar(x);
        pthread_mutex_lock(&gLock);gVolume=x<0?0:x>1?1:x;note(6,0,a->mSelector,(unsigned)(gVolume*1000));pthread_mutex_unlock(&gLock);
        AudioObjectPropertyAddress changed[2]={{kAudioLevelControlPropertyScalarValue,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMaster},{kAudioLevelControlPropertyDecibelValue,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMaster}};
        if(gHost){gHost->PropertiesChanged(gHost,kVolume,2,changed);
            changed[0]=(AudioObjectPropertyAddress){kDeviceVolumeScalar,kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMaster};
            changed[1]=(AudioObjectPropertyAddress){kDeviceVolumeDecibels,kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMaster};
            gHost->PropertiesChanged(gHost,kDevice,2,changed);}
        return 0;
    }
    if((o==kOutStream||(o==kTapStream&&gTapEnabled))&&a->mSelector==kAudioStreamPropertyIsActive){
        if(n!=sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
        pthread_mutex_lock(&gLock);bool active=*(const UInt32 *)v!=0;
        bool *state=o==kTapStream?&gTapActive:&gStreamActive,changed=active!=*state;
        *state=active;
        // Disabling the stream discards queued software mixes; DMA already
        // submitted remains owned by the worker and completes normally.
        if(!active && o==kOutStream)memset(&gMix,0,sizeof gMix);
        pthread_mutex_unlock(&gLock);
        if(changed&&gHost)gHost->PropertiesChanged(gHost,o,1,a);
        return 0;
    }
    if((o==kOutStream||(o==kTapStream&&gTapEnabled))&&(a->mSelector==kAudioStreamPropertyVirtualFormat||a->mSelector==kAudioStreamPropertyPhysicalFormat)){
        AudioStreamBasicDescription f=format(2);return n==sizeof f&&!memcmp(v,&f,sizeof f)?0:kAudioDeviceUnsupportedFormatError;
    }
    if(o==kDevice&&a->mSelector==kAudioDevicePropertyNominalSampleRate)return n==sizeof(Float64)&&*(const Float64 *)v==kRate?0:kAudioDeviceUnsupportedFormatError;
    return kAudioHardwareUnknownPropertyError;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,const void *v){
    OSStatus rc=setPropertyData(d,o,pid,a,qs,q,n,v);
    if(a){pthread_mutex_lock(&gLock);propertyNote(o,a,3,rc);pthread_mutex_unlock(&gLock);}
    return rc;
}
// Only the worker calls the kernel writer. The real-time IO callbacks enqueue
// time-indexed mixes without waiting for ring space or a DSP transaction.
static void *worker(void *unused){
    (void)unused;
    for(;;){
        pthread_mutex_lock(&gLock);if(gStop){pthread_mutex_unlock(&gLock);break;}
        uint64_t now=mach_absolute_time();uint64_t frame=now>gOrigin?(uint64_t)((now-gOrigin)/gHostTicksPerFrame):0;
        if(gSent>frame+4096){pthread_mutex_unlock(&gLock);usleep(5000);continue;}
        uint64_t missing=picasso_mix_peek(&gMix,gSent,1024,gChunk);gReservedEnd=gSent+1024;
        pthread_mutex_unlock(&gLock);
        kern_return_t rc=method(PicassoAudioWrite,gChunk,sizeof gChunk,NULL,NULL);
        pthread_mutex_lock(&gLock);
        gReservedEnd=0;
        if(!rc){picasso_tap_store(&gTap,gSent,1024,gChunk);picasso_mix_consume(&gMix,gSent,1024);gSent+=1024;gMissing+=missing;}
        else if(rc!=kIOReturnNoSpace){gFault=(int)rc;note(4,gFault,2,0);pthread_mutex_unlock(&gLock);break;}
        if((gSent&16383)==0)note(3,0,0,0);
        pthread_mutex_unlock(&gLock);usleep(rc?5000:1000);
    }
    return NULL;
}
// A stop requests asynchronous DSP cleanup. Keep the service connection and
// device identity alive, but wait for the actual idle state before a new Begin.
// This runs on StartIO's control thread, never on the realtime mix callback.
static kern_return_t waitIdle(void){
    for(unsigned retry=0;retry<200;++retry){
        PicassoAudioStatus status;size_t size=sizeof status;
        kern_return_t rc=method(PicassoAudioGetStatus,NULL,0,&status,&size);
        if(rc)return rc;
        if(size!=sizeof status || status.version!=1)return kIOReturnBadArgument;
        if(status.state==PicassoAudioIdle)return 0;
        if(status.state==PicassoAudioFailed)return kIOReturnNotReady;
        usleep(10000);
    }
    return kIOReturnBusy;
}
static OSStatus StartIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client){
    (void)d;if(o!=kDevice||client==UINT32_MAX)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);unsigned slot=32;
    for(unsigned i=0;i<32;i++){if(gClients[i]==client+1){pthread_mutex_unlock(&gLock);return 0;}if(!gClients[i])slot=i;}
    if(slot==32||gStop||(gIoUsers&&gFault)){pthread_mutex_unlock(&gLock);return kAudioHardwareIllegalOperationError;}
    if(!gIoUsers){
        if(connectService()){pthread_mutex_unlock(&gLock);return kAudioHardwareNotRunningError;}
        kern_return_t rc=waitIdle();
        if(!rc)rc=method(PicassoAudioBegin,&gFormat,sizeof gFormat,NULL,NULL);
        if(rc){note(2,(int)rc,1,0);pthread_mutex_unlock(&gLock);return kAudioHardwareNotRunningError;}
        memset(&gTap,0,sizeof gTap);gTapReads=gTapMissing=0;
        memset(&gMix,0,sizeof gMix);gSent=gMissing=gReservedEnd=gCalls=0;gFault=0;gOrigin=mach_absolute_time();++gSeed;
        int e=pthread_create(&gThread,NULL,worker,NULL);if(e){(void)method(PicassoAudioCancel,NULL,0,NULL,NULL);pthread_mutex_unlock(&gLock);return kAudioHardwareUnspecifiedError;}
        gThreadLive=true;note(2,0,1,0);
    }
    gClients[slot]=client+1;++gIoUsers;pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client){
    (void)d;if(o!=kDevice)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);unsigned slot=32;for(unsigned i=0;i<32;i++)if(gClients[i]&&gClients[i]==client+1)slot=i;
    if(slot==32){pthread_mutex_unlock(&gLock);return 0;}gClients[slot]=0;--gIoUsers;
    if(gIoUsers){pthread_mutex_unlock(&gLock);return 0;}
    gStop=true;bool join=gThreadLive;pthread_mutex_unlock(&gLock);if(join)pthread_join(gThread,NULL);
    pthread_mutex_lock(&gLock);gThreadLive=false;(void)method(PicassoAudioCancel,NULL,0,NULL,NULL);note(2,0,4,0);
    memset(&gTap,0,sizeof gTap);
    // The device still exists after StopIO. Keep this exclusive connection;
    // closing it here would report DeviceIsAlive=0 between previews.
    gStop=false;pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client,Float64 *sample,UInt64 *host,UInt64 *seed){
    (void)d;(void)client;if(o!=kDevice||!sample||!host||!seed)return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);uint64_t now=mach_absolute_time(),elapsed=now>gOrigin?now-gOrigin:0;
    uint64_t period=(uint64_t)(elapsed/(gHostTicksPerFrame*kZeroPeriod));*sample=(Float64)(period*kZeroPeriod);
    *host=gOrigin+(uint64_t)(*sample*gHostTicksPerFrame);*seed=gSeed;pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client,UInt32 op,Boolean *will,Boolean *inPlace){
    (void)d;(void)client;if(o!=kDevice||!will||!inPlace)return kAudioHardwareIllegalOperationError;*will=op==kAudioServerPlugInIOOperationWriteMix || (gTapEnabled&&op==kAudioServerPlugInIOOperationReadInput);*inPlace=true;return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 n,const AudioServerPlugInIOCycleInfo *i){(void)d;(void)o;(void)c;(void)op;(void)n;(void)i;return 0;}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 client,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *info,void *main,void *secondary){
    (void)d;(void)client;(void)secondary;
    bool read=op==kAudioServerPlugInIOOperationReadInput && stream==kTapStream && gTapEnabled;
    bool write=op==kAudioServerPlugInIOOperationWriteMix && stream==kOutStream;
    if(o!=kDevice||(!read&&!write)||!info||!main||!frames||frames>8192)return kAudioHardwareIllegalOperationError;
    double t=read?info->mInputTime.mSampleTime:info->mOutputTime.mSampleTime;if(!isfinite(t)||t<0||t>9007199254740991.0||floor(t)!=t)return kAudioHardwareIllegalOperationError;
    if(pthread_mutex_trylock(&gLock))return kAudioHardwareNotRunningError;
    if(!gIoUsers||gFault||gStop){pthread_mutex_unlock(&gLock);return kAudioHardwareNotRunningError;}
    if(read){
        uint64_t missing=frames;
        if(gTapActive)missing=picasso_tap_read(&gTap,(uint64_t)t,frames,main);
        else memset(main,0,(size_t)frames*2*sizeof(Float32));
        ++gTapReads;gTapMissing+=missing;
        // Accepted PCM is a software playback reference, not measured acoustic
        // feedback. Empty/expired frames are silence and remain counted as gaps.
        if(gTapReads==1)routeNote(14,0,5,kTapStream,frames,(unsigned)missing,5);
        pthread_mutex_unlock(&gLock);return 0;
    }
    if(!gStreamActive){pthread_mutex_unlock(&gLock);return 0;}
    int rc=picasso_mix_add(&gMix,gReservedEnd?gReservedEnd:gSent,(uint64_t)t,frames,main,gain());++gCalls;
    if(rc && gCalls<32)note(4,rc,op,frames);pthread_mutex_unlock(&gLock);
    return rc?kAudioHardwareIllegalOperationError:0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 n,const AudioServerPlugInIOCycleInfo *i){return BeginIOOperation(d,o,c,op,n,i);}
static AudioServerPlugInDriverInterface gInterface={NULL,QueryInterface,AddRef,Release,Initialize,CreateDevice,DestroyDevice,AddDeviceClient,RemoveDeviceClient,PerformDeviceConfigurationChange,AbortDeviceConfigurationChange,HasProperty,IsPropertySettable,GetPropertyDataSize,GetPropertyData,SetPropertyData,StartIO,StopIO,GetZeroTimeStamp,WillDoIOOperation,BeginIOOperation,DoIOOperation,EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr=&gInterface;
static HRESULT QueryInterface(void *self,REFIID uuid,LPVOID *out){
    if(!out)return E_POINTER;*out=NULL;CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);HRESULT rc=E_NOINTERFACE;
    if(CFEqual(requested,IUnknownUUID)||CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID)){AddRef(self);*out=&gInterfacePtr;rc=S_OK;}CFRelease(requested);return rc;
}
__attribute__((visibility("default"))) void *PicassoAudio_Create(CFAllocatorRef allocator,CFUUIDRef type){
    (void)allocator;if(!CFEqual(type,kAudioServerPlugInTypeUUID))return NULL;
    pthread_mutex_lock(&gLock);if(!gHostTicksPerFrame){mach_timebase_info_data_t tb;mach_timebase_info(&tb);gHostTicksPerFrame=1e9/kRate*tb.denom/tb.numer;gOrigin=mach_absolute_time();gSeed=1;}
    pthread_mutex_unlock(&gLock);return &gInterfacePtr;
}
