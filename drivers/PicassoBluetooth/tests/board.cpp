// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PicassoBluetoothBoard.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <initializer_list>
static void put16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p,unsigned v){put16(p,v);put16(p+2,v>>16);}
int main(){
    using namespace picasso_bt;
    Clock c={};
    for(unsigned baud:{2400u,115200u,3000000u}){
        assert(clock(baud,c));unsigned d=divider(baud,c.hz,16);
        assert(d && c.hz/d/16==baud);
    }
    assert(!clock(3200000,c));assert(!divider(3000000,19200000,16));
    assert(pin_address(38)==0xf926000 && pin_address(73)==0xf949000);
    assert(rail_voltage(0,1800) && !rail_voltage(0,1801));
    assert(!rail_voltage(1,1800) && rail_voltage(2,3312) && !rail_voltage(2,3313));
    assert(unified_ldo(0x6a) && !unified_ldo(0x7c) && !unified_ldo(3));
    uint8_t db[512]={};put32(db+4,0x0c0330db);put16(db+8,4);put16(db+14,1);
    memcpy(db+144,"ldoc2",5);put32(db+160,0x40004);
    uint32_t address=0;assert(!vrm_address(db,sizeof(db),"ldoc2",address) && address==0x40004);
    assert(vrm_address(db,143,"ldoc2",address)<0);
    put16(db+14,65535);assert(vrm_address(db,sizeof(db),"ldoc2",address)<0);put16(db+14,1);
    put16(db+10,65535);assert(vrm_address(db,sizeof(db),"ldoc2",address)<0);put16(db+10,0);
    put32(db+160,0x40005);assert(vrm_address(db,sizeof(db),"ldoc2",address)<0);
    put32(db+160,0x30000);assert(vrm_address(db,sizeof(db),"ldoc2",address)<0);
    put32(db+160,0x40004);put16(db+14,2);memcpy(db+168,db+144,24);
    assert(vrm_address(db,sizeof(db),"ldoc2",address)<0);
    puts("PASS board clock, pin tile, voltage envelope and command DB malformed bounds/duplicates");
}
