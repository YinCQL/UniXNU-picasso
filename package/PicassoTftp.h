// SPDX-License-Identifier: BSD-2-Clause-Patent
// QRTR TFTP: RFC1350/2347/2348 and linux-msm/tqftpserv. Bounded volatile
// /readwrite store for DSP startup; no host filesystem access or invented files.
#ifndef PICASSO_TFTP_H
#define PICASSO_TFTP_H
#include <stdint.h>
enum { PicassoTftpPort=0x188, PicassoTftpBytes=516, PicassoTftpFiles=8,
       PicassoTftpFileBytes=4096, PicassoTftpTransfers=4 };
struct PicassoTftpFile { char name[128]; uint32_t size; bool used; uint8_t data[PicassoTftpFileBytes]; };
struct PicassoTftpTransfer {
    uint32_t node, peer, port, ticks, file, block, blockSize, offset, limit, size, expectedSize;
    uint32_t requestBytes, replyBytes, lastBytes, lifetime, interval, retryAt;
    bool used, writing, done, sizeKnown, initial;
    uint8_t request[512], reply[PicassoTftpBytes], staging[PicassoTftpFileBytes];
};
struct PicassoTftp {
    PicassoTftpFile files[PicassoTftpFiles];
    PicassoTftpTransfer transfers[PicassoTftpTransfers];
    uint32_t requests, replies, errors, events, expired, reads, writes, retries;
    uint32_t nextPort, lastNode, lastPort, lastOpcode, lastError, lastBytes, lastDestination;
    char lastName[128];
    bool announced;
};
inline unsigned picasso_tftp_u16(const uint8_t *p) { return (unsigned(p[0])<<8)|p[1]; }
inline void picasso_tftp_put16(uint8_t *p,unsigned n) { p[0]=uint8_t(n>>8);p[1]=uint8_t(n); }
inline unsigned picasso_tftp_length(const char *p) { unsigned n=0;while(p[n])++n;return n; }
inline bool picasso_tftp_equal(const uint8_t *a,const uint8_t *b,unsigned n) {
    for(unsigned i=0;i<n;++i)if(a[i]!=b[i])return false;return true;
}
inline bool picasso_tftp_name(const char *a,const char *b) {
    unsigned n=0;while(a[n] && a[n]==b[n])++n;return a[n]==b[n];
}
inline void picasso_tftp_invalidate(PicassoTftp *t) {
    t->announced=false;
    for(auto &x:t->transfers)x.used=false;
    // The store is session-scoped, not persistent calibration or Android Data.
    for(auto &f:t->files) { f.used=false;f.size=0; }
}
// Call only while the QRTR reply queue is empty. Return at most one retry,
// retaining the original TID and exact last response until the peer advances.
inline int picasso_tftp_tick(PicassoTftp *t) {
    int due=-1;
    for(unsigned n=0;n<PicassoTftpTransfers;++n) {
        auto &x=t->transfers[n];if(!x.used)continue;
        if(++x.ticks>x.lifetime) { x.used=false;++t->expired;++t->events;continue; }
        if(!x.done && x.ticks>=x.retryAt && due<0) {
            due=int(n);x.retryAt=x.ticks+x.interval;++t->retries;++t->events;
        }
    }
    return due;
}
inline unsigned picasso_tftp_error(PicassoTftp *t,uint8_t *out,unsigned error) {
    const char *s=error==1?"File not found":error==2?"Access violation":error==3?"Store full":
        error==5?"Unknown transfer":error==8?"Option rejected":"Invalid request";
    out[0]=0;out[1]=5;picasso_tftp_put16(out+2,error);
    unsigned n=0;do { out[4+n]=uint8_t(s[n]); } while(s[n++]);
    t->lastError=error;++t->errors;return 4+n;
}
inline bool picasso_tftp_string(const uint8_t *p,unsigned bytes,unsigned &at,char *out,unsigned cap) {
    unsigned n=0;
    while(at<bytes && p[at]) {
        if(n+1>=cap || p[at]<0x20 || p[at]>0x7e)return false;
        out[n++]=char(p[at++]);
    }
    if(at==bytes)return false;
    out[n]=0;++at;return n!=0;
}
inline bool picasso_tftp_number(const char *s,uint32_t &v) {
    v=0;if(!s[0])return false;
    for(unsigned n=0;s[n];++n) {
        if(s[n]<'0' || s[n]>'9' || v>(0xffffffffU-unsigned(s[n]-'0'))/10)return false;
        v=v*10+unsigned(s[n]-'0');
    }
    return true;
}
inline unsigned picasso_tftp_option(uint8_t *p,unsigned at,const char *name,unsigned value) {
    for(unsigned n=0;;++n) { p[at++]=uint8_t(name[n]);if(!name[n])break; }
    char digits[10];unsigned n=0;do { digits[n++]=char('0'+value%10);value/=10; }while(value);
    while(n)p[at++]=uint8_t(digits[--n]);p[at++]=0;return at;
}
inline unsigned picasso_tftp_data(PicassoTftp *t,PicassoTftpTransfer &x,uint8_t *out) {
    unsigned count=x.limit-x.offset;if(count>x.blockSize)count=x.blockSize;
    out[0]=0;out[1]=3;picasso_tftp_put16(out+2,x.block);
    for(unsigned n=0;n<count;++n)out[4+n]=t->files[x.file].data[x.offset+n];
    x.lastBytes=count;return count+4;
}
// Caller reserves one QRTR response slot (plus RESUME if needed) BEFORE this
// mutates anything. Only the current HELLO peer may reach this endpoint.
inline unsigned picasso_tftp_receive(PicassoTftp *t,uint32_t node,uint32_t peer,uint32_t port,
                                    const uint8_t *p,unsigned bytes,uint8_t *out,uint32_t *from) {
    ++t->requests;++t->events;t->lastNode=node;t->lastPort=peer;t->lastBytes=bytes;t->lastError=0;
    t->lastDestination=port;
    t->lastOpcode=bytes>=2?picasso_tftp_u16(p):0;*from=port;
    unsigned op=t->lastOpcode;
    if(bytes<2)return picasso_tftp_error(t,out,4);
    auto save=[&](PicassoTftpTransfer &x,unsigned n) {
        x.ticks=0;x.retryAt=x.interval;x.replyBytes=n;for(unsigned i=0;i<n;++i)x.reply[i]=out[i];return n;
    };
    auto repeat=[&](PicassoTftpTransfer &x) {
        x.ticks=0;x.retryAt=x.interval;for(unsigned i=0;i<x.replyBytes;++i)out[i]=x.reply[i];return x.replyBytes;
    };
    if(port!=PicassoTftpPort) {
        PicassoTftpTransfer *slot=nullptr;
        for(auto &x:t->transfers)if(x.used && x.port==port && x.node==node && x.peer==peer)slot=&x;
        if(!slot) { if(op==5)return 0;return picasso_tftp_error(t,out,5); }
        auto &x=*slot;
        if(op==5) { x.used=false;return 0; }
        if(bytes<4)return picasso_tftp_error(t,out,4);
        unsigned block=picasso_tftp_u16(p+2);
        if(x.writing && op==3) {
            unsigned count=bytes-4;
            if(count>x.blockSize)return picasso_tftp_error(t,out,4);
            if(block+1==x.block && block) {
                if(count!=x.lastBytes || !picasso_tftp_equal(p+4,x.staging+x.size-count,count))
                    return picasso_tftp_error(t,out,4);
                return repeat(x);
            }
            if(x.done || block!=x.block)return picasso_tftp_error(t,out,4);
            if(count>PicassoTftpFileBytes-x.size || (x.sizeKnown && x.size+count>x.expectedSize)) {
                x.used=false;return picasso_tftp_error(t,out,3);
            }
            if(count<x.blockSize && x.sizeKnown && x.size+count!=x.expectedSize) {
                x.used=false;return picasso_tftp_error(t,out,4);
            }
            for(unsigned n=0;n<count;++n)x.staging[x.size+n]=p[4+n];
            x.size+=count;x.lastBytes=count;++x.block;x.initial=false;
            if(count<x.blockSize) {
                auto &f=t->files[x.file];
                // Match pwrite/O_CREAT: seek writes preserve the existing tail.
                f.size=x.size>x.limit?x.size:x.limit;
                for(unsigned n=0;n<f.size;++n)f.data[n]=x.staging[n];
                f.used=true;x.done=true;++t->writes;
            }
            out[0]=0;out[1]=4;picasso_tftp_put16(out+2,block);return save(x,4);
        }
        if(!x.writing && op==4 && bytes==4) {
            // Duplicate ACKs do not retransmit DATA (RFC1123 4.2.3.1).
            if(block+1==x.block)return 0;
            if(block!=x.block)return picasso_tftp_error(t,out,4);
            if(x.done) { x.ticks=0;return 0; }
            // ACK0 completes OACK. Each later ACK advances one real block.
            if(block && x.lastBytes<x.blockSize) { x.done=true;x.initial=false;x.ticks=0;++t->reads;return 0; }
            x.initial=false;x.offset+=x.lastBytes;++x.block;
            return save(x,picasso_tftp_data(t,x,out));
        }
        return picasso_tftp_error(t,out,4);
    }
    if(op==5)return 0;
    if((op!=1 && op!=2) || bytes>512)return picasso_tftp_error(t,out,4);
    char name[128]={},mode[16]={};unsigned at=2;
    if(!picasso_tftp_string(p,bytes,at,name,sizeof name) || !picasso_tftp_string(p,bytes,at,mode,sizeof mode))
        return picasso_tftp_error(t,out,4);
    for(unsigned n=0;n<128;++n)t->lastName[n]=name[n];
    for(unsigned n=0;mode[n];++n)if(mode[n]>='A' && mode[n]<='Z')mode[n]+=32;
    if(!picasso_tftp_name(mode,"octet"))return picasso_tftp_error(t,out,4);
    // Reject dot components, backslashes, repeated separators and trailing '/'.
    unsigned start=1;
    if(name[0]!='/')return picasso_tftp_error(t,out,2);
    for(unsigned n=1;;++n) {
        if(name[n]=='\\')return picasso_tftp_error(t,out,2);
        if(name[n]=='/' || !name[n]) {
            unsigned len=n-start;
            if(!len || (len==1 && name[start]=='.') || (len==2 && name[start]=='.' && name[start+1]=='.'))
                return picasso_tftp_error(t,out,2);
            start=n+1;
        }
        if(!name[n])break;
    }
    unsigned seen=0,blockSize=512,seek=0,tsize=0,rsize=0,timeout=1000;
    const char *names[]={"blksize","timeoutms","tsize","wsize","seek","rsize","timeout"};
    while(at<bytes) {
        char key[16]={},value[16]={};uint32_t v=0;
        if(!picasso_tftp_string(p,bytes,at,key,sizeof key) || !picasso_tftp_string(p,bytes,at,value,sizeof value) ||
            !picasso_tftp_number(value,v))return picasso_tftp_error(t,out,8);
        for(unsigned n=0;key[n];++n)if(key[n]>='A' && key[n]<='Z')key[n]+=32;
        unsigned i=0;while(i<7 && !picasso_tftp_name(key,names[i]))++i;
        if(i==7 || (seen&(1U<<i)))return picasso_tftp_error(t,out,8);seen|=1U<<i;
        if(i==0) { if(v<8 || v>65464)return picasso_tftp_error(t,out,8);blockSize=v>512?512:v; }
        if(i==1) { if(!v || v>255000)return picasso_tftp_error(t,out,8);timeout=v; }
        if(i==2)tsize=v;
        if(i==3 && (!v || v>65535))return picasso_tftp_error(t,out,8);
        if(i==4)seek=v;
        if(i==5)rsize=v;
        if(i==6) { if(!v || v>255)return picasso_tftp_error(t,out,8);timeout=v*1000; }
    }
    if((seen&2) && (seen&64))return picasso_tftp_error(t,out,8);
    if(seek>PicassoTftpFileBytes || rsize>PicassoTftpFileBytes ||
        (op==1 && tsize) || (op==2 && (seen&32)) || ((seen&32) && !rsize))
        return picasso_tftp_error(t,out,8);
    for(auto &x:t->transfers)if(x.used && x.node==node && x.peer==peer) {
        if(x.requestBytes==bytes && picasso_tftp_equal(x.request,p,bytes) &&
            x.initial) { *from=x.port;return repeat(x); }
        if(!x.done)return picasso_tftp_error(t,out,4);
    }
    bool writable=picasso_tftp_length(name)>11 && picasso_tftp_equal(reinterpret_cast<const uint8_t *>(name),
        reinterpret_cast<const uint8_t *>("/readwrite/"),11);
    if(!writable)return picasso_tftp_error(t,out,op==1?1:2);
    unsigned file=PicassoTftpFiles;
    for(unsigned n=0;n<PicassoTftpFiles;++n)if(t->files[n].used && picasso_tftp_name(t->files[n].name,name))file=n;
    if(file==PicassoTftpFiles && op==1)return picasso_tftp_error(t,out,1);
    auto reserved=[&](unsigned n) {
        for(const auto &x:t->transfers)if(x.used && !x.done && x.file==n)return true;return false;
    };
    // Include uncommitted new filenames in the reservation check.
    for(const auto &x:t->transfers)if(x.used && !x.done && picasso_tftp_name(t->files[x.file].name,name))
        return picasso_tftp_error(t,out,3);
    if(file==PicassoTftpFiles)for(unsigned n=0;n<PicassoTftpFiles;++n)if(!t->files[n].used && !reserved(n)) { file=n;break; }
    if(file==PicassoTftpFiles || reserved(file))return picasso_tftp_error(t,out,3);
    auto &f=t->files[file];unsigned oldSize=f.used?f.size:0,offset=seek;
    if(offset>oldSize || (op==2 && tsize>PicassoTftpFileBytes-offset))return picasso_tftp_error(t,out,3);
    PicassoTftpTransfer *slot=nullptr;
    for(auto &x:t->transfers)if(!x.used) { slot=&x;break; }
    if(!slot)for(auto &x:t->transfers)if(x.done) { slot=&x;break; }
    if(!slot || t->nextPort>=0xfffd-PicassoTftpPort)return picasso_tftp_error(t,out,3);
    auto &x=*slot;x.used=true;x.writing=op==2;x.done=false;x.initial=true;x.ticks=0;x.file=file;
    x.interval=(timeout+19)/20;x.lifetime=x.interval*6;
    x.node=node;x.peer=peer;x.port=++t->nextPort+PicassoTftpPort;*from=x.port;
    x.block=op==2?1:0;x.blockSize=blockSize;x.offset=offset;x.lastBytes=0;x.size=offset;
    x.limit=oldSize;if(rsize && rsize<x.limit-offset)x.limit=offset+rsize;
    x.expectedSize=offset+tsize;x.sizeKnown=(seen&4)!=0;
    x.requestBytes=bytes;for(unsigned n=0;n<bytes;++n)x.request[n]=p[n];
    for(unsigned n=0;n<128;++n)f.name[n]=name[n];
    if(x.writing)for(unsigned n=0;n<oldSize;++n)x.staging[n]=f.data[n];
    if(seen) {
        out[0]=0;out[1]=6;unsigned len=2;
        const unsigned values[]={blockSize,timeout,op==1?f.size:tsize,1,seek,rsize,timeout/1000};
        for(unsigned i=0;i<7;++i)if(seen&(1U<<i))len=picasso_tftp_option(out,len,names[i],values[i]);
        return save(x,len);
    }
    if(op==2) { out[0]=0;out[1]=4;out[2]=out[3]=0;return save(x,4); }
    x.block=1;return save(x,picasso_tftp_data(t,x,out));
}
#endif
