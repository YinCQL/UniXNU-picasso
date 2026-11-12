// SPDX-License-Identifier: BSD-2-Clause-Patent
// GNU runtime fixture; only dispatch spelling and image offsets are host adapted.
#include <objc/runtime.h>
#include <objc/message.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static BOOL on_main=YES, springboard=YES, available=YES, original_result;
static unsigned requests,calls,installed,unavailable;
static uintptr_t observed_caller;
static id workspace;
__attribute__((objc_root_class))
@interface NSObject { Class isa; }
- (BOOL)respondsToSelector:(SEL)selector;
@end
@implementation NSObject
- (BOOL)respondsToSelector:(SEL)s { return class_getInstanceMethod(object_getClass(self),s)!=NULL; }
@end
@interface NSThread:NSObject
+ (BOOL)isMainThread;
@end
@implementation NSThread
+ (BOOL)isMainThread { return on_main; }
@end
@interface NSProcessInfo:NSObject
+ (NSProcessInfo *)processInfo;
- (id)processName;
- (BOOL)isEqualToString:(id)value;
@end
@implementation NSProcessInfo
+ (NSProcessInfo *)processInfo { static id p;if(!p)p=class_createInstance(self,0);return p; }
- (id)processName { return self; }
- (BOOL)isEqualToString:(id)value { (void)value;return springboard; }
@end
__attribute__((noinline)) static BOOL original(id receiver,SEL selector) {
    assert(receiver&&selector);++calls;
    observed_caller=(uintptr_t)__builtin_return_address(1);
    return original_result;
}
static id get_workspace(id receiver,SEL selector) { assert(receiver&&selector);return available?workspace:nil; }
static void show_workspace(id receiver,SEL selector) { assert(receiver==workspace&&selector);++requests; }
static id host_get(id receiver,SEL selector) { return ((id (*)(id,SEL))objc_msg_lookup(receiver,selector))(receiver,selector); }
static void host_present(id receiver,SEL selector) { ((void (*)(id,SEL))objc_msg_lookup(receiver,selector))(receiver,selector); }
static void picasso_power_note(const char *reason) {
    if(!strcmp(reason,"installed"))++installed;
    else if(!strcmp(reason,"unavailable"))++unavailable;
    else assert(!strcmp(reason,"requested"));
}
static char *darwin_return_type(Method method) {
    char *type=method_copyReturnType(method);
    if(type)type[strcspn(type,"0123456789")]=0;
    return type;
}
#define method_copyReturnType darwin_return_type
#include "power-wrapper.inc"
#undef method_copyReturnType
__attribute__((noinline)) static BOOL invoke(id receiver,SEL selector) {
    return ((BOOL (*)(id,SEL))objc_msg_lookup(receiver,selector))(receiver,selector);
}
__attribute__((noinline)) static BOOL other_invoke(id receiver,SEL selector) {
    return ((BOOL (*)(id,SEL))objc_msg_lookup(receiver,selector))(receiver,selector);
}
int main(int argc,const char **argv) {
    Class cls=objc_allocateClassPair(objc_getClass("NSObject"),"SBSiriHardwareButtonInteraction",0);
    Class wc=objc_allocateClassPair(objc_getClass("NSObject"),"SBWorkspace",0);
    Class mc=objc_allocateClassPair(objc_getClass("NSObject"),"SBMainWorkspace",0);
    SEL consume=sel_registerName("consumeLongPress"),get=sel_registerName("mainWorkspace"),present=sel_registerName("presentPowerDownTransientOverlay");
    const char *type=argc>1?(!strcmp(argv[1],"return")?"i16@0:8":"C24@0:8@16"):"C16@0:8";
    class_addMethod(cls,consume,(IMP)original,type);objc_registerClassPair(cls);
    class_addMethod(object_getClass(wc),get,(IMP)get_workspace,"@16@0:8");objc_registerClassPair(wc);
    class_addMethod(mc,present,(IMP)show_workspace,"v16@0:8");objc_registerClassPair(mc);
    workspace=class_createInstance(mc,0);id button=class_createInstance(cls,0);
    springboard=NO;picasso_power_install(0);assert(!installed&&!unavailable);springboard=YES;
    picasso_power_install(4);assert(!installed&&unavailable==1);
    if(argc>1){
        picasso_power_install(0);assert(!installed&&unavailable==2);
        assert(method_getImplementation(class_getInstanceMethod(cls,consume))==(IMP)original);
        puts("PASS incompatible power method ABI leaves original method unchanged");return 0;
    }
    assert(!picasso_power_signature(class_getClassMethod(wc,get),@encode(void)));
    assert(!picasso_power_signature(class_getInstanceMethod(mc,present),@encode(id)));
    picasso_power_install(0);assert(installed==1);
    for(unsigned n=0;n<100;n++)picasso_power_install(0);
    assert(installed==1&&picasso_original_power_consume==original);
    // Learn the real return address through the original fallback, then revisit
    // the same machine-code call instruction. No return-address builtin is mocked.
    for(unsigned phase=0;phase<5;phase++) {
        if(phase==1)picasso_power_caller=observed_caller;
        on_main=phase!=2;available=phase!=3;original_result=phase==4;
        BOOL result=invoke(button,consume);
        assert(result==(phase==1||phase==4));
    }
    assert(requests==2&&calls==3&&unavailable==2);
    original_result=YES;assert(other_invoke(button,consume));
    original_result=NO;assert(!other_invoke(button,consume));
    assert(requests==2&&calls==5);
    object_dispose(button);object_dispose(workspace);
    puts("PASS actual power wrapper: exact caller, main thread, original BOOL results, unavailable workspace, foreign process/address/ABI rejection and repeat install");
    return 0;
}
