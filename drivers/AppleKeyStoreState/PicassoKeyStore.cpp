// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publish state only when the boot actually selected unprotected ephemeral Data.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern "C" {
extern kmod_info_t g_kernel_kmod_info;
int proc_selfpid(void);
void proc_name(int pid, char *name, int size);
void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
IOService *native_keyevents_allocate(void);
}
static bool enabled;
static IOService *stateService;
extern "C" bool picasso_aks_state_enabled(void) {return enabled;}
extern "C" bool picasso_aks_ready(void) {return enabled && stateService != nullptr;}

extern "C" void picasso_aks_observed(unsigned selector,IOReturn result,
    unsigned scalar_input_count,unsigned scalar_output_count,
    unsigned structure_input_size,unsigned structure_output_size,
    uint64_t context) {
    // Four reports per known selector and result category, across all clients.
    // Unknown selector values share a single bucket. Never log key material.
    static unsigned counts[258][4], total;
    unsigned *counter=&counts[selector<=256?selector:257][result==kIOReturnSuccess?0:1];
    unsigned count=__atomic_load_n(counter,__ATOMIC_RELAXED);
    while(count<4) {
        if(__atomic_compare_exchange_n(counter,&count,count+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
            unsigned used=__atomic_load_n(&total,__ATOMIC_RELAXED);
            for(;;) {
                if(used>=96)return;
                if(__atomic_compare_exchange_n(&total,&used,used+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))break;
            }
            int pid=proc_selfpid();
            char name[32]={};
            proc_name(pid,name,sizeof(name));
            printf("PICASSO_AKS pid=%d name=%s selector=%u sample=%u result=%x "
                "in=%u out=%u sin=%u sout=%u ctx=%llx\n",
                pid,name[0]?name:"?",selector,count+1,result,
                scalar_input_count,scalar_output_count,
                structure_input_size,structure_output_size,
                (unsigned long long)context);
            break;
        }
    }
}

extern "C" void picasso_keystore_probe(void) {
    static bool attempted;
    if(attempted)return;
    attempted=true;
    unsigned requested=0;PE_parse_boot_argn("picasso-aks-state",&requested,sizeof(requested));
    if(requested!=1)return;
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *data=chosen?OSDynamicCast(OSData,chosen->getProperty("ephemeral-storage")):nullptr;
    uint32_t ephemeral=0;
    if(data && data->getLength()==sizeof(ephemeral))memcpy(&ephemeral,data->getBytesNoCopy(),sizeof(ephemeral));
    OSSafeReleaseNULL(chosen);
    const OSSymbol *name=OSSymbol::withCString("AppleKeyStore");
    bool absent=name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    IOService *provider=IOService::getResourceService();
    if(ephemeral!=1 || !absent || !provider) {
        printf("PICASSO_AKS publish refused ephemeral=%u class_absent=%u provider=%u\n",ephemeral,absent,provider!=nullptr);return;
    }
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){printf("PICASSO_AKS preModLoad failed\n");return;}
    native_keyevents_static_init();
    OSReturn status=OSMetaClass::postModLoad(handle);
    if(status){printf("PICASSO_AKS metaclass result=%x\n",status);return;}
    IOService *service=native_keyevents_allocate();
    bool initialized=service && service->init();
    bool attached=initialized && service->attach(provider);
    // Set before publication so synchronous match/open callbacks see the policy.
    enabled=attached;
    bool started=attached && service->start(provider);
    if(!started) {
        enabled=false;
        if(attached)service->detach(provider);
        OSSafeReleaseNULL(service);
    } else stateService=service;
    printf("PICASSO_AKS started=%u ephemeral=%u state_only=1 key_operations=0\n",started,ephemeral);
}
