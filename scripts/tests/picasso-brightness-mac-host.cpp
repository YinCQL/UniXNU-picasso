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
static bool grant=false;
static unsigned macCalls;static int original=1,subsetVerdict=0,credential=0;
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
static int mac_iokit_check_set_properties(int cred,IORegistryEntry *,OSObject *o){
    assert(cred==credential);
    ++macCalls;auto *d=dynamic_cast<OSDictionary *>(o);
    return d && d->getCount()==1 && d->getObject("brightness")?subsetVerdict:original;
}
static void IOLog(const char *,...){}
#include "brightness-mac.inc"
static void number(OSDictionary *d,const char *key,uint64_t value){auto *n=new OSNumber(value);assert(d->setObject(key,n));n->release();}
static OSDictionary *request(){auto *d=new OSDictionary;number(d,"brightness",2048);number(d,"FadePeriod",65536);number(d,"backlight-level",0);return d;}
int main(){
    unsigned cases=0;
    // The caller identity is policy-owned. Each credential must pass the
    // actual brightness-only policy; no name, platform flag or path heuristic.
    for(int cred=0;cred<3;++cred) for(unsigned scenario=0;scenario<12;++scenario){
        credential=cred;OSObject *o=request(),*before=o;IORegistryEntry *target=&board;
        grant=false;macCalls=0;original=1;subsetVerdict=cred==2?13:0;picasso_brightness_is_service=isService;
        switch(scenario){
        case 1:target=&other;break;case 2:subsetVerdict=13;break;
        case 3:allocationFailure=true;break;case 4:propertyFailure=true;break;
        case 5:number(static_cast<OSDictionary *>(o),"unexpected",1);break;
        case 6:static_cast<OSNumber *>(static_cast<OSDictionary *>(o)->getObject("brightness"))->value=4096;break;
        case 7:static_cast<OSNumber *>(static_cast<OSDictionary *>(o)->getObject("FadePeriod"))->value=UINT64_MAX;break;
        case 8:original=0;break;case 9:grant=true;break;
        case 10:picasso_brightness_is_service=nullptr;break;
        case 11:static_cast<OSNumber *>(static_cast<OSDictionary *>(o)->getObject("backlight-level"))->value=4096;break;
        }
        int result=picasso_iokit_set_properties_check(target,&o);
        bool projected=cred!=2 && (scenario==0 || scenario==9);
        assert((o!=before)==projected);
        if(projected){assert(!result && macCalls==2);assert(static_cast<OSDictionary *>(o)->getCount()==1);}
        else {assert(result==(scenario==8?0:scenario==2 || (cred==2 && (scenario==0 || scenario==9))?13:1));}
        o->release();allocationFailure=propertyFailure=false;++cases;
    }
    OSObject *o=request();grant=true;macCalls=0;
    assert(!picasso_iokit_set_properties_check(&other,&o) && !macCalls);o->release();++cases;
    printf("PASS brightness MAC: %u production credential/subset/denial cases\n",cases);
}
