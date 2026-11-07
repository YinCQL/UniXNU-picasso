// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoQmi.h"
int main() {
    PicassoQmi q={};q.bound=true;q.node=5;q.port=3;
    uint8_t packet[96];unsigned bytes=picasso_qmi_query(&q,1,packet);
    const uint8_t request[]={0,2,0,0x21,0,21,0,1,18,0,
        'm','s','m','/','a','d','s','p','/','s','e','n','s','o','r','_','p','d'};
    assert(bytes==60 && !memcmp(packet+32,request,sizeof request));
    assert(picasso_qmi_u32(packet)==1 && picasso_qmi_u32(packet+4)==1 && picasso_qmi_u32(packet+12)==0x186);
    assert(picasso_qmi_u32(packet+20)==28 && picasso_qmi_u32(packet+24)==5 && picasso_qmi_u32(packet+28)==3);
    assert(picasso_qmi_due(&q)==0);q.domains[0].sent=q.domains[0].pending=true;
    uint8_t response[]={2,1,0,0x21,0,14,0,2,4,0,0,0,0,0,0x10,4,0,0xff,0xff,0xff,0x1f};
    assert(!picasso_qmi_receive(&q,6,3,0x186,response,sizeof response));
    assert(!picasso_qmi_receive(&q,5,4,0x186,response,sizeof response));
    assert(!picasso_qmi_receive(&q,5,3,0x187,response,sizeof response));
    response[1]=2;assert(picasso_qmi_receive(&q,5,3,0x186,response,sizeof response));
    assert(q.ignored==1 && q.domains[0].pending);response[1]=1;
    for(unsigned n=0;n<sizeof response;++n)picasso_qmi_receive(&q,5,3,0x186,response,n);
    assert(q.malformed==sizeof response && q.domains[0].pending);
    response[20]=0x10;picasso_qmi_receive(&q,5,3,0x186,response,sizeof response);
    assert(q.malformed==sizeof response+1);response[20]=0x1f;
    assert(picasso_qmi_receive(&q,5,3,0x186,response,sizeof response));
    assert(q.domains[0].completed && q.domains[0].stateValid && q.domains[0].state==0x1fffffff);
    assert(picasso_qmi_due(&q)==1);q.domains[1].sent=q.domains[1].pending=true;
    response[1]=2;response[20]=0x0f;response[10]=1;response[12]=3;
    picasso_qmi_receive(&q,5,3,0x186,response,sizeof response);
    assert(q.domains[1].completed && q.domains[1].error==-5 && !q.domains[1].stateValid && q.domains[1].remoteError==3);
    q.domains[2].sent=q.domains[2].pending=true;
    for(unsigned n=0;n<100;++n)assert(picasso_qmi_due(&q)==-1);
    assert(picasso_qmi_due(&q)==-1 && q.domains[2].error==-60);
    picasso_qmi_lost(&q);assert(!q.bound && q.lost && !q.domains[0].stateValid);
    // Duplicate result TLVs and malformed optional lengths cannot complete.
    q={};q.bound=true;q.node=5;q.port=3;q.domains[0].pending=true;
    const uint8_t duplicate[]={2,1,0,0x21,0,14,0,2,4,0,0,0,0,0,2,4,0,0,0,0,0};
    picasso_qmi_receive(&q,5,3,0x186,duplicate,sizeof duplicate);assert(q.malformed==1 && q.domains[0].pending);
    const uint8_t optional[]={2,1,0,0x21,0,7,0,2,4,0,0,0,0,0};
    picasso_qmi_receive(&q,5,3,0x186,optional,sizeof optional);
    assert(q.domains[0].completed && !q.domains[0].stateValid && !q.domains[0].error);
    // Refresh is deferred until all startup transactions finish, and uses
    // IDs 4..6 so delayed/duplicate startup replies cannot become fresh state.
    q={};q.bound=true;q.node=5;q.port=3;picasso_qmi_registry_idle(&q,0);assert(!q.refreshRequested);
    picasso_qmi_registry_idle(&q,1283);assert(q.refreshRequested && q.registryRequests==1283);
    assert(picasso_qmi_due(&q)==0 && !q.round);
    for(auto &d:q.domains) { d.sent=true;d.completed=true;d.stateValid=true;d.state=0x1fffffff; }
    q.domains[2].pending=true;
    assert(picasso_qmi_due(&q)==-1 && q.refreshRequested && !q.round);
    q.domains[2].pending=false;
    assert(picasso_qmi_due(&q)==0 && q.round==1 && !q.refreshRequested && q.registryRequests==1283);
    for(const auto &d:q.domains)assert(!d.stateValid && !d.completed && !d.sent && !d.pending);
    for(unsigned n=0;n<3;++n) {
        bytes=picasso_qmi_query(&q,n,packet);assert(bytes<=96 && picasso_qmi_u16(packet+33)==n+4);
        auto &d=q.domains[n];d.sent=d.pending=true;
        response[1]=uint8_t(n+1);response[10]=response[12]=0;response[20]=0x1f;
        picasso_qmi_receive(&q,5,3,0x186,response,sizeof response);assert(d.pending && !d.stateValid);
        response[1]=uint8_t(n+4);response[20]=n==1?0x0f:0x1f;
        picasso_qmi_receive(&q,5,3,0x186,response,sizeof response);
        assert(!d.pending && d.completed && d.stateValid && d.state==(n==1?0x0fffffffU:0x1fffffffU));
    }
    assert(picasso_qmi_due(&q)==-1);picasso_qmi_registry_idle(&q,2000);
    assert(!q.refreshRequested && q.registryRequests==1283 && q.round==1);
    picasso_qmi_lost(&q);assert(!q.bound && !q.domains[1].stateValid);
    q={};picasso_qmi_registry_idle(&q,20);assert(picasso_qmi_due(&q)==-1 && !q.round);
    q.bound=true;for(auto &d:q.domains)d.sent=true;
    assert(picasso_qmi_due(&q)==0 && q.round==1);q.domains[0].sent=q.domains[0].pending=true;
    for(unsigned n=0;n<101;++n)picasso_qmi_due(&q);
    assert(q.domains[0].error==-60 && !q.domains[0].stateValid);
    puts("PASS QMI QUERY_STATE exact wire, endpoints, transactions, TLV bounds, errors, optional state, timeout and loss");
    puts("PASS registry-idle QMI refresh ordering, unique transactions, stale replies, actual DOWN/UP states, one-shot bound and timeout");
}
