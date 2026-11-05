// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Two CRC-protected slots. Payload reaches FUA storage before its header.
// No claimed encryption/authentication or guarantee of sector atomicity.
#include <stdint.h>
#include <string.h>
namespace picasso_record {
constexpr uint32_t block=4096;
inline uint32_t crc(const uint8_t *p,uint64_t n) {
    uint32_t c=~0U;
    while(n--) {c^=*p++;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320U & (0U-(c&1)));}
    return ~c;
}
inline uint32_t u32(const uint8_t *p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t u64(const uint8_t *p) {return u32(p)|(uint64_t(u32(p+4))<<32);}
inline void put32(uint8_t *p,uint32_t n) {for(unsigned b=0;b<4;++b)p[b]=uint8_t(n>>(b*8));}
inline void put64(uint8_t *p,uint64_t n) {put32(p,uint32_t(n));put32(p+4,uint32_t(n>>32));}
inline bool zero(const uint8_t *p,uint64_t n) {while(n--)if(*p++)return false;return true;}
struct Record {
    uint64_t first=0,generation=0;
    uint32_t kind=0,capacity=0,length=0;
    int slot=-1;
    bool ready=false,failed=false;
    uint32_t blocks() const {return 1+(capacity+block-1)/block;}
    // Caller supplies capacity bytes of scratch and output; calls are serialized.
    // 1 restored, 2 unused, 0 IO/corruption. A surviving valid slot wins a torn peer.
    template<class Ops> int open(Ops &ops,uint64_t base,uint32_t type,uint32_t cap,uint8_t *scratch,uint8_t *out) {
        first=base;kind=type;capacity=cap;generation=0;length=0;slot=-1;ready=false;failed=false;
        if(!cap || cap%block || !scratch || !out)return 0;
        bool empty[2]={false,false};
        for(int s=0;s<2;++s) {
            uint8_t hdr[block];
            if(!ops.read(first+uint64_t(s)*blocks(),1,hdr))return 0;
            bool valid=!memcmp(hdr,"PCRCD001",8) && u32(hdr+8)==kind && u32(hdr+12)==capacity &&
                u32(hdr+28)<=capacity && u64(hdr+16)>0 && u32(hdr+32)==crc(hdr,32) && zero(hdr+36,block-36);
            // Always read the complete slot: a zero header must not hide old data.
            if(!ops.read(first+uint64_t(s)*blocks()+1,blocks()-1,scratch))return 0;
            empty[s]=zero(hdr,block) && zero(scratch,cap);
            valid=valid && u32(hdr+24)==crc(scratch,u32(hdr+28));
            if(valid && u64(hdr+16)>generation) {
                generation=u64(hdr+16);length=u32(hdr+28);slot=s;memcpy(out,scratch,length);
            }
        }
        if(slot<0 && !(empty[0]&&empty[1]))return 0;
        ready=true;return slot<0?2:1;
    }
    template<class Ops> bool save(Ops &ops,const uint8_t *data,uint32_t bytes,uint8_t *scratch) {
        if(!ready || failed || !data || !scratch || bytes>capacity || generation==~uint64_t(0))return false;
        int next=slot==0?1:0;
        memset(scratch,0,capacity);memcpy(scratch,data,bytes);
        uint8_t hdr[block]={};memcpy(hdr,"PCRCD001",8);put32(hdr+8,kind);put32(hdr+12,capacity);
        put64(hdr+16,generation+1);put32(hdr+24,crc(data,bytes));put32(hdr+28,bytes);put32(hdr+32,crc(hdr,32));
        if(!ops.write(first+uint64_t(next)*blocks()+1,blocks()-1,scratch) ||
           !ops.write(first+uint64_t(next)*blocks(),1,hdr) || !ops.sync()) {failed=true;return false;}
        slot=next;++generation;length=bytes;return true;
    }
};
}
