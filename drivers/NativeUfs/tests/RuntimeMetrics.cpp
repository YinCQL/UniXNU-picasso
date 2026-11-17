// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include "../RuntimeMetrics.h"
static uint64_t tick=10;
static uint64_t clockNow () { return tick; }
int main () {
  const uint64_t bounds[]={0,49999,50000,99999,100000,199999,200000,399999,400000,UINT64_MAX};
  const unsigned ranges[]={0,0,1,1,2,2,3,3,4,4};
  for (unsigned i=0;i<10;++i) assert(ufsWaitRange(bounds[i])==ranges[i]);
  assert(ufsElapsedTicks(10,8)==0 && ufsElapsedTicks(10,10)==0 && ufsElapsedTicks(10,13)==3);
  UfsOpMetrics m;
  { UfsMetricSpan span(m,clockNow,8192);tick=40;span.success=true; }
  { UfsMetricSpan span(m,clockNow,4096);tick=50; }
  assert(m.ops==2 && m.bytes==12288 && m.ticks==40 && m.maximum==30 && m.failures==1);
  { UfsMetricSpan span(m,clockNow,1);span.active=false; }
  assert(m.ops==2);
  std::vector<std::thread> workers;
  for (unsigned i=0;i<16;++i) workers.emplace_back([&m,i]{for(unsigned n=0;n<1000;++n) ufsMetricAccount(m,4096,100+i,true);});
  for(auto &thread:workers) thread.join();
  assert(m.ops==16002 && m.bytes==12288+16000ULL*4096 && m.maximum==115 && m.failures==1);
  assert(m.ticks==40+1000ULL*(16*100+120));
  puts("PASS production metrics: histogram boundaries, backwards clock clamp, early failure, explicit completion, 16 concurrent writers and exact atomic totals");
}
