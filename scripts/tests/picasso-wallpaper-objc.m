// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <objc/runtime.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static unsigned calls[5], observations[6], notes[5];
static id answer, input, config, error_value;
static void *error_slot;
static unsigned observed_ticket[6];
static const char *states[5];
static void picasso_wallpaper_observe(id image,unsigned kind,unsigned ticket)
{
    assert(kind<6);++observations[kind];observed_ticket[kind]=ticket;
    assert(image==(kind==1?input:answer));
}
static void picasso_wallpaper_note(const char *stage,unsigned ticket,const char *state,
    unsigned long w,unsigned long h,unsigned long row,unsigned long bytes,unsigned bpp,
    unsigned alpha,const void *sample)
{
    (void)w;(void)h;(void)row;(void)bytes;(void)bpp;(void)alpha;(void)sample;
    assert(!strcmp(stage,"install") && ticket<5);++notes[ticket];states[ticket]=state;
}
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
{
    assert(self && sel && error==error_slot);++calls[0];
    if(error)memcpy(error,&error_value,sizeof(id));return answer;
}
static id original_composite(id self,SEL sel,id configuration,id image)
{ assert(self && sel && image==input && configuration==config);++calls[1];return answer; }
static id original_baked(id self,SEL sel)
{ assert(self && sel);++calls[2];return answer; }
static id original_source(id self,SEL sel)
{ assert(self && sel);++calls[3];return answer; }
static id original_home(id self,SEL sel)
{ assert(self && sel);++calls[4];return answer; }
__attribute__((objc_root_class))
@interface Fixture { Class isa; }
@end
@implementation Fixture
@end
int main(int argc,const char **argv)
{
    const char *mode=argc>1?argv[1]:"normal";
    unsigned rejected=5;const char *reason="different-abi";
    if(!strcmp(mode,"missing-class")){picasso_wallpaper_install(0);picasso_wallpaper_install_home(0);
        for(unsigned i=0;i<5;++i)assert(notes[i]==1 && !strcmp(states[i],"missing-class"));
        puts("PASS missing classes reported independently");return 0;}
    if(!strcmp(mode,"return") || !strcmp(mode,"error"))rejected=0;
    if(!strcmp(mode,"composite-abi") || !strcmp(mode,"missing-method") || !strcmp(mode,"different-imp"))rejected=1;
    if(!strcmp(mode,"snapshot-abi"))rejected=2;
    if(!strcmp(mode,"home-abi") || !strcmp(mode,"home-imp") || !strcmp(mode,"home-missing"))rejected=4;
    if(!strcmp(mode,"missing-method") || !strcmp(mode,"home-missing"))reason="missing-method";
    if(!strcmp(mode,"different-imp") || !strcmp(mode,"home-imp"))reason="different-imp";
    const char *names[]={"PBUIImageEncoder","PBUIBackdropCompositor","PBUIBakedEffectSnapshotSource","PBUIURLBackedSnapshotSource","SBHomeScreenBackdropView"};
    const char *selectors[]={"createUIImageWithError:","applyEffect:toImage:","snapshot","snapshot","homeScreenBlurredContentSnapshotImage"};
    const char *types[]={"@24@0:8^@16","@32@0:8@16@24","@16@0:8","@16@0:8","@16@0:8"};
    IMP targets[]={(IMP)original_read,(IMP)original_composite,(IMP)original_baked,(IMP)original_source,(IMP)original_home};
    IMP originals[5];
    const unsigned offsets[]={0x2e9d0,0x2d970,0x2fc5c,0x5c6fc,0x45f8fc};
    /* Real executable stubs at the qualified offsets let the production
     * installer resolve its exact classes/selectors and publish all five
     * wrappers. Each stub tail-calls the host's checked original function. */
    unsigned char *mapping=mmap(NULL,0x460000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapping!=MAP_FAILED);
    for(unsigned i=0;i<5;++i){
        unsigned char *code=mapping+offsets[i];
        code[0]=0x48;code[1]=0xb8;uintptr_t address=(uintptr_t)targets[i];
        memcpy(code+2,&address,8);code[10]=0xff;code[11]=0xe0;
        originals[i]=(IMP)code;
    }
    assert(!mprotect(mapping,0x460000,PROT_READ|PROT_EXEC));
    IMP replacements[]={(IMP)picasso_wallpaper_read,(IMP)picasso_wallpaper_composite,(IMP)picasso_wallpaper_baked,(IMP)picasso_wallpaper_source,(IMP)picasso_wallpaper_home};
    IMP *saved[]={&picasso_wallpaper_read_original,&picasso_wallpaper_composite_original,&picasso_wallpaper_baked_original,&picasso_wallpaper_source_original,&picasso_wallpaper_home_original};
    Class classes[5];SEL sels[5];
    for(unsigned i=0;i<5;++i){
        classes[i]=objc_allocateClassPair(objc_getClass("Fixture"),names[i],0);assert(classes[i]);
        sels[i]=sel_registerName(selectors[i]);
        const char *type=types[i];
        if(i==rejected){
            if(!strcmp(mode,"return"))type="i24@0:8^@16";
            if(!strcmp(mode,"error"))type="@24@0:8@16";
            if(!strcmp(mode,"composite-abi"))type="@32@0:8@16q24";
            if(!strcmp(mode,"snapshot-abi") || !strcmp(mode,"home-abi"))type="@24@0:8@16";
        }
        if((strcmp(mode,"missing-method") && strcmp(mode,"home-missing")) || i!=rejected)assert(class_addMethod(classes[i],sels[i],originals[i],type));
        objc_registerClassPair(classes[i]);
    }
    /* Deliberately disagree with the expected compositor IMP in that mode. */
    if(!strcmp(mode,"different-imp"))class_replaceMethod(classes[1],sels[1],targets[1],types[1]);
    if(!strcmp(mode,"home-imp"))class_replaceMethod(classes[4],sels[4],targets[4],types[4]);
    picasso_wallpaper_install((uintptr_t)mapping);
    picasso_wallpaper_install_home((uintptr_t)mapping);
    for(unsigned i=0;i<5;++i){
        assert(notes[i]==1 && !strcmp(states[i],i==rejected?reason:"installed"));
        if(i==rejected){
            assert(!*saved[i]);
            if(strcmp(mode,"missing-method") && strcmp(mode,"home-missing"))assert(method_getImplementation(class_getInstanceMethod(classes[i],sels[i]))==(!strcmp(mode,"different-imp") || !strcmp(mode,"home-imp")?targets[i]:originals[i]));
        }else{
            assert(*saved[i]==originals[i]);
            assert(method_getImplementation(class_getInstanceMethod(classes[i],sels[i]))==replacements[i]);
        }
    }
    for(unsigned n=0;n<100;++n){picasso_wallpaper_install((uintptr_t)mapping);picasso_wallpaper_install_home((uintptr_t)mapping);}
    for(unsigned i=0;i<5;++i)assert(notes[i]==1);
    if(rejected<5){puts("PASS independent installation, ABI/IMP/missing-method refusal, bounded reasons");return 0;}
    id objects[5];for(unsigned i=0;i<5;++i)objects[i]=class_createInstance(classes[i],0);
    answer=class_createInstance(classes[0],0);input=class_createInstance(classes[0],0);config=objects[0];error_value=input;
    id error=nil;error_slot=&error;
    PicassoWallpaperRead read=(PicassoWallpaperRead)method_getImplementation(class_getInstanceMethod(classes[0],sels[0]));
    PicassoWallpaperComposite composite=(PicassoWallpaperComposite)method_getImplementation(class_getInstanceMethod(classes[1],sels[1]));
    PicassoWallpaperSnapshot baked=(PicassoWallpaperSnapshot)method_getImplementation(class_getInstanceMethod(classes[2],sels[2]));
    PicassoWallpaperSnapshot source=(PicassoWallpaperSnapshot)method_getImplementation(class_getInstanceMethod(classes[3],sels[3]));
    PicassoWallpaperSnapshot home=(PicassoWallpaperSnapshot)method_getImplementation(class_getInstanceMethod(classes[4],sels[4]));
    for(unsigned n=0;n<10000;++n){
        assert(read(objects[0],sels[0],&error)==answer && error==error_value);
        assert(composite(objects[1],sels[1],config,input)==answer);
        assert(observed_ticket[1]==observed_ticket[2]);
        assert(baked(objects[2],sels[2])==answer && source(objects[3],sels[3])==answer && home(objects[4],sels[4])==answer);
    }
    id retained_answer=answer,retained_input=input;answer=nil;input=nil;config=nil;
    assert(read(objects[0],sels[0],&error)==nil && error==error_value);
    error_slot=NULL;assert(read(objects[0],sels[0],NULL)==nil);
    assert(composite(objects[1],sels[1],nil,nil)==nil);
    assert(baked(objects[2],sels[2])==nil && source(objects[3],sels[3])==nil && home(objects[4],sels[4])==nil);
    assert(calls[0]==10002 && calls[1]==10001 && calls[2]==10001 && calls[3]==10001 && calls[4]==10001);
    assert(observations[0]==10002 && observations[1]==10001 && observations[2]==10001 && observations[3]==10001 && observations[4]==10001 && observations[5]==10001);
    /* Production installer also tolerates repeated calls once all slots exist. */
    for(unsigned n=0;n<100;++n){picasso_wallpaper_install(0);picasso_wallpaper_install_home(0);}
    for(unsigned i=0;i<5;++i){assert(notes[i]==1);object_dispose(objects[i]);}
    object_dispose(retained_answer);object_dispose(retained_input);
    assert(!munmap(mapping,0x460000));
    puts("PASS five production slots through real ObjC dispatch, argument order, image/nil/NSError identity, paired tickets, repeated install");
    return 0;
}
