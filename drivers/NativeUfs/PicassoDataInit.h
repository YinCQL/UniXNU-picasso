// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PicassoRecord.h"
// One-time replacement journal, outside Data. Pending is durable before reset;
// complete is durable before publishing Data. A complete journal NEVER grants
// reset again, including after a damaged header or a different seed.
namespace picasso_data_init {
constexpr uint32_t identityBytes=88, payloadBytes=identityBytes+4;
struct Journal {
    picasso_record::Record record;
    uint8_t payload[payloadBytes]={};
    bool pending=false,complete=false;
    template<class Ops> int begin(Ops &ops,uint64_t first,const uint8_t identity[identityBytes],
                                 bool authorized,uint8_t *scratch,uint8_t *saved) {
        pending=complete=false;
        int state=record.open(ops,first,4,4096,scratch,saved);
        memcpy(payload,identity,identityBytes);
        if(state==2) {
            if(!authorized)return 0;
            picasso_record::put32(payload+identityBytes,1);
            if(!record.save(ops,payload,payloadBytes,scratch))return 0;
            pending=true;return 1;
        }
        if(state!=1 || record.length!=payloadBytes || memcmp(saved,identity,identityBytes))return 0;
        uint32_t phase=picasso_record::u32(saved+identityBytes);
        if(phase==2){complete=true;return 2;}
        if(phase!=1 || !authorized)return 0;
        pending=true;return 1;
    }
    template<class Ops> bool finish(Ops &ops,uint8_t *scratch) {
        if(!pending && !complete)return false;
        picasso_record::put32(payload+identityBytes,2);
        // Retire pending in BOTH slots before publication. Otherwise corruption
        // of the sole complete slot could revive pending and erase live Data.
        // Re-establish redundancy when resuming after a torn completion too.
        if(!record.save(ops,payload,payloadBytes,scratch) ||
           !record.save(ops,payload,payloadBytes,scratch))return false;
        pending=false;complete=true;return true;
    }
};
}
