// SPDX-License-Identifier: BSD-2-Clause-Patent
// Import tracing adapted from UniXNU-sheng 6e892fefaa67cc1f41b53f79766704d92d92f4d6.
// Only iPhone 20H392 VirtualAudio, UUID 3196333F-5A48-373B-B427-0146DC1177FB.
// Its non-lazy GOT is checked by UUID, section bounds and resolved symbols
// before COW replacement. Return values and exceptions are always forwarded.
#ifndef PICASSO_AUDIO_ROUTE_TRACE_H
#define PICASSO_AUDIO_ROUTE_TRACE_H
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <errno.h>

static pthread_mutex_t gRouteLock=PTHREAD_MUTEX_INITIALIZER;
static PicassoAudioClientNote gRouteNotes[128];
static unsigned gRouteCount,gRouteFlushed;
static uintptr_t gRouteBase;
static OSStatus (*gRouteGet)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *,void *);
static OSStatus (*gRouteSize)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *);
static Boolean (*gRouteHas)(AudioObjectID,const AudioObjectPropertyAddress *);
static OSStatus (*gRouteSet)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32,const void *);
static void (*gRouteThrow)(void *,void *,void (*)(void *));
static void routeNote(unsigned kind,OSStatus result,unsigned selector,unsigned object,unsigned scope,unsigned element,unsigned op){
    // Never call the kernel or wait behind HAL locks in a foreign callback.
    if(pthread_mutex_trylock(&gRouteLock))return;
    PicassoAudioClientNote n={1,kind,result,selector,object,0,scope,element,op};
    for(unsigned i=0;i<gRouteCount;++i)if(!memcmp(&gRouteNotes[i],&n,sizeof n)){
        pthread_mutex_unlock(&gRouteLock);return;
    }
    if(gRouteCount<128)gRouteNotes[gRouteCount++]=n;
    pthread_mutex_unlock(&gRouteLock);
}
static void flushRouteNotes(void){
    pthread_mutex_lock(&gLock);
    if(gConnect){
        for(unsigned i=0;i<16;++i){
            pthread_mutex_lock(&gRouteLock);
            if(gRouteFlushed==gRouteCount){pthread_mutex_unlock(&gRouteLock);break;}
            PicassoAudioClientNote n=gRouteNotes[gRouteFlushed++];
            pthread_mutex_unlock(&gRouteLock);
            (void)method(PicassoAudioNote,&n,sizeof n,NULL,NULL);
        }
    }
    pthread_mutex_unlock(&gLock);
}
static void routeProperty(unsigned kind,AudioObjectID o,const AudioObjectPropertyAddress *a,OSStatus r,unsigned op){
    if(a)routeNote(kind,r,a->mSelector,o,a->mScope,a->mElement,op);
}
static OSStatus routeGet(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *size,void *data){
    OSStatus r=gRouteGet(o,a,qs,q,size,data);
    // Transport and stream layout also show whether route creation progressed.
    if(r || (a && (a->mSelector==kAudioDevicePropertyTransportType || a->mSelector=='slay')))
        routeProperty(9,o,a,r,0);
    return r;
}
static OSStatus routeSize(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *size){
    OSStatus r=gRouteSize(o,a,qs,q,size);if(r)routeProperty(10,o,a,r,1);return r;
}
static Boolean routeHas(AudioObjectID o,const AudioObjectPropertyAddress *a){
    Boolean r=gRouteHas(o,a);if(!r)routeProperty(11,o,a,0,2);return r;
}
static OSStatus routeSet(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,const void *data){
    OSStatus r=gRouteSet(o,a,qs,q,size,data);routeProperty(12,o,a,r,3);return r;
}
static void routeThrow(void *thrown,void *type,void (*destroy)(void *)){
    uintptr_t at=(uintptr_t)__builtin_return_address(0);
    // Only CAException starts with an OSStatus. A standard exception starts
    // with a vtable pointer, which is neither an error code nor useful telemetry.
    const char *name=type?((const char *const *)type)[1]:NULL;
    unsigned category=name&&!strcmp(name,"11CAException")?1:name&&!strcmp(name,"St11logic_error")?2:
        name&&!strcmp(name,"St13runtime_error")?3:name&&!strcmp(name,"St16invalid_argument")?4:0;
    UInt32 first=0;if(thrown&&category==1)memcpy(&first,thrown,sizeof first);
    routeNote(13,(OSStatus)first,at>=gRouteBase && at-gRouteBase<0x60c000?(unsigned)(at-gRouteBase):0,category,0,0,4);
    __attribute__((musttail)) return gRouteThrow(thrown,type,destroy);
}
static bool routeImageMatches(const struct mach_header_64 *h){
    static const uint8_t uuid[16]={0x31,0x96,0x33,0x3f,0x5a,0x48,0x37,0x3b,0xb4,0x27,0x01,0x46,0xdc,0x11,0x77,0xfb};
    if(h->magic!=MH_MAGIC_64 || h->cputype!=CPU_TYPE_ARM64 || h->cpusubtype!=CPU_SUBTYPE_ARM64_ALL ||
       h->filetype!=MH_DYLIB || h->ncmds!=36 || h->sizeofcmds!=4040)return false;
    const uint8_t *p=(const uint8_t *)(h+1),*end=p+h->sizeofcmds;bool id=false,got=false;
    for(unsigned i=0;i<h->ncmds;++i){
        if((size_t)(end-p)<sizeof(struct load_command))return false;
        const struct load_command *lc=(const void *)p;
        if(lc->cmdsize<sizeof *lc || lc->cmdsize>(size_t)(end-p))return false;
        if(lc->cmd==LC_UUID && lc->cmdsize==sizeof(struct uuid_command))
            id=!memcmp(((const struct uuid_command *)lc)->uuid,uuid,sizeof uuid);
        if(lc->cmd==LC_SEGMENT_64 && lc->cmdsize>=sizeof(struct segment_command_64)){
            const struct segment_command_64 *s=(const void *)lc;
            if(s->nsects>(lc->cmdsize-sizeof *s)/sizeof(struct section_64))return false;
            const struct section_64 *sect=(const void *)(s+1);
            for(unsigned j=0;j<s->nsects;++j)if(!strncmp(s->segname,"__DATA_CONST",16) &&
                !strncmp(sect[j].sectname,"__got",16) && sect[j].addr==0x60c000 && sect[j].size==0x1300 &&
                (sect[j].flags&SECTION_TYPE)==S_NON_LAZY_SYMBOL_POINTERS && s->vmaddr==0x60c000 && s->vmsize==0x28000)got=true;
        }
        p+=lc->cmdsize;
    }
    return p==end && id && got;
}
static void routeImageAdded(const struct mach_header *header,intptr_t slide){
    Dl_info info;
    if(!dladdr(header,&info) || !info.dli_fname ||
       strcmp(info.dli_fname,"/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio"))return;
    const struct mach_header_64 *h=(const void *)header;
    if(!routeImageMatches(h) || (uintptr_t)slide!=(uintptr_t)h){routeNote(14,-1,0,0,0,0,0);return;}
    static const uintptr_t offsets[5]={0x60c100,0x60c108,0x60c110,0x60c138,0x60cc00};
    static const char *const names[5]={"AudioObjectGetPropertyData","AudioObjectGetPropertyDataSize","AudioObjectHasProperty","AudioObjectSetPropertyData","__cxa_throw"};
    void *hooks[5]={(void *)routeGet,(void *)routeSize,(void *)routeHas,(void *)routeSet,(void *)routeThrow};
    void *resolved[5];
    for(unsigned i=0;i<5;++i){
        resolved[i]=dlsym(RTLD_DEFAULT,names[i]);
        if(!resolved[i] || *(void **)((uintptr_t)h+offsets[i])!=resolved[i]){routeNote(14,-2,i,0,0,0,0);return;}
    }
    gRouteGet=(void *)resolved[0];gRouteSize=(void *)resolved[1];gRouteHas=(void *)resolved[2];
    gRouteSet=(void *)resolved[3];gRouteThrow=(void *)resolved[4];gRouteBase=(uintptr_t)h;
    long pagesize=getpagesize();if(pagesize!=4096 && pagesize!=16384){routeNote(14,-3,0,0,0,0,0);return;}
    uintptr_t page=((uintptr_t)h+offsets[0])&~((uintptr_t)pagesize-1);
    if((((uintptr_t)h+offsets[4])&~((uintptr_t)pagesize-1))!=page){routeNote(14,-4,0,0,0,0,0);return;}
    vm_size_t regionSize=0;vm_address_t region=page;vm_region_basic_info_data_64_t regionInfo;
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
    kern_return_t rc=vm_region_64(mach_task_self(),&region,&regionSize,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&regionInfo,&count,&object);
    if(object)mach_port_deallocate(mach_task_self(),object);
    if(rc || region>page || regionSize<(page-region)+(uintptr_t)pagesize || !(regionInfo.protection&VM_PROT_READ) || (regionInfo.protection&VM_PROT_EXECUTE)){
        routeNote(14,rc?rc:-5,0,0,0,0,0);return;
    }
    rc=vm_protect(mach_task_self(),page,(vm_size_t)pagesize,false,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if(rc){routeNote(14,rc,1,0,0,0,0);return;}
    for(unsigned i=0;i<5;++i)__atomic_store_n((void **)((uintptr_t)h+offsets[i]),hooks[i],__ATOMIC_RELEASE);
    rc=vm_protect(mach_task_self(),page,(vm_size_t)pagesize,false,regionInfo.protection);
    routeNote(14,rc,2,5,0,0,0);
}
#if !__has_feature(ptrauth_calls)
__attribute__((constructor)) static void watchRouteImage(void){_dyld_register_func_for_add_image(routeImageAdded);}
#endif
#endif
