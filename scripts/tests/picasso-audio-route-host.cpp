// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
using proc_t=void *;using user_addr_t=uint64_t;
static int native_gpu_addon_error, native_gpu_addon_failed, native_gpu_addon_files=4;
static int native_gpu_addon_written=4, native_gpu_addon_same;
static unsigned routeMounts,halMounts;
static int routeStage=5,routeError;
static void native_gpu_addon_mount(proc_t,user_addr_t,user_addr_t offset,const char *source,size_t sn,
    const char *point,size_t pn,const char *info,const char *binary,volatile int *stage,volatile int *error){
    assert(offset==0xa00&&sn==strlen(source)+1&&pn==strlen(point)+1);
    assert(!strcmp(source,"/private/var/ngpu/audio-route/VirtualAudio.plugin"));
    assert(!strcmp(point,"/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin"));
    assert(!strcmp(info,"/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/Info.plist"));
    assert(!strcmp(binary,"/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio"));
    ++routeMounts;*stage=routeStage;*error=routeError;
}
static void picasso_audio_addon_mount(proc_t,user_addr_t,volatile int *stage,volatile int *error){
    assert(routeMounts==1&&*stage==5&&!*error);++halMounts;
}
#include "route-helpers.inc"
int main(){
    const char *names[]={"audio/PicassoAudio.driver/Info.plist","audio/PicassoAudio.driver/PicassoAudio",
        "audio-route/VirtualAudio.plugin/Info.plist","audio-route/VirtualAudio.plugin/VirtualAudio"};
    for(unsigned i=0;i<4;++i){
        auto mode=(i&1)?0755U:0644U;
        assert(picasso_audio_addon_entry(4,i,names[i],mode));
        for(unsigned count: {0U,2U,3U,5U,4096U})assert(!picasso_audio_addon_entry(count,i,names[i],mode));
        assert(!picasso_audio_addon_entry(4,i,names[i],mode^1));
        assert(!picasso_audio_addon_entry(4,i,names[(i+1)%4],mode));
        assert(!picasso_audio_addon_entry(4,i,nullptr,mode));
        assert(!picasso_audio_addon_entry(4,i,"audio-route/../VirtualAudio",mode));
    }
    assert(!picasso_audio_addon_entry(4,4,names[0],0644));
    picasso_audio_prepare(nullptr,0);assert(routeMounts==1&&halMounts==1);
    for(int stage=1;stage<=5;++stage)for(int error: {0,5}){
        routeMounts=halMounts=0;routeStage=stage;routeError=error;
        picasso_audio_prepare(nullptr,0);assert(routeMounts==1);assert(halMounts==unsigned(stage==5&&!error));
    }
    for(int failure=0;failure<3;++failure){
        routeMounts=halMounts=0;native_gpu_addon_error=failure==0?5:0;
        native_gpu_addon_failed=failure==1?1:0;native_gpu_addon_files=failure==2?2:4;
        picasso_audio_prepare(nullptr,0);assert(!routeMounts&&!halMounts);
    }
    puts("PASS exact addon whitelist, plugin mount arguments, ordering and failures");
}
