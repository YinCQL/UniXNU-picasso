/* Diagnostics must not mutate busy state, recurse indefinitely, or alter open policy. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
class OSObject {public:virtual ~OSObject(){};};
class IORegistryEntry:public OSObject{};
class OSMetaClass {public:const char *getClassName(){return "Test";}};
class IOService:public IORegistryEntry {public:unsigned busy=1;static IOService *getServiceRoot();unsigned getBusyState(){return busy;}const char *getName(){return "Test";}OSMetaClass *getMetaClass(){static OSMetaClass c;return &c;}unsigned long long getState(){return 1;}};
static IOService root,other,nodes[600];
IOService *IOService::getServiceRoot(){return &root;}
static int iterations,releases,logs;static const char *caller="configd";static bool create=true;
static int gIOServicePlane=1;enum {kIORegistryIterateRecursively=1};
class IORegistryIterator {unsigned index=0;public:static IORegistryIterator *iterateOver(IOService *s,int plane,int options){assert(s==&root && plane==1 && options==1);return create?new IORegistryIterator:nullptr;}IORegistryEntry *getNextObject(){++iterations;return index<600?&nodes[index++]:nullptr;}bool isValid(){return true;}void release(){++releases;delete this;}};
#define OSDynamicCast(t,p) dynamic_cast<t *>(p)
#define OSSafeReleaseNULL(p) do {if(p)(p)->release();(p)=nullptr;}while(0)
static int proc_selfpid(){return 34;}
static void proc_name(int,char *out,int n){snprintf(out,n,"%s",caller);}
static void IOLog(const char *,...){++logs;}
#include "network-observe.inc"
int main(){
    caller="Preferences";picasso_network_busy_observe(&root,1);assert(!logs);caller="configd";picasso_network_busy_observe(&other,1);assert(!logs);
    picasso_network_busy_observe(&root,1);assert(iterations==512 && releases==1 && logs==26);
    picasso_network_busy_observe(&root,0);assert(logs==27);
    picasso_network_busy_observe(&root,1);assert(logs==27 && iterations==512 && root.busy==1);
    puts("PASS network observation: scope, two samples, 512-node and 24-log limits, unchanged busy state");
}
