/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoBoard.h"
enum { CellBytes = 4 };
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}
static uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
IOReturn NativeAdrenoBoard::parseIommus(const uint8_t *cells,size_t length,
    uint32_t cells_per_entry,uint16_t *stream_ids,size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || cells_per_entry<1 || cells_per_entry>4)
        return kIOReturnBadArgument;
    const size_t per_entry=(1+cells_per_entry)*(size_t)CellBytes;
    if(length%per_entry)return kIOReturnBadArgument;
    const size_t total=length/per_entry;
    if(!stream_ids || total>capacity)return kIOReturnNoResources;
    for(size_t index=0;index<total;++index) {
        const uint8_t *at=cells+index*per_entry;
        const uint32_t ph_be=be32(at);
        const uint32_t ph_le=le32(at);
        if(!ph_be && !ph_le)return kIOReturnBadArgument; /* phandle must be named */
        const bool is_le=(ph_le<ph_be);
        const uint32_t sid=is_le?le32(at+CellBytes):be32(at+CellBytes);
        if(sid>0xffffu)return kIOReturnBadArgument;
        stream_ids[index]=static_cast<uint16_t>(sid);
    }
    *count=total;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoBoard::parseReg(const uint8_t *cells,size_t length,
    uint32_t address_cells,uint32_t size_cells,NativeAdrenoWindow *windows,
    size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || (address_cells<1 || address_cells>2) ||
        (size_cells<1 || size_cells>2))return kIOReturnBadArgument;
    const size_t per_window=(address_cells+size_cells)*(size_t)CellBytes;
    if(length%per_window)return kIOReturnBadArgument;
    const size_t total=length/per_window;
    if(total>capacity || !windows)return kIOReturnNoResources;
    for(size_t index=0;index<total;++index) {
        const uint8_t *at=cells+index*per_window;
        uint64_t base=0,bytes=0;
        bool adt_le=false;
        if(address_cells==2 && size_cells==2) {
            const uint32_t w1_le=le32(at+CellBytes);
            const uint32_t w3_le=le32(at+3*CellBytes);
            const uint32_t w0_be=be32(at);
            const uint32_t w1_be=be32(at+CellBytes);
            if(w1_le==0 && w3_le==0 && w0_be!=0 && w1_be==0) {
                adt_le=true;
            }
        }
        if(adt_le) {
            base=uint64_t(le32(at))|(uint64_t(le32(at+CellBytes))<<32);
            bytes=uint64_t(le32(at+2*CellBytes))|(uint64_t(le32(at+3*CellBytes))<<32);
        } else {
            for(uint32_t cell=0;cell<address_cells;++cell) {
                if(base>>32)return kIOReturnBadArgument; /* more than 64 bits */
                base=(base<<32)|be32(at+cell*CellBytes);
            }
            for(uint32_t cell=0;cell<size_cells;++cell) {
                if(bytes>>32)return kIOReturnBadArgument;
                bytes=(bytes<<32)|be32(at+(address_cells+cell)*CellBytes);
            }
        }
        if(!bytes || base+bytes<base)return kIOReturnBadArgument;
        windows[index]={base,bytes};
    }
    *count=total;
    return kIOReturnSuccess;
}
