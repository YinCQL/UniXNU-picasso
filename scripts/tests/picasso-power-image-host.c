// SPDX-License-Identifier: BSD-2-Clause-Patent
// Executes the actual image callback with the stock Mach-O load commands.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_ARM64 0x100000c
#define LC_SEGMENT_64 0x19
#define LC_UUID 0x1b
typedef bool BOOL;
#define YES true
#define NO false
struct mach_header {uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags;};
struct mach_header_64 {uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct load_command {uint32_t cmd,cmdsize;};
struct uuid_command {uint32_t cmd,cmdsize;unsigned char uuid[16];};
struct segment_command_64 {uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
static unsigned installs,shutdowns,hashes,notes;
static bool hash_ok=true;
static void picasso_power_install(uintptr_t base){assert(base);++installs;}
static void picasso_shutdown_install(uintptr_t base){assert(base);++shutdowns;}
static void picasso_power_note(const char *reason){assert(!strcmp(reason,"unavailable"));++notes;}
// Header parsing is the subject here; byte hashes are checked against the stock
// cache independently, and a failing hash must still block installation.
static bool picasso_astc_hash(uintptr_t base,unsigned offset,unsigned size,const char *expected){
    assert(base&&offset&&size&&strlen(expected)==64);++hashes;return hash_ok;
}
#include "power-image.inc"
int main(int argc,char **argv){
    assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *bytes=malloc((size_t)size);assert(bytes&&fread(bytes,1,(size_t)size,f)==(size_t)size);fclose(f);
    struct mach_header_64 *h=(void *)bytes;assert(h->ncmds==224&&h->sizeofcmds==24136);
    intptr_t slide=(intptr_t)(uintptr_t)bytes-(intptr_t)0x1c57ca000ULL;
    picasso_power_image_added((void *)bytes,slide);
    if(atoi(argv[2])){assert(!installs&&!hashes);puts("REPRODUCED stock SpringBoard 224-command header rejected before hashes/install");free(bytes);return 0;}
    assert(installs==1&&shutdowns==1&&hashes==7&&!notes);
    hash_ok=false;picasso_power_image_added((void *)bytes,slide);assert(installs==1&&notes==1);hash_ok=true;
    unsigned old=h->ncmds;h->ncmds=h->sizeofcmds/8+1;picasso_power_image_added((void *)bytes,slide);assert(installs==1);h->ncmds=old;
    struct load_command *cmd=(void *)(h+1);old=cmd->cmdsize;cmd->cmdsize=0;picasso_power_image_added((void *)bytes,slide);assert(installs==1);cmd->cmdsize=old;
    for(unsigned i=0;i<h->ncmds;i++){
        if(cmd->cmd==LC_UUID){((struct uuid_command *)cmd)->uuid[0]^=1;break;}
        cmd=(void *)((unsigned char *)cmd+cmd->cmdsize);
    }
    picasso_power_image_added((void *)bytes,slide);assert(installs==1);
    free(bytes);puts("PASS stock 224-command header accepted; malformed count/command, UUID mismatch and hash failure rejected");return 0;
}
