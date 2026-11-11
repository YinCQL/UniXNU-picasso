// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production image callback and hash gate with qualified code bytes.
#include <objc/runtime.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_ARM64 0x100000c
#define LC_UUID 0x1b
#define LC_SEGMENT_64 0x19
struct mach_header { uint32_t magic; int32_t cputype, cpusubtype; uint32_t filetype,ncmds,sizeofcmds,flags; };
struct mach_header_64 { uint32_t magic; int32_t cputype, cpusubtype; uint32_t filetype,ncmds,sizeofcmds,flags,reserved; };
struct load_command { uint32_t cmd,cmdsize; };
struct uuid_command { uint32_t cmd,cmdsize; unsigned char uuid[16]; };
struct segment_command_64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; int32_t maxprot,initprot; uint32_t nsects,flags; };
_Static_assert(sizeof(struct mach_header_64)==32 && sizeof(struct segment_command_64)==72 && sizeof(struct uuid_command)==24,"Mach-O layout");
static unsigned queued,noted;
static void *_NSConcreteStackBlock[32] __attribute__((used));
static void *_NSConcreteGlobalBlock[32] __attribute__((used));
static void *dispatch_get_main_queue(void) { return (void *)1; }
static void dispatch_async(void *queue,void (^block)(void))
{ assert(queue==(void *)1);++queued;block(); }
static void picasso_wallpaper_observe(id image,unsigned kind,unsigned ticket)
{ (void)image;(void)kind;(void)ticket;abort(); }
static void picasso_wallpaper_note(const char *stage,unsigned ticket,const char *state,
    unsigned long w,unsigned long h,unsigned long row,unsigned long bytes,unsigned bpp,
    unsigned alpha,const void *sample)
{
    (void)w;(void)h;(void)row;(void)bytes;(void)bpp;(void)alpha;(void)sample;
    assert(!strcmp(stage,"install") && ticket==4 && !strcmp(state,"installed"));++noted;
}
#define CC_SHA256_DIGEST_LENGTH SHA256_DIGEST_LENGTH
#define CC_LONG unsigned long
#define CC_SHA256 SHA256
#include "wallpaper-hash.inc"
static char *return_type(Method method)
{ char *type=method_copyReturnType(method);if(type)type[strcspn(type,"0123456789")]=0;return type; }
static char *argument_type(Method method,unsigned i)
{ char *type=method_copyArgumentType(method,i);if(type)type[strcspn(type,"0123456789")]=0;return type; }
#define method_copyReturnType return_type
#define method_copyArgumentType argument_type
#include "wallpaper-wrapper.inc"
#undef method_copyReturnType
#undef method_copyArgumentType

#include "wallpaper-image.inc"
static const unsigned char qualified_code[160]={0xf6,0x57,0xbd,0xa9,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,0xfd,0x83,0x00,0x91,0xf3,0x03,0x00,0xaa,0x88,0x53,0x0c,0xb0,0x16,0x89,0x84,0xb9,0x00,0x68,0x76,0xf8,0x80,0x03,0x00,0xb5,0xe0,0x03,0x13,0xaa,0x4f,0x05,0x1a,0x94,0xb2,0x80,0xfb,0x94,0xf4,0x03,0x00,0xaa,0x1c,0xa8,0x18,0x94,0xf5,0x03,0x00,0xaa,0xfc,0x80,0xfb,0x94,0xa8,0x0a,0x00,0xd1,0xa9,0x06,0x00,0xd1,0x1f,0x0d,0x00,0xf1,0x34,0x31,0x9f,0x9a,0xe0,0x03,0x13,0xaa,0x44,0x05,0x1a,0x94,0xa7,0x80,0xfb,0x94,0xf5,0x03,0x00,0xaa,0x00,0x10,0x6a,0x1e,0x21,0x00,0x80,0x52,0xe2,0x03,0x14,0xaa,0x03,0x00,0x80,0xd2,0x24,0x00,0x80,0x52,0xe3,0x55,0xfe,0x97,0x9f,0x80,0xfb,0x94,0x68,0x6a,0x76,0xf8,0x60,0x6a,0x36,0xf8,0x08,0x81,0xfb,0x94,0xec,0x80,0xfb,0x94,0x60,0x6a,0x76,0xf8,0xfd,0x7b,0x42,0xa9,0xf4,0x4f,0x41,0xa9,0xf6,0x57,0xc3,0xa8,0x0e,0x81,0xfb,0x14};
__attribute__((objc_root_class))
@interface Fixture { Class isa; }
@end
@implementation Fixture
@end
int main(void)
{
    unsigned char *image=calloc(1,0x460000);assert(image);
    struct mach_header_64 *header=(void *)image;
    *header=(struct mach_header_64){.magic=MH_MAGIC_64,.cputype=CPU_TYPE_ARM64,.ncmds=224,.sizeofcmds=24136};
    struct uuid_command *uuid=(void *)(header+1);
    *uuid=(struct uuid_command){.cmd=LC_UUID,.cmdsize=24,.uuid={0x22,0x03,0x4a,0xba,0x9a,0x9a,0x3b,0x94,0x93,0xa5,0xa0,0xde,0x02,0xbe,0xfd,0x2e}};
    struct segment_command_64 *text=(void *)(uuid+1);
    *text=(struct segment_command_64){.cmd=LC_SEGMENT_64,.cmdsize=72,.segname="__TEXT",.vmaddr=0x1c57ca000,.vmsize=0x460000};
    /* The qualified 20H392 image has 224 commands / 24136 bytes. Keep
     * UUID and TEXT first, then valid opaque commands to exercise that
     * real count and size without invoking unrelated loader semantics. */
    unsigned char *cursor=(unsigned char *)(text+1);
    for(unsigned i=0;i<222;++i){
        struct load_command *command=(void *)cursor;
        *command=(struct load_command){.cmd=0x80000018,.cmdsize=i==221?172:108};
        cursor+=command->cmdsize;
    }
    assert(cursor==(unsigned char *)(header+1)+header->sizeofcmds);
    memcpy(image+0x45f8fc,qualified_code,sizeof(qualified_code));
    uintptr_t base=(uintptr_t)image;intptr_t slide=(intptr_t)(base-text->vmaddr);
    Class cls=objc_allocateClassPair(objc_getClass("Fixture"),"SBHomeScreenBackdropView",0);assert(cls);
    SEL sel=sel_registerName("homeScreenBlurredContentSnapshotImage");
    IMP original=(IMP)(image+0x45f8fc);assert(class_addMethod(cls,sel,original,"@16@0:8"));objc_registerClassPair(cls);
    uuid->uuid[0]^=1;picasso_wallpaper_image_added((void *)header,slide);uuid->uuid[0]^=1;
    image[0x45f99b]^=1;picasso_wallpaper_image_added((void *)header,slide);image[0x45f99b]^=1;
    text->vmsize=0x45f99b;picasso_wallpaper_image_added((void *)header,slide);text->vmsize=0x460000;
    uuid->cmdsize=4;picasso_wallpaper_image_added((void *)header,slide);uuid->cmdsize=24;
    header->sizeofcmds=24137;picasso_wallpaper_image_added((void *)header,slide);header->sizeofcmds=24136;
    header->cputype=0;picasso_wallpaper_image_added((void *)header,slide);header->cputype=CPU_TYPE_ARM64;
    header->ncmds=header->sizeofcmds/sizeof(struct load_command)+1;
    picasso_wallpaper_image_added((void *)header,slide);header->ncmds=224;
    header->ncmds=223;picasso_wallpaper_image_added((void *)header,slide);header->ncmds=224;
    assert(!queued && !noted && !picasso_wallpaper_home_original);
    assert(method_getImplementation(class_getInstanceMethod(cls,sel))==original);
    picasso_wallpaper_image_added((void *)header,slide);
    assert(queued==1 && noted==1 && picasso_wallpaper_home_original==original);
    assert(method_getImplementation(class_getInstanceMethod(cls,sel))==(IMP)picasso_wallpaper_home);
    for(unsigned i=0;i<100;++i)picasso_wallpaper_image_added((void *)header,slide);
    assert(queued==101 && noted==1 && picasso_wallpaper_home_original==original);
    free(image);puts("PASS production image callback: real 224-command/24136-byte layout, command count bounds, full SHA256 including last byte, UUID, architecture, text bounds, malformed commands, install and repeated callback");
    return 0;
}
