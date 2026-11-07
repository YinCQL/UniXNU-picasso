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
static bool gRoutePortLayout;
static atomic_uint gRoutePortSamples;
static uint64_t (*gRouteProduct)(void);
static atomic_bool gRouteSampleProduct;
static OSStatus (*gRouteGet)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *,void *);
static OSStatus (*gRouteSize)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32 *);
static Boolean (*gRouteHas)(AudioObjectID,const AudioObjectPropertyAddress *);
static OSStatus (*gRouteSet)(AudioObjectID,const AudioObjectPropertyAddress *,UInt32,const void *,UInt32,const void *);
static void (*gRouteThrow)(void *,void *,void (*)(void *));
static void routeNote(unsigned kind,OSStatus result,unsigned selector,unsigned object,unsigned scope,unsigned element,unsigned op){
    // Queue only; do not submit IOKit notes or wait behind HAL locks here.
    if(pthread_mutex_trylock(&gRouteLock))return;
    PicassoAudioClientNote n={1,kind,result,selector,object,0,scope,element,op};
    for(unsigned i=0;i<gRouteCount;++i)if(!memcmp(&gRouteNotes[i],&n,sizeof n)){
        pthread_mutex_unlock(&gRouteLock);return;
    }
    if(gRouteCount<128)gRouteNotes[gRouteCount++]=n;
    pthread_mutex_unlock(&gRouteLock);
}
static void flushRouteNotes(void){
    // Read the original MobileGestalt result on the non-realtime watch thread,
    // outside HAL locks. This also covers VA caching it before hook installation.
    if(atomic_exchange_explicit(&gRouteSampleProduct,false,memory_order_acquire)){
        uint64_t product=gRouteProduct();
        routeNote(14,0,3,(unsigned)product,(unsigned)(product>>32),0,5);
    }
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
    if(r || (a && (a->mSelector==kAudioDevicePropertyTransportType || a->mSelector=='slay' || a->mSelector=='taps' || a->mSelector=='vdb#')))
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
// A failed route currently gives only "nort". Capture the port registry at
// that failure without taking foreign locks. The bounded snapshot is only an
// observation, not an atomic view or a route override. Never invoke foreign
// C++ methods, dereference unchecked heap pointers, or change routing state.
// Layout is qualified separately against the exact 20H392 image below.
static bool routeRead(uintptr_t address,void *data,size_t bytes){
    if(address<4096 || address>UINTPTR_MAX-bytes)return false;
    vm_size_t copied=0;
    return !vm_read_overwrite(mach_task_self(),(vm_address_t)address,(vm_size_t)bytes,(vm_address_t)data,&copied) && copied==bytes;
}
static void routePorts(void){
    if(!gRoutePortLayout)return;
    unsigned sample=atomic_load_explicit(&gRoutePortSamples,memory_order_relaxed);
    do {if(sample>=3)return;} while(!atomic_compare_exchange_weak_explicit(&gRoutePortSamples,&sample,sample+1,memory_order_relaxed,memory_order_relaxed));
    uintptr_t manager=0,link=0,head=0;unsigned visited=0;uint8_t tether=0;
    if(!routeRead(gRouteBase+0x63e9d0,&manager,sizeof manager) || !manager || manager>UINTPTR_MAX-0x60){routeNote(14,-1,6,0,0,0,6);return;}
    head=manager+0x58;
    if(!routeRead(head,&link,sizeof link)){routeNote(14,-2,6,0,0,0,6);return;}
    uintptr_t seen[16];
    while(link!=head && visited<16){
        bool duplicate=false;for(unsigned i=0;i<visited;++i)if(seen[i]==link)duplicate=true;
        if(duplicate){routeNote(14,-3,6,visited,0,0,6);return;}
        seen[visited++]=link;
        uintptr_t node[3];uint8_t port[0xb0];
        if(!routeRead(link,node,sizeof node) || !routeRead(node[2],port,sizeof port)){routeNote(14,-4,6,visited,0,0,6);return;}
        uintptr_t vtable,owner;UInt32 type,portClass;uint16_t flags;
        memcpy(&vtable,port,sizeof vtable);memcpy(&type,port+0x38,sizeof type);memcpy(&portClass,port+0x3c,sizeof portClass);
        memcpy(&flags,port+0x60,sizeof flags);memcpy(&owner,port+0x68,sizeof owner);
        if(vtable<gRouteBase+0x60d300 || vtable>=gRouteBase+0x62d0b0){routeNote(14,-5,6,type,0,0,6);return;}
        // Only Speaker's verified derived class owns this connection byte.
        // Other port classes have different tails; report unknown, never infer.
        uint8_t connected=0;UInt32 connection=UINT32_MAX;
        if(vtable==gRouteBase+0x629848 && node[2]<=UINTPTR_MAX-0x1b0 &&
           routeRead(node[2]+0x1b0,&connected,sizeof connected))connection=connected;
        routeNote(14,0,7,type,flags,connection,6);
        routeNote(14,0,10,type,portClass,0,6);
        routeNote(14,0,8,type,(unsigned)(vtable-gRouteBase),owner!=0,6);
        link=node[0];
    }
    bool readTether=routeRead(gRouteBase+0x63f538,&tether,sizeof tether);
    routeNote(14,link==head?0:-6,6,visited,readTether?tether:UINT32_MAX,0,6);
}
static bool routePortLayoutMatches(const struct mach_header_64 *h){
    // __DATA has a zero-fill registry singleton and mode flag, not file bytes.
    const uint8_t *p=(const uint8_t *)(h+1);bool data=false;
    for(unsigned i=0;i<h->ncmds;++i){
        const struct load_command *lc=(const void *)p;
        if(lc->cmd==LC_SEGMENT_64 && lc->cmdsize>=sizeof(struct segment_command_64)){
            const struct segment_command_64 *s=(const void *)p;
            if(!strncmp(s->segname,"__DATA",16)&&s->vmaddr==0x634000&&s->vmsize==0x30000&&s->filesize==0x4000)data=true;
        }
        p+=lc->cmdsize;
    }
    if(!data)return false;
    static const struct {uintptr_t offset;UInt32 word;} words[]={
        {0x120d0,0x90003168},{0x120d4,0xf944e900}, // registry singleton
        {0xa6bec,0xf9400288},{0xa6bf0,0xf9400918}, // list prev / node port
        {0x3e664,0xb9403a6a},{0x1b70e4,0x3906c268} // type / connected
    };
    for(unsigned i=0;i<sizeof words/sizeof words[0];++i){
        UInt32 word;if(!routeRead((uintptr_t)h+words[i].offset,&word,sizeof word)||word!=words[i].word)return false;
    }
    return true;
}
static void routeThrow(void *thrown,void *type,void (*destroy)(void *)){
    uintptr_t at=(uintptr_t)__builtin_return_address(0);
    // The verified arm64 20H392 CAException is 16 bytes: vtable at 0,
    // OSStatus at 8. Other exception layouts are not interpreted.
    const char *name=type?((const char *const *)type)[1]:NULL;
    unsigned category=name&&!strcmp(name,"11CAException")?1:name&&!strcmp(name,"St11logic_error")?2:
        name&&!strcmp(name,"St13runtime_error")?3:name&&!strcmp(name,"St16invalid_argument")?4:0;
    UInt32 first=0;if(thrown&&category==1)memcpy(&first,(const uint8_t *)thrown+8,sizeof first);
    routeNote(13,(OSStatus)first,at>=gRouteBase && at-gRouteBase<0x60c000?(unsigned)(at-gRouteBase):0,category,0,0,4);
    if(category==1 && first==0x6e6f7274)routePorts();
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
    gRouteProduct=(void *)dlsym(RTLD_DEFAULT,"MGGetProductType");
    gRouteGet=(void *)resolved[0];gRouteSize=(void *)resolved[1];gRouteHas=(void *)resolved[2];
    gRouteSet=(void *)resolved[3];gRouteThrow=(void *)resolved[4];gRouteBase=(uintptr_t)h;
    gRoutePortLayout=routePortLayoutMatches(h);routeNote(14,gRoutePortLayout?0:-1,9,0,0,0,6);
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
    if(gRouteProduct)atomic_store_explicit(&gRouteSampleProduct,true,memory_order_release);
}
#if !__has_feature(ptrauth_calls)
__attribute__((constructor)) static void watchRouteImage(void){_dyld_register_func_for_add_image(routeImageAdded);}
#endif
#endif
