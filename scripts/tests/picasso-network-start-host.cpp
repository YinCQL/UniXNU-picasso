/* Actual stack loader: real OSKext/matching protocol represented by counted calls. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
using kern_return_t=int;
static bool resourcePresent=true,bsdPresent=true,matchPresent=true,stackPresent=true;
static int loadError;static unsigned loads,waits,releases;
class OSDictionary {public:void release(){++releases;}};
static OSDictionary matching;
class IOService {
public:
    static IOService *getResourceService();
    void *getProperty(const char *key){assert(!strcmp(key,"IOBSD"));return bsdPresent?this:nullptr;}
    unsigned getBusyState(){return 0;}
    static OSDictionary *serviceMatching(const char *key){assert(!strcmp(key,"IONetworkStack"));return matchPresent?&matching:nullptr;}
    static IOService *waitForMatchingService(OSDictionary *,uint64_t);
    void release(){++releases;}
};
static IOService resource,stack;
IOService *IOService::getResourceService(){return resourcePresent?&resource:nullptr;}
IOService *IOService::waitForMatchingService(OSDictionary *,uint64_t ns){assert(ns==1000000000ULL);++waits;return stackPresent?&stack:nullptr;}
static kern_return_t OSKextLoadKextWithIdentifier(const char *name){assert(!strcmp(name,"com.apple.iokit.IONetworkingFamily"));++loads;return loadError;}
static void IOLog(const char *,...){}
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
#include "network-start.inc"
int main(){
    for(unsigned i=0;i<6;++i){
        resourcePresent=i!=1;bsdPresent=i!=2;loadError=i==3?5:0;matchPresent=i!=4;stackPresent=i!=5;
        loads=waits=releases=0;picasso_network_stack_start();
        assert(loads==(i!=1 && i!=2));assert(waits==(i==0 || i==5));
        assert(releases==(i==0?2U:i==5?1U:0U));
    }
    puts("PASS original network stack loader: 6 initialization/failure cases");
}
