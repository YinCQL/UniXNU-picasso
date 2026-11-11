// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production qualified UIImage content lookup with real ObjC slots.
#include <objc/runtime.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef void *IOSurfaceRef;
static uintptr_t picasso_wallpaper_uikit_base;
static unsigned getter_calls;
static IOSurfaceRef getter(id image,SEL selector){assert(image&&selector);++getter_calls;return (void *)0x1234;}
static char *return_type(Method method){char *type=method_copyReturnType(method);if(type)type[strcspn(type,"0123456789")]=0;return type;}
#define method_copyReturnType return_type
#include "wallpaper-content.inc"
#undef method_copyReturnType
__attribute__((objc_root_class)) @interface Fixture { Class isa; } @end
@implementation Fixture @end
int main(int argc,char **argv){
 const char *mode=argc>1?argv[1]:"normal";
 Class image=objc_allocateClassPair(objc_getClass("Fixture"),"UIImage",0);assert(image);
 assert(class_addIvar(image,"padding",!strcmp(mode,"offset")?88:96,3,"[96c]"));
 assert(class_addIvar(image,"_content",8,3,!strcmp(mode,"ivar-type")?"@":"@\"_UIImageContent\""));objc_registerClassPair(image);
 Class content=objc_allocateClassPair(objc_getClass("Fixture"),"_UIImageIOSurfaceContent",0);assert(content);
 SEL sel=sel_registerName("IOSurface");
 unsigned char *mapping=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mapping!=MAP_FAILED);unsigned char *code=mapping+16;
 code[0]=0x48;code[1]=0xb8;uintptr_t target=(uintptr_t)getter;memcpy(code+2,&target,8);code[10]=0xff;code[11]=0xe0;
 const char *type=!strcmp(mode,"abi")?"@16@0:8":"^{__IOSurface=}16@0:8";
 if(strcmp(mode,"missing"))assert(class_addMethod(content,sel,(IMP)code,type));objc_registerClassPair(content);
 id object=class_createInstance(image,0),backing=class_createInstance(content,0);assert(object&&backing);
 Ivar ivar=class_getInstanceVariable(image,"_content");object_setIvar(object,ivar,backing);
 picasso_wallpaper_uikit_base=(uintptr_t)code-0x50af90;
 if(!strcmp(mode,"imp"))class_replaceMethod(content,sel,(IMP)getter,type);
 if(!strcmp(mode,"nil"))object_setIvar(object,ivar,nil);
 if(!strcmp(mode,"class"))object_setIvar(object,ivar,object);
 if(!strcmp(mode,"unqualified"))picasso_wallpaper_uikit_base=0;
 if(!strcmp(mode,"normal")){assert(picasso_wallpaper_image_surface(object)==(void *)0x1234&&getter_calls==1);}
 else assert(!picasso_wallpaper_image_surface(object)&&!getter_calls);
 object_dispose(object);object_dispose(backing);munmap(mapping,4096);puts("PASS production content lookup / exact class, ivar offset/type, getter IMP/ABI, nil and qualification gate");return 0;
}
