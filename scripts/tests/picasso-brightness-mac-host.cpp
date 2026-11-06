/* Execute the production policy, including caller checks, with counted MAC verdicts. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
class OSObject { public: unsigned refs=1; virtual ~OSObject(){}; void release(){if(!--refs)delete this;} };
class IORegistryEntry:public OSObject{};
class IOService:public IORegistryEntry{};
class OSNumber:public OSObject{ public: uint64_t value; explicit OSNumber(uint64_t v):value(v){};uint64_t unsigned64BitValue(){return value;} };
static bool allocationFailure, propertyFailure;
class OSDictionary:public OSObject {
public:
    const char *keys[4]={};OSObject *values[4]={};unsigned count=0;
    ~OSDictionary(){for(unsigned i=0;i<count;++i)values[i]->release();}
    static OSDictionary *withCapacity(unsigned){return allocationFailure?nullptr:new OSDictionary;}
    bool setObject(const char *key,OSObject *v){if(propertyFailure)return false;assert(count<4);keys[count]=key;values[count++]=v;++v->refs;return true;}
    OSObject *getObject(const char *key){for(unsigned i=0;i<count;++i)if(!strcmp(keys[i],key))return values[i];return nullptr;}
    unsigned getCount(){return count;}
};
#define OSDynamicCast(t,p) dynamic_cast<t *>(p)
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
static IOService board,other;
static bool isService(IORegistryEntry *p){return p==&board;}
static bool (*picasso_brightness_is_service)(IORegistryEntry *)=isService;
using proc_t=void *;using vnode_t=void *;
static bool process=true,platform=true,vnode=true,pathError=false,grant=false;
static unsigned vnodeGets,vnodePuts,macCalls;static int original=1,subsetVerdict=0;
static const char *processName="backboardd",*executable="/usr/libexec/backboardd";
static void *current_task(){return nullptr;}
static proc_t get_bsdtask_info(void *){return process?&board:nullptr;}
static int kauth_cred_get(){return 0;}
static bool csproc_get_platform_binary(proc_t){return platform;}
static int proc_selfpid(){return 50;}
static void proc_name(int,char *out,int n){snprintf(out,n,"%s",processName);}
static vnode_t proc_getexecutablevnode(proc_t){if(vnode){++vnodeGets;return &board;}return nullptr;}
static int vn_getpath(vnode_t,char *out,int *n){snprintf(out,*n,"%s",executable);return pathError;}
static void vnode_put(vnode_t){++vnodePuts;}
static bool native_sandbox_grant(IOService *){return grant;}
static int mac_iokit_check_set_properties(int,IORegistryEntry *,OSObject *o){
    ++macCalls;auto *d=dynamic_cast<OSDictionary *>(o);
    return d && d->getCount()==1 && d->getObject("brightness")?subsetVerdict:original;
}
static void IOLog(const char *,...){}
#include "brightness-mac.inc"
static void number(OSDictionary *d,const char *key,uint64_t value){auto *n=new OSNumber(value);assert(d->setObject(key,n));n->release();}
static OSDictionary *request(){auto *d=new OSDictionary;number(d,"brightness",2048);number(d,"FadePeriod",65536);number(d,"backlight-level",0);return d;}
int main(){
    unsigned cases=0;
    for(unsigned scenario=0;scenario<18;++scenario){
        OSObject *o=request(),*before=o;IORegistryEntry *target=&board;
        process=platform=vnode=true;pathError=grant=false;vnodeGets=vnodePuts=macCalls=0;original=1;subsetVerdict=0;
        processName="backboardd";executable="/usr/libexec/backboardd";picasso_brightness_is_service=isService;
        switch(scenario){
        case 1:processName="backboardd-copy";break;case 2:platform=false;break;case 3:process=false;break;
        case 4:processName="Settings";break;case 5:executable="/tmp/backboardd";break;
        case 6:vnode=false;break;case 7:pathError=true;break;
        case 8:target=&other;break;case 9:subsetVerdict=13;break;
        case 10:allocationFailure=true;break;case 11:propertyFailure=true;break;
        case 12:number(static_cast<OSDictionary *>(o),"unexpected",1);break;
        case 13:static_cast<OSNumber *>(static_cast<OSDictionary *>(o)->getObject("brightness"))->value=4096;break;
        case 14:static_cast<OSNumber *>(static_cast<OSDictionary *>(o)->getObject("FadePeriod"))->value=UINT64_MAX;break;
        case 15:original=0;break;case 16:grant=true;break;
        case 17:picasso_brightness_is_service=nullptr;break;
        }
        int result=picasso_iokit_set_properties_check(target,&o);
        bool projected=scenario==0 || scenario==16;
        assert((o!=before)==projected);assert(vnodeGets==vnodePuts);
        if(projected){assert(!result && macCalls==2);assert(static_cast<OSDictionary *>(o)->getCount()==1);}
        else {assert(result==(scenario==15?0:scenario==9?13:1));}
        o->release();allocationFailure=propertyFailure=false;++cases;
    }
    // Foreign service grants retain their original behavior, without a new board grant.
    OSObject *o=request();grant=true;macCalls=0;
    assert(!picasso_iokit_set_properties_check(&other,&o) && !macCalls);o->release();++cases;
    printf("PASS brightness MAC: %u production caller/subset/denial cases\n",cases);
}
