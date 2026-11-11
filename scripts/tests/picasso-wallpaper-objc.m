// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <objc/runtime.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls[3], observations[4], notes;
static id answer, input, config, error_value;
static void *error_slot;
static unsigned observed_ticket[4];
static void picasso_wallpaper_observe(id image,unsigned kind,unsigned ticket)
{
    assert(kind<4);++observations[kind];observed_ticket[kind]=ticket;
    assert(image==(kind==1?input:answer));
}
static void picasso_wallpaper_note(const char *stage,unsigned ticket,const char *state,
    unsigned long w,unsigned long h,unsigned long row,unsigned long bytes,unsigned bpp,
    unsigned alpha,const void *sample)
{ (void)ticket;(void)w;(void)h;(void)row;(void)bytes;(void)bpp;(void)alpha;(void)sample;
  assert(!strcmp(stage,"install") && !strcmp(state,"installed"));++notes; }
static char *return_type(Method method)
{ char *type=method_copyReturnType(method);if(type)type[strcspn(type,"0123456789")]=0;return type; }
static char *argument_type(Method method,unsigned i)
{ char *type=method_copyArgumentType(method,i);if(type)type[strcspn(type,"0123456789")]=0;return type; }
#define method_copyReturnType return_type
#define method_copyArgumentType argument_type
#include "wallpaper-wrapper.inc"
#undef method_copyReturnType
#undef method_copyArgumentType
static id original_read(id self,SEL sel,id *error)
{ assert(self && sel && error==error_slot);++calls[0];if(error)memcpy(error,&error_value,sizeof(id));return answer; }
static id original_cache(id self,SEL sel,id configuration)
{ assert(self && sel && configuration==config);++calls[1];return answer; }
static id original_composite(id self,SEL sel,id image,id configuration)
{ assert(self && sel && image==input && configuration==config);++calls[2];return answer; }
__attribute__((objc_root_class))
@interface Fixture { Class isa; }
- (id)read:(id *)error;
- (id)cache:(id)configuration;
- (id)composite:(id)image configuration:(id)configuration;
@end
@implementation Fixture
- (id)read:(id *)error { return original_read(self,_cmd,error); }
- (id)cache:(id)configuration { return original_cache(self,_cmd,configuration); }
- (id)composite:(id)image configuration:(id)configuration {return original_composite(self,_cmd,image,configuration);}
@end
int main(int argc,const char **argv)
{
    picasso_wallpaper_install(0);assert(!notes);
    Class cls=objc_allocateClassPair(objc_getClass("Fixture"),"PBUIImageEncoder",0);
    SEL read_sel=sel_registerName("createUIImageWithError:");
    const char *type=argc>1?(!strcmp(argv[1],"return")?"i24@0:8^@16":"@24@0:8@16"):"@24@0:8^@16";
    class_addMethod(cls,read_sel,(IMP)original_read,type);objc_registerClassPair(cls);
    uintptr_t base=(uintptr_t)original_read-0x2e9d0;
    picasso_wallpaper_install(base+4);assert(!notes);
    picasso_wallpaper_install(base);
    if(argc>1){assert(!notes && method_getImplementation(class_getInstanceMethod(cls,read_sel))==(IMP)original_read);
        puts("PASS wrong wallpaper ABI leaves dispatch unchanged");return 0;}
    assert(notes==1 && picasso_wallpaper_read_original==original_read);
    for(unsigned i=0;i<100;++i)picasso_wallpaper_install(base);assert(notes==1);
    id obj=class_createInstance(cls,0);answer=class_createInstance(cls,0);
    error_value=class_createInstance(cls,0);id error=nil;error_slot=&error;
    /* Invoke the actually replaced dispatch slot, preserving NSError storage. */
    typedef id (*SendRead)(id,SEL,id *);
    SendRead read=(SendRead)method_getImplementation(class_getInstanceMethod(cls,read_sel));
    for(unsigned i=0;i<10000;++i){assert(read(obj,read_sel,&error)==answer && error==error_value);}
    answer=nil;assert(read(obj,read_sel,&error)==nil && error==error_value);
    error_slot=NULL;assert(read(obj,read_sel,NULL)==nil);
    Class fixture=objc_getClass("Fixture");
    SEL cache_sel=sel_registerName("cache:"),composite_sel=sel_registerName("composite:configuration:");
    Method cache=class_getInstanceMethod(fixture,cache_sel),composite=class_getInstanceMethod(fixture,composite_sel);
    assert(picasso_wallpaper_method(cache,(uintptr_t)method_getImplementation(cache),3,0));
    assert(!picasso_wallpaper_method(cache,(uintptr_t)method_getImplementation(cache),4,0));
    assert(picasso_wallpaper_method(composite,(uintptr_t)method_getImplementation(composite),4,0));
    picasso_wallpaper_cache_original=(PicassoWallpaperCache)method_getImplementation(cache);
    picasso_wallpaper_composite_original=(PicassoWallpaperComposite)method_getImplementation(composite);
    class_replaceMethod(fixture,cache_sel,(IMP)picasso_wallpaper_cache,method_getTypeEncoding(cache));
    class_replaceMethod(fixture,composite_sel,(IMP)picasso_wallpaper_composite,method_getTypeEncoding(composite));
    Fixture *f=(Fixture *)class_createInstance(fixture,0);config=obj;input=error_value;answer=obj;
    for(unsigned i=0;i<10000;++i){assert([f cache:config]==answer);assert([f composite:input configuration:config]==answer);
        assert(observed_ticket[1]==observed_ticket[2]);}
    answer=nil;input=nil;config=nil;
    assert([f cache:nil]==nil && [f composite:nil configuration:nil]==nil);
    assert(calls[0]==10002 && calls[1]==10001 && calls[2]==10001);
    object_dispose(f);object_dispose(obj);object_dispose(error_value);
    puts("PASS real ObjC dispatch preserves image/nil/arguments/error, paired tickets, code-address/ABI refusal, repeat install");
    return 0;
}
