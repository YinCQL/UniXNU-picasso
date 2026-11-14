// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the actual added XNU helpers and HAL wait against narrow host APIs.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdio>
using task_t=void *;using proc_t=void *;
static task_t task=(void *)1,processTask=task;
static proc_t process=(void *)2;
static bool platform=true;static unsigned refs;
static const char *identity="com.apple.mediaserverd";
static task_t current_task(){return task;}
static proc_t proc_self(){if(process)++refs;return process;}
static task_t proc_task(proc_t){return processTask;}
static const char *csproc_get_identity(proc_t){return identity;}
static bool csproc_get_platform_binary(proc_t){return platform;}
static void proc_rele(proc_t){assert(refs);--refs;}
struct Meta {const char *name;const char *getClassName(){return name;}};
struct IOService {Meta meta;Meta *getMetaClass(){return &meta;}};
struct IOUserClient:IOService {IOService *provider;IOService *getProvider(){return provider;}};
#include "access.inc"
using user_addr_t=uint64_t;using vnode_t=void *;
#define NULLVP nullptr
#define EAUTH 80
static vnode_t lower=(void *)3,post=(void *)3;
static int beforeError,afterError,mountStage=5,mountError,vnodePuts,lookups;
static void *vfs_context_current(){return nullptr;}
static int vnode_lookup(const char *,int,vnode_t *v,void *){
    ++lookups;int error=lookups==1?beforeError:afterError;
    *v=error?nullptr:(lookups==1?lower:post);return error;
}
static void vnode_put(vnode_t p){assert(p);++vnodePuts;}
static void native_gpu_addon_mount(proc_t,user_addr_t,user_addr_t,const char *,size_t,const char *,size_t,const char *a,const char *b,volatile int *s,volatile int *e){
    assert(strstr(a,"PicassoAudio.driver/Info.plist"));assert(strstr(b,"PicassoAudio.driver/PicassoAudio"));*s=mountStage;*e=mountError;
}
#include "mount.inc"
using kern_return_t=int;
enum{kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnBusy=3};
#include "PicassoAudioAbi.h"
static int state,methodError;static unsigned polls,sleeps;
static kern_return_t method(unsigned selector,const void *,size_t,void *v,size_t *s){
    assert(selector==PicassoAudioGetStatus);++polls;auto *p=(PicassoAudioStatus *)v;
    *p={};p->version=1;p->state=state==99?(polls<4?PicassoAudioDraining:PicassoAudioIdle):state;*s=sizeof *p;return methodError;
}
static int usleep(unsigned usec){assert(usec==10000);++sleeps;return 0;}
#include "idle.inc"
int main(){
    IOService service{{"PicassoAudio"}},other{{"PicassoAudio"}};IOUserClient client{{{"PicassoAudioUserClient"}},&service};
    assert(picasso_audio_open_grant(&service,&client,task,0));assert(!refs);
    assert(!picasso_audio_open_grant(&service,&client,task,1));assert(!picasso_audio_open_grant(&service,&client,(void *)5,0));
    assert(!picasso_audio_open_grant(nullptr,&client,task,0));assert(!picasso_audio_open_grant(&service,nullptr,task,0));
    client.provider=&other;assert(!picasso_audio_open_grant(&service,&client,task,0));client.provider=&service;
    service.meta.name="IOAudio2Device";assert(!picasso_audio_open_grant(&service,&client,task,0));service.meta.name="PicassoAudio";
    client.meta.name="OtherUserClient";assert(!picasso_audio_open_grant(&service,&client,task,0));client.meta.name="PicassoAudioUserClient";
    platform=false;assert(!picasso_audio_open_grant(&service,&client,task,0));platform=true;
    identity="com.example.mediaserverd";assert(!picasso_audio_open_grant(&service,&client,task,0));identity=nullptr;assert(!picasso_audio_open_grant(&service,&client,task,0));identity="com.apple.mediaserverd";
    processTask=(void *)5;assert(!picasso_audio_open_grant(&service,&client,task,0));processTask=task;
    process=nullptr;assert(!picasso_audio_open_grant(&service,&client,task,0));process=(void *)2;assert(!refs);
    volatile int stage=0,error=0;
    picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==5&&!error&&vnodePuts==2);
    lookups=vnodePuts=0;beforeError=ENOENT;picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==5&&!error&&!vnodePuts&&lookups==1);
    lookups=vnodePuts=0;beforeError=0;afterError=ENOENT;picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==6&&error==ENOENT&&vnodePuts==1);
    lookups=vnodePuts=0;afterError=0;post=(void *)4;picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==6&&error==EAUTH&&vnodePuts==2);
    lookups=vnodePuts=0;beforeError=EACCES;picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==6&&error==EACCES&&!vnodePuts);
    lookups=vnodePuts=0;beforeError=ENOENT;mountStage=4;mountError=ENOENT;picasso_audio_addon_mount(process,0,&stage,&error);assert(stage==4&&error==ENOENT);
    state=PicassoAudioIdle;assert(!waitIdle()&&polls==1&&!sleeps);
    polls=sleeps=0;state=99;assert(!waitIdle()&&polls==4&&sleeps==3);
    polls=sleeps=0;state=PicassoAudioRunning;assert(waitIdle()==kIOReturnBusy&&polls==200&&sleeps==200);
    polls=sleeps=0;state=PicassoAudioFailed;assert(waitIdle()==kIOReturnNotReady&&polls==1&&!sleeps);
    methodError=42;assert(waitIdle()==42);
    std::puts("PASS exact signed audio access, addon visibility and bounded DSP idle wait");
}
