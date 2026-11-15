/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoDvfs.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstddef>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>
struct Lock {std::mutex mutex;std::condition_variable condition;std::thread::id owner;};
struct Submit {bool bad=false;bool poisoned()const{return bad;}};
struct Clock {void *cookie=nullptr;uint64_t (*now_us)(void *);};
using IOReturn=int;using wait_result_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnTimeout=1,THREAD_UNINT=0;
struct Ring {bool bad=false;int state=0;bool poisoned()const{return bad;} int idle(const Clock*,int){return state;}};
static std::atomic<uint64_t> clockUs{1000000};
static uint64_t nowUs(void *) {return clockUs.load();}
static Lock lock;static Submit submit;static Ring ring;
static struct {Lock *lock;Submit *submit;Ring *ring;Clock clock;} g={&lock,&submit,&ring,{nullptr,nowUs}};
static void IOLockLock(Lock *l) {l->mutex.lock();l->owner=std::this_thread::get_id();}
static void IOLockUnlock(Lock *l) {assert(l->owner==std::this_thread::get_id());l->owner={};l->mutex.unlock();}
static unsigned wakes=0;static std::atomic<unsigned> sleeps{0};
static void IOLockWakeup(Lock *l,void *,bool){assert(l->owner==std::this_thread::get_id());++wakes;l->condition.notify_all();}
static void IOLockSleep(Lock *l,void *,int){
 assert(l->owner==std::this_thread::get_id());l->owner={};++sleeps;
 std::unique_lock<std::mutex> guard(l->mutex,std::adopt_lock);l->condition.wait(guard);
 guard.release();l->owner=std::this_thread::get_id();
}
static void IOSleep(unsigned ms){clockUs+=uint64_t(ms)*1000;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
struct Done{};static void *current_thread(){return nullptr;}static void thread_terminate(void *){throw Done{};}
enum AdrenoHfiStatus {AdrenoHfiSuccess,AdrenoHfiTimeout,AdrenoHfiFirmwareError};
struct AdrenoHfi {unsigned poisoned=0,pending_header=0;};
struct GmuState {
 bool booted=true,hfiStarted=true,dcvsFailed=false,bandwidthTable=false;
 uint32_t votesAvailable=5,perfCount=5,perfIndex=4,perfHz[16]={0,275000000,400000000,500000000,625000000};
 uint8_t bwIndex[16]={0,1,2,3,4};unsigned bandwidthLevels=5;AdrenoHfi hfi;
};
static GmuState state;static GmuState *gState=&state;
constexpr uint32_t kGmuGxPwrStatus=0x1F8D0;
static uint32_t power=0,busy=0;static uint64_t cycles=0;static unsigned gpuReads=0,hfiCalls=0;
static AdrenoHfiStatus result=AdrenoHfiSuccess;static uint32_t expectedBw=255;
static bool requireLock=false,tear=false;static unsigned hiReads=0;
static uint32_t coreRead(uint32_t addr){if(requireLock)assert(lock.owner==std::this_thread::get_id());return addr==kGmuGxPwrStatus?power:busy;}
static uint32_t gpuRead(uint32_t addr){++gpuReads;if(addr==0x401)return tear?++hiReads:uint32_t(cycles>>32);return uint32_t(cycles);}
static int gxWait(void *) {return 0;}
static AdrenoHfiStatus AdrenoHfiExchange(AdrenoHfi *h,unsigned id,const uint32_t *v,size_t words,unsigned budget,int(*wait)(void*),uint32_t*,size_t cap,size_t *count,uint32_t *error){
 if(requireLock)assert(lock.owner==std::this_thread::get_id());
 ++hfiCalls;assert(id==30 && words==3 && v[0]==1 && v[1]>=1 && v[1]<state.perfCount && v[2]==expectedBw);
 assert(budget==20 && wait==gxWait && cap==16);*count=3;*error=result==AdrenoHfiFirmwareError?7:0;
 if(result==AdrenoHfiTimeout)h->poisoned=1;return result;
}
#include "gmu.inc"
#include "publish.inc"
static NativeAdrenoDvfsSample sample(){NativeAdrenoDvfsSample s={};assert(!native_adreno620_dvfs_read(&s));return s;}
static void policyTests(){
 auto s=sample();NativeAdrenoDvfsPolicy p;uint64_t t=1000000;p.lastWork=t;p.baseline(t,s);
 assert(p.sample(t+50000,s,true)==4);assert(p.sample(t+100000,s,true)==3);s.current=3;
 assert(p.sample(t+150000,s,true)==3);assert(p.sample(t+200000,s,true)==2);s.current=2;
 assert(p.sample(t+250000,s,true)==1);s.current=1;assert(p.canPark(t+250000,s,true));
 assert(p.work(t+300000,s,true)==4);assert(!p.canPark(t+300000,s,true));s.current=4;
 assert(p.sample(t+350000,s,true)==4);assert(p.sample(t+400000,s,true)==4);
 assert(p.work(t+400001,s,false)==4);s.current=1;s.busyTicks+=768000;
 assert(p.sample(t+450000,s,false)==3); // 80% at 275 -> 500 MHz.
 p.invalidate();s.busyTicks=0xffff0000;p.baseline(t,s);s.busyTicks+=480000;s.current=2;
 assert(p.sample(t+50000,s,false)==2 && p.busyPermille==500);
 s.busyTicks=0;p.sample(t+100000,s,false);assert(!p.valid); // reset cannot be quiet
 p.invalidate();s.busyTicks=0;p.baseline(t,s);assert(p.sample(t+500000,s,true)==2 && !p.valid);
 for(unsigned count=0;count<=17;++count){auto bad=s;bad.count=count;if(count!=5 && count!=4 && count!=3 && count!=2)assert(!NativeAdrenoDvfsPolicy::usable(bad));}
 auto bad=s;bad.current=0;assert(!NativeAdrenoDvfsPolicy::usable(bad));bad=s;bad.hz[2]=bad.hz[1];assert(!NativeAdrenoDvfsPolicy::usable(bad));
 for(unsigned current=1;current<5;++current)for(unsigned load=0;load<=1000;load+=10){
  NativeAdrenoDvfsPolicy test;s.current=current;s.busyTicks=0;test.baseline(t,s);s.busyTicks=load*960;
  unsigned v=test.sample(t+50000,s,false);assert(v>=1 && v<5);
 }
}
static void gmuTests(){
 assert(native_adreno620_dvfs_vote(0)==-1 && native_adreno620_dvfs_vote(5)==-1 && hfiCalls==0);
 power=1<<6;unsigned reads=gpuReads;NativeAdrenoDvfsSample s={};
 assert(native_adreno620_dvfs_read(&s)==-1 && gpuReads==reads && native_adreno620_dvfs_vote(1)==-1);power=0;
 tear=true;assert(native_adreno620_dvfs_read(&s)==-1);tear=false;
 assert(!native_adreno620_dvfs_vote(1) && state.perfIndex==1 && hfiCalls==1);
 assert(!native_adreno620_dvfs_vote(1) && hfiCalls==1);
 expectedBw=3;state.bandwidthTable=true;assert(!native_adreno620_dvfs_vote(3));state.bandwidthTable=false;expectedBw=255;
 for(auto failure:{AdrenoHfiFirmwareError,AdrenoHfiTimeout}){
  state=GmuState{};result=failure;assert(native_adreno620_dvfs_vote(1)==-1 && state.perfIndex==4 && state.dcvsFailed);
  unsigned calls=hfiCalls;assert(native_adreno620_dvfs_vote(1)==-1 && hfiCalls==calls && native_adreno620_dvfs_read(&s)==-1);
 }
 state=GmuState{};result=AdrenoHfiSuccess;state.votesAvailable=4;assert(native_adreno620_dvfs_read(&s)==-1);state.votesAvailable=5;
}
static void publishTests(){
 requireLock=true;IOLockLock(g.lock);gDvfsEnabled=true;gDvfsParked=true;state.perfIndex=1;
 nativeAdrenoDvfsWork();assert(!gDvfsParked && wakes==1 && state.perfIndex==4);
 unsigned calls=hfiCalls,reads=gpuReads;clockUs+=16000;nativeAdrenoDvfsWork();assert(hfiCalls==calls && gpuReads==reads);
 result=AdrenoHfiFirmwareError;NativeAdrenoDvfsSample s={};assert(!native_adreno620_dvfs_read(&s));
 assert(!nativeAdrenoDvfsVote(1,s) && !gDvfsEnabled && s.current==4 && gDvfsErrors==1);
 state=GmuState{};state.perfIndex=1;result=AdrenoHfiSuccess;
 gDvfs=NativeAdrenoDvfsPolicy{};gDvfsEnabled=true;gDvfsParked=true;IOLockUnlock(g.lock);
 std::thread worker([]{try {nativeAdrenoDvfs(nullptr,0);}catch(Done&) {}});
 while(!sleeps.load())std::this_thread::yield();
 IOLockLock(g.lock);nativeAdrenoDvfsWork();assert(state.perfIndex==4 && !gDvfsParked);IOLockUnlock(g.lock);
 for(unsigned i=0;i<100;++i){IOLockLock(g.lock);bool parked=gDvfsParked;IOLockUnlock(g.lock);if(parked)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
 IOLockLock(g.lock);assert(gDvfsParked && state.perfIndex==1);gDvfsEnabled=false;IOLockWakeup(g.lock,&gDvfs,false);IOLockUnlock(g.lock);worker.join();
}
int main(){policyTests();gmuTests();publishTests();puts("PASS production GPU DVFS");}
