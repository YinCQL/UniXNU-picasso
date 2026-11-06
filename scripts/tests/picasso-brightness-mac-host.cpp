/* Execute the production board-only backlight rule and executable identity checks. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
class OSObject { public: unsigned refs=1; virtual ~OSObject(){}; void release(){if(!--refs)delete this;} };
class IORegistryEntry:public OSObject{};
class IOService:public IORegistryEntry{};
class OSNumber:public OSObject{ public: uint64_t value;unsigned bits=32;explicit OSNumber(uint64_t v):value(v){};uint64_t unsigned64BitValue(){return value;} unsigned numberOfBits(){return bits;} };
class OSDictionary:public OSObject {
public:
    const char *keys[4]={};OSObject *values[4]={};unsigned count=0;
    ~OSDictionary(){for(unsigned i=0;i<count;++i)values[i]->release();}
    bool setObject(const char *key,OSObject *v){assert(count<4);keys[count]=key;values[count++]=v;++v->refs;return true;}
    OSObject *getObject(const char *key){for(unsigned i=0;i<count;++i)if(!strcmp(keys[i],key))return values[i];return nullptr;}
    unsigned getCount(){return count;}
};
#define OSDynamicCast(t,p) dynamic_cast<t *>(p)
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
static IOService board,other;
static bool isService(IORegistryEntry *p){return p==&board;}
static bool (*picasso_brightness_is_service)(IORegistryEntry *)=isService;
static bool grant;
static unsigned macCalls;
static int original,credential=50;
static int kauth_cred_get(){return credential;}
static int proc_selfpid(){return 50;}
class OSString:public OSObject {public: const char *s;explicit OSString(const char *p):s(p){} const char *getCStringNoCopy(){return s;}};
class OSCollectionIterator:public OSObject {
    OSDictionary *dict;unsigned index=0;OSString key{""};
public:
    explicit OSCollectionIterator(OSDictionary *d):dict(d){}
    static OSCollectionIterator *withCollection(OSDictionary *d){return new OSCollectionIterator(d);}
    OSObject *getNextObject(){if(index==dict->count)return nullptr;key.s=dict->keys[index++];return &key;}
};
static bool native_sandbox_grant(IOService *){return grant;}
static int mac_iokit_check_set_properties(int cred,IORegistryEntry *,OSObject *){
    assert(cred==credential);++macCalls;return original;
}
static void IOLog(const char *,...){}
struct Proc{};using proc_t=Proc *;using vnode_t=int *;
static Proc proc;static int vnode,vnodeRefs;
static bool noProc,platform,noVnode,pathError;
static const char *caller,*executable;
static int current_task(){return 1;}
static void *get_bsdtask_info(int){return noProc?nullptr:&proc;}
static int csproc_get_platform_binary(proc_t p){assert(p==&proc);return platform;}
static void proc_name(int,char *out,int size){snprintf(out,size,"%s",caller);}
static vnode_t proc_getexecutablevnode(proc_t){if(noVnode)return nullptr;++vnodeRefs;return &vnode;}
static int vn_getpath(vnode_t v,char *out,int *n){assert(v==&vnode && vnodeRefs==1);snprintf(out,*n,"%s",executable);return pathError;}
static int vnode_put(vnode_t v){assert(v==&vnode && vnodeRefs==1);--vnodeRefs;return 0;}
#include "brightness-mac.inc"
static void number(OSDictionary *d,const char *key,uint64_t value){auto *n=new OSNumber(value);assert(d->setObject(key,n));n->release();}
static OSDictionary *request(unsigned count=3){auto *d=new OSDictionary;number(d,"brightness",2999);if(count>1)number(d,"FadePeriod",0xffff0000);if(count>2)number(d,"backlight-level",2999);return d;}
int main(){
    unsigned cases=0;
    for(unsigned count=1;count<=3;++count)for(unsigned scenario=0;scenario<18;++scenario){
        OSObject *o=request(count),*before=o;IORegistryEntry *target=&board;
        auto *d=static_cast<OSDictionary *>(o);
        grant=false;macCalls=0;original=1;picasso_brightness_is_service=isService;
        noProc=noVnode=pathError=false;platform=true;caller="backboardd";executable="/usr/libexec/backboardd";
        switch(scenario){
        case 1:target=&other;break;case 2:noProc=true;break;case 3:platform=false;break;
        case 4:caller="SpringBoard";break;case 5:noVnode=true;break;case 6:pathError=true;break;
        case 7:executable="/private/var/backboardd";break;
        case 8:number(d,"unexpected",1);break;
        case 9:static_cast<OSNumber *>(d->getObject("brightness"))->value=4096;break;
        case 10:static_cast<OSNumber *>(d->getObject("brightness"))->bits=31;break;
        case 11:original=0;break;case 12:grant=true;break;
        case 13:picasso_brightness_is_service=nullptr;break;
        case 14:static_cast<OSNumber *>(d->getObject("brightness"))->bits=63;break;
        case 15:static_cast<OSNumber *>(d->getObject("brightness"))->value=UINT64_MAX;break;
        case 16:if(count>1)static_cast<OSNumber *>(d->getObject("FadePeriod"))->value=UINT64_MAX;break;
        case 17:if(count>2)static_cast<OSNumber *>(d->getObject("backlight-level"))->bits=31;break;
        }
        int result=picasso_iokit_set_properties_check(target,&o);
        bool accepted=scenario==0 || scenario==11 || scenario==12 || (scenario==16 && count==1) || (scenario==17 && count<3);
        assert(result==(accepted?0:1));assert(o==before && macCalls==1 && !vnodeRefs);
        o->release();++cases;
    }
    OSObject *o=request();grant=true;macCalls=0;
    assert(!picasso_iokit_set_properties_check(&other,&o) && !macCalls);o->release();++cases;
    printf("PASS brightness MAC: %u production identity/type/scope/denial cases\n",cases);
}
