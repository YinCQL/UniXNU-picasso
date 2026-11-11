// SPDX-License-Identifier: BSD-2-Clause-Patent
// GNU Objective-C dispatch exercises the actual wrapper; SDK builds qualify ABI.
#include <objc/runtime.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long NSUInteger;
__attribute__((objc_root_class))
@interface NSDictionary { Class isa; }
+ (id)dictionaryWithObjects:(const id *)objects forKeys:(const id *)keys count:(NSUInteger)count;
@end
@implementation NSDictionary
+ (id)dictionaryWithObjects:(const id *)objects forKeys:(const id *)keys count:(NSUInteger)count
{
    (void)objects; (void)keys; assert(!count);
    static id empty; if(!empty) empty=class_createInstance(self,0); return empty;
}
@end
static unsigned notes, calls, installs, unavailable;
static int qualified;
static id status;
static void picasso_input_note(const char *reason){
    if(!strcmp(reason,"empty_cached"))++notes;
    else if(!strcmp(reason,"installed"))++installs;
    else {assert(!strcmp(reason,"unavailable"));++unavailable;}
}
// GNU includes the frame offset in copied return types; Darwin excludes it.
// Normalize this host-runtime difference, without altering the production code.
static char *darwin_return_type(Method m) {
    char *type=method_copyReturnType(m);
    if(type){size_t n=strcspn(type,"0123456789");type[n]=0;}
    return type;
}
#define method_copyReturnType darwin_return_type
#include "input-wrapper.inc"
#undef method_copyReturnType
__attribute__((noinline)) static id original(id receiver,SEL selector)
{
    assert(receiver && selector);++calls;
    picasso_offline_query_return=qualified?(uintptr_t)__builtin_return_address(1):0;
    return status;
}
__attribute__((objc_root_class))
@interface InputFixture { Class isa; }
- (id)offlineDictationStatus;
@end
@implementation InputFixture
- (id)offlineDictationStatus { return original(self,_cmd); }
@end
int main(int argc, const char **argv)
{
    uintptr_t base=(uintptr_t)original-0x2910;
    picasso_input_install(base);assert(unavailable==1 && !installs);
    Class cls=objc_allocateClassPair(objc_getClass("InputFixture"),"AFPreferences",0);
    SEL sel=sel_registerName("offlineDictationStatus");
    const char *type=argc>1?(!strcmp(argv[1],"return")?"i16@0:8":"@24@0:8@16"):"@16@0:8";
    class_addMethod(cls,sel,(IMP)original,type);objc_registerClassPair(cls);
    if(argc>1){
        picasso_input_install(base);assert(unavailable==2 && !installs);
        assert(method_getImplementation(class_getInstanceMethod(cls,sel))==(IMP)original);
        puts("PASS incompatible ABI leaves original method unchanged");return 0;
    }
    picasso_input_install(base+4);assert(unavailable==2 && !installs);
    picasso_input_install(base);assert(installs==1);
    for(unsigned n=0;n<100;++n)picasso_input_install(base);
    assert(installs==1 && picasso_original_offline_status==original);
    id obj=class_createInstance(cls,0); qualified=1;
    // Mimic the verified query's nil guard and callback-driven re-query.
    id cache=nil;unsigned notifications=0;
    for(unsigned n=0;n<10000;++n){
        if(!cache){cache=[obj offlineDictationStatus];++notifications;}
    }
    assert(cache && notifications==1 && calls==1 && notes==1);
    qualified=0;assert([obj offlineDictationStatus]==nil && notes==1);
    qualified=1;status=class_createInstance(cls,0);
    for(unsigned n=0;n<10000;++n) assert([obj offlineDictationStatus]==status);
    qualified=0;assert([obj offlineDictationStatus]==status && notes==1);
    object_dispose(status);object_dispose(obj);
    puts("PASS actual Objective-C wrapper: 10000 re-queries converge, other-caller nil preserved, nonnil identity preserved, ABI/address rejection and repeat install");
    return 0;
}
