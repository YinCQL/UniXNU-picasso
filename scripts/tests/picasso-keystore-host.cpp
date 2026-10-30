// Host API stand-ins; the actual state service is included without edits.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <map>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <intrin.h>
#define __ATOMIC_ACQUIRE 0
#define __ATOMIC_RELEASE 0
#define __ATOMIC_RELAXED 0
#define __atomic_load_n(p,o) ((unsigned)_InterlockedCompareExchange((volatile long *)(p),0,0))
#define __atomic_store_n(p,v,o) ((void)_InterlockedExchange((volatile long *)(p),(long)(v)))
static bool host_compare(unsigned *p,unsigned *expected,unsigned desired) {
    unsigned actual=(unsigned)_InterlockedCompareExchange((volatile long *)p,(long)desired,(long)*expected);
    if(actual==*expected)return true;*expected=actual;return false;
}
#define __atomic_compare_exchange_n(p,e,d,w,s,f) host_compare(p,e,d)
using UInt32=uint32_t;
using task_t=void *;
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
    kIOReturnNotOpen=-3,kIOReturnUnsupported=-4,kIOReturnError=-5,
    kIOReturnNoMemory=-6,kIOReturnNoSpace=-7,kIOReturnUnderrun=-8};
struct OSDictionary {};
struct IOExternalMethodDispatch {};
struct IOMemoryDescriptor {
    uint8_t bytes[256]={}; size_t length=256, written=0; bool shortWrite=false;
    size_t getLength()const{return length;}
    size_t writeBytes(size_t offset,const void *data,size_t n) {
        assert(offset+n<=length);written=shortWrite?n-1:n;memcpy(bytes+offset,data,written);return written;
    }
};
struct IOExternalMethodArguments {
    const uint64_t *scalarInput=nullptr;uint32_t scalarInputCount=0;
    uint64_t *scalarOutput=nullptr;uint32_t scalarOutputCount=0;
    const void *structureInput=nullptr;uint32_t structureInputSize=0;
    void *structureOutput=nullptr;uint32_t structureOutputSize=0;
    IOMemoryDescriptor *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    uint32_t structureOutputDescriptorSize=0;unsigned asyncWakePort=0,asyncReferenceCount=0;
};
static unsigned destroyed,detached,failAt,step;
class OSObject {
public:
    virtual ~OSObject(){++destroyed;}
    void release(){delete this;}
};
class IOService:public OSObject {
public:
    bool inactive=false;unsigned failProperty=0,propertyCount=0,publications=0,stops=0;
    bool isInactive()const{return inactive;}
    virtual bool start(IOService *){return ++step!=failAt;}
    virtual void stop(IOService *){++stops;}
    bool attach(IOService *){return ++step!=failAt;}
    void detach(IOService *){++detached;}
    void setName(const char *){}
    bool setProperty(const char *name,bool value){
        if(!strcmp(name,"KeyOperationsAvailable"))assert(!value);
        return ++propertyCount!=failProperty;
    }
    void registerService(){++publications;}
    virtual IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,class IOUserClient **){return 0;}
    virtual IOReturn newUserClient(task_t,void *,UInt32,class IOUserClient **){return 0;}
};
class IOUserClient:public IOService {
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *){return ++step!=failAt;}
    virtual IOReturn clientClose(){return 0;}
    void terminate(){inactive=true;}
    virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *){return 0;}
};
#define OSDeclareDefaultStructors(n)
#define OSDefineMetaClassAndStructors(n,b)
static bool enabled;
extern "C" bool picasso_aks_state_enabled(){return enabled;}
static const char *processName="SpringBoard";
static int proc_selfpid(){return 27;}
static void proc_name(int,char *name,int size){snprintf(name,size,"%s",processName);}
static std::string diagnosticOutput;
static void IOLog(const char *fmt,...) {
    char line[256];va_list args;va_start(args,fmt);int n=vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    assert(n>0 && n<(int)sizeof(line));diagnosticOutput+=line;
}
#include "observed.inc"
#include "NativeKeyStoreEvents.cpp"

static std::map<std::string,unsigned> decode(const uint8_t *p,size_t n) {
    assert(n>=2 && p[0]==0x31 && p[1]+2u==n);
    std::map<std::string,unsigned> result;
    size_t i=2;
    while(i<n) {
        assert(i+4<n && p[i++]==0x30);size_t len=p[i++],end=i+len;
        assert(end<=n && p[i++]==0x0c);size_t k=p[i++];assert(i+k+3==end);
        std::string key((const char *)p+i,k);i+=k;
        assert(p[i++]==2 && p[i++]==1);assert(!result.count(key));result[key]=p[i++];
    }
    assert(result.size()==10);return result;
}
static IOReturn call(IOUserClient *client,unsigned selector,IOExternalMethodArguments &a) {
    return client->externalMethod(selector,&a,nullptr,nullptr,nullptr);
}
int main() {
    AppleKeyStore service;IOService provider;IOUserClient *client=(IOUserClient *)1;
    assert(!service.start(&provider));
    assert(service.newUserClient((void *)1,nullptr,0,&client)==kIOReturnNotReady && !client);
    enabled=true;
    assert(service.newUserClient(nullptr,nullptr,0,&client)==kIOReturnBadArgument);
    assert(service.newUserClient((void *)1,nullptr,1,&client)==kIOReturnUnsupported && !client);
    for(unsigned p=1;p<=4;++p) {
        AppleKeyStore failing;failing.failProperty=p;
        assert(!failing.start(&provider) && !failing.publications && failing.stops==1);
    }
    assert(service.start(&provider) && service.publications==1);
    for(unsigned f=1;f<=3;++f) {
        step=0;failAt=f;unsigned before=destroyed,detachBefore=detached;
        assert(service.newUserClient((void *)1,nullptr,0,&client)==kIOReturnError && !client);
        assert(destroyed==before+1 && detached==detachBefore+(f==3));
    }
    failAt=0;step=0;
    assert(service.newUserClient((void *)1,nullptr,0,&client)==0 && client);
    uint64_t context=0,scalar=0xdeadbeef;
    IOExternalMethodArguments a;
    assert(call(client,7,a)==kIOReturnNotOpen);
    a.scalarInputCount=1;assert(call(client,0,a)==kIOReturnBadArgument);
    a.scalarInputCount=0;assert(call(client,0,a)==0);
    assert(call(client,0,a)==0);
    a.scalarInput=&context;a.scalarInputCount=1;a.scalarOutput=&scalar;a.scalarOutputCount=1;
    assert(call(client,7,a)==0 && scalar==4);
    context=1;scalar=0xdeadbeef;
    assert(call(client,7,a)==kIOReturnUnsupported && scalar==0xdeadbeef);
    context=UINT64_MAX;assert(call(client,17,a)==kIOReturnUnsupported);
    context=0;a.scalarInput=nullptr;assert(call(client,7,a)==kIOReturnBadArgument);
    a.scalarInput=&context;
    uint8_t out[160];memset(out,0xa5,sizeof(out));
    a.scalarOutputCount=0;a.structureOutput=out;a.structureOutputSize=sizeof(out);
    assert(call(client,17,a)==0);
    auto state=decode(out,a.structureOutputSize);
    assert(state.at("ss")==4 && state.at("sls")==3 && state.at("bh")==0);
    size_t length=a.structureOutputSize;
    assert(out[length]==0xa5);
    for(size_t size=0;size<length;++size) {
        a.structureOutputSize=(uint32_t)size;memset(out,0xa5,sizeof(out));
        assert(call(client,35,a)==kIOReturnNoSpace && a.structureOutputSize==size);
        for(auto byte:out)assert(byte==0xa5);
    }
    a.structureOutput=nullptr;a.structureOutputSize=0;
    IOMemoryDescriptor memory;a.structureOutputDescriptor=&memory;a.structureOutputDescriptorSize=(uint32_t)memory.length;
    assert(call(client,35,a)==0 && memory.written==length && decode(memory.bytes,length)==state);
    memory.written=0;a.structureOutputDescriptorSize=(uint32_t)length-1;
    assert(call(client,17,a)==kIOReturnNoSpace && !memory.written);
    a.structureOutputDescriptorSize=256;memory.length=length-1;
    assert(call(client,17,a)==kIOReturnNoSpace && !memory.written);
    memory.length=256;memory.shortWrite=true;
    assert(call(client,17,a)==kIOReturnUnderrun && a.structureOutputDescriptorSize==256);
    a.structureOutputDescriptor=nullptr;
    processName="keybagd";
    for(unsigned i=0;i<10;++i)picasso_aks_observed(14,kIOReturnUnsupported,true,0xdeadbeef);
    processName="SpringBoard";
    // All bag, unlock, crypto, assertion, notification and unknown operations fail.
    for(unsigned selector=1;selector<300;++selector) {
        if(selector==7 || selector==17 || selector==35)continue;
        scalar=0xdeadbeef;
        assert(call(client,selector,a)==kIOReturnUnsupported && scalar==0xdeadbeef);
    }
    a.scalarOutputCount=1;a.structureOutputSize=0;
    a.structureInputSize=1;assert(call(client,7,a)==kIOReturnBadArgument);a.structureInputSize=0;
    a.structureInputDescriptor=&memory;assert(call(client,7,a)==kIOReturnBadArgument);a.structureInputDescriptor=nullptr;
    a.asyncWakePort=1;assert(call(client,7,a)==kIOReturnBadArgument);a.asyncWakePort=0;
    a.asyncReferenceCount=1;assert(call(client,7,a)==kIOReturnBadArgument);a.asyncReferenceCount=0;
    enabled=false;assert(call(client,7,a)==kIOReturnNotReady);enabled=true;
    assert(client->clientClose()==0 && call(client,7,a)==kIOReturnNotReady);
    client->release();
    assert(diagnosticOutput.find("name=SpringBoard selector=7 sample=1 result=0 state_context=0x0")!=std::string::npos);
    assert(diagnosticOutput.find("state_context=0x1")!=std::string::npos);
    assert(diagnosticOutput.find("name=keybagd selector=14 sample=2")!=std::string::npos);
    assert(diagnosticOutput.find("name=keybagd selector=14 sample=3")==std::string::npos);
    assert(diagnosticOutput.find("deadbeef")==std::string::npos);
    assert(diagnosticOutput.find("sample=3")==std::string::npos);
    processName="securityd";picasso_aks_observed(0,kIOReturnSuccess,false,0);
    assert(diagnosticOutput.find("name=securityd selector=0 sample=1 result=0")!=std::string::npos);
    puts("PASS state ABI, no key operations, lifecycle failures, input/output bounds and short writes");
}
