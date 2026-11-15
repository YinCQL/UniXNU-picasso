/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <libkern/crypto/sha2.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <stddef.h>
#include <stdint.h>
int picasso_bt_firmware_read(const char *,const uint8_t **,size_t *);
void picasso_bt_firmware_release(const uint8_t *,size_t);
extern volatile int native_gpu_addon_stage;
static bool allowed(const char *name) {
    return name && (!strcmp(name,"qca/crbtfw32.tlv") || !strcmp(name,"qca/crnv32.bin") || !strcmp(name,"qca/crnv32u.bin"));
}
int picasso_bt_firmware_read(const char *name,const uint8_t **data,size_t *size) {
    if(!data || !size)return EINVAL;
    *data=NULL;*size=0;
    if(!allowed(name))return ENOENT;
    if(native_gpu_addon_stage<1)return EAGAIN;
    char path[160];snprintf(path,sizeof(path),"%s/%s","/private/var/ngpu/bluetooth-firmware",name+4);
    vfs_context_t ctx=vfs_context_kernel();vnode_t vp=NULL;
    int error=vnode_open(path,FREAD|O_NOFOLLOW,0,0,&vp,ctx);uint8_t *buffer=NULL;size_t length=0;
    if(!error) {
        struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
        error=vnode_getattr(vp,&va,ctx);
        if(!error && (vnode_vtype(vp)!=VREG || !va.va_data_size || va.va_data_size>4u*1024*1024))error=EINVAL;
        if(!error) {
            length=(size_t)va.va_data_size;buffer=IOMallocData(length);
            if(!buffer)error=ENOMEM;
            else {
                int resid=0;
                error=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,(int)length,0,UIO_SYSSPACE,IO_NOAUTH,
                    vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
                if(!error && resid)error=EIO;
            }
        }
    }
    if(vp){int closed=vnode_close(vp,FREAD,ctx);if(!error)error=closed;}
    if(!error) {
        static const struct {const char *name;size_t size;uint8_t sha[32];} expected[]={
            {"qca/crbtfw32.tlv",124092,{0x6c,0x5b,0x52,0xe8,0xbc,0xb0,0xd3,0xa4,0xf6,0x0b,0x13,0x7f,0x31,0xd8,0xe0,0xcd,0xec,0x38,0x1b,0x41,0x18,0xb9,0xa9,0x55,0xe0,0x5e,0x61,0xaf,0x61,0x59,0x5d,0x47}},
            {"qca/crnv32.bin",5488,{0x43,0xf6,0xef,0x7f,0x73,0xf7,0x21,0x54,0x40,0x69,0xaa,0x2d,0x33,0x4d,0xa5,0x98,0xd6,0x37,0xa0,0xcf,0x49,0x2b,0x3e,0x4b,0x9f,0xc6,0x4a,0x42,0xe0,0x6a,0xdc,0x26}},
            {"qca/crnv32u.bin",5488,{0x6a,0x4c,0xae,0x13,0x75,0xd8,0x24,0xa7,0x30,0xde,0x5f,0x4f,0x89,0x99,0xc5,0x29,0x01,0xe0,0x7f,0xfb,0x08,0xb8,0xb3,0x33,0x92,0x70,0xc4,0x72,0xdb,0x9f,0xb0,0xa9}}
        };
        bool matched=false;uint8_t hash[32];SHA256_CTX sha;
        SHA256_Init(&sha);SHA256_Update(&sha,buffer,length);SHA256_Final(hash,&sha);
        for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i)
            if(!strcmp(name,expected[i].name) && length==expected[i].size && !memcmp(hash,expected[i].sha,32))matched=true;
        if(!matched)error=EINVAL;
    }
    if(error){if(buffer)IOFreeData(buffer,length);return error;}
    *data=buffer;*size=length;return 0;
}
void picasso_bt_firmware_release(const uint8_t *data,size_t size) {if(data)IOFreeData((void *)data,size);}
