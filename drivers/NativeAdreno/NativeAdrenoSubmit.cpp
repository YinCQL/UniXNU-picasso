/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
extern "C" int proc_selfpid(void);
/* Board diagnostics: CP_INTERRUPT_STATUS, CP_HW_FAULT, CP_PROTECT_STATUS and one CP_SQE_STAT sample, for a stalled fence. Absent on the host. */
extern "C" uint32_t native_adreno_cp_fault_regs(uint32_t *out,uint32_t count) __attribute__((weak_import));

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,
    SubmitMaxBuffers=(uint32_t)NativeAdrenoSubmitMaxBuffers,SubmitMaxIbBytes=0xfffffu*4u,
    SubmitAuditBytes=256u*1024u};   /* IB bytes the write-combine audit compares per submission */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
/* IBs per submission: [0] submissions, [1] IBs, [2] the most in one, [3] submissions with more than 16 (one frame was several before). */
extern "C" {volatile uint32_t native_adreno_submit_ibs[4];}
/* The submission (1-based count) at which the write-combine audit found a dirty IB line or failed; 0 = never. */
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
/* The write-combine audit before each submission while trusted: [0] submissions audited, [1] IB lines compared, [2] dirty lines,
 * [3] stale clean lines, [4] the last audit failure (any failure also ends the trust), [5] pages found back at the default attribute
 * and write-combined again. */
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The CP's state once a fence stopped advancing (NativeAdrenoRing::stallState): [0] the fence, [1] the timed-out waits of it so far, then
 * the ring's words. Taken at the 10th and again at the 1000th timed-out wait of the fence that is the next to complete. native_adreno_stall_mem
 * holds, from the same moment, the SMMU context's FSR, FAR (lo, hi) and FSYNR0, then for the BR IB1, the BV IB1 and the BV IB2 17 words: the
 * dword index in the IB where the 16 words that follow start (the IB1s are found from the ring packet before each read pointer, so the index
 * counts from the IB's start; for the BV IB2 only the fetch address is known and the index is 0xffff0000 + words before it), then the words.
 * The last 4: the map handle that was next when the stalled job was submitted, then the handles of the mappings under the BR IB1, the BV IB1
 * and the BV IB2 (0: unmapped). A handle at or above the first was mapped after the submission: its memory was freed and reused under the GPU. */
extern "C" {volatile uint32_t native_adreno_stall[2+NativeAdrenoRingStallWords];volatile uint32_t native_adreno_stall_mem[4+3*17+4];}
/* 16 words of the IB at start, from 12 dwords before the dword the thread has reached (size minus what remains). */
static void peekIb(const NativeAdrenoVm *vm,uint64_t start,uint32_t size,uint32_t remaining,volatile uint32_t *slot)
{
    uint32_t words[16]={};
    const uint32_t reached=remaining<=size?size-remaining:0,first=reached>12?reached-12:0;
    if(vm->peekWordsUncached(start+4ull*first,words,16))for(unsigned i=0;i<16;++i)words[i]=0xdeadbeef;
    slot[0]=first;
    for(unsigned i=0;i<16;++i)slot[1+i]=words[i];
}
/* 16 words around a fetch address whose IB start is unknown: from 12 words before it, or nearer when that crosses into no buffer. */
static void peekFetch(const NativeAdrenoVm *vm,uint64_t fetch,volatile uint32_t *slot)
{
    uint32_t words[16]={},before=12;
    while(before && vm->peekWordsUncached(fetch-4*before,words,16))before-=4;
    if(!before && vm->peekWordsUncached(fetch,words,16))for(unsigned i=0;i<16;++i)words[i]=0xdeadbeef;
    slot[0]=0xffff0000u+before;
    for(unsigned i=0;i<16;++i)slot[1+i]=words[i];
}
static uint32_t stall_fence,stall_timeouts,stall_stage;   /* stage: 0 none yet, 1 write pointer given again, 2 translations dropped, 3 bus drained */
static uint64_t stall_began_us,stall_last_us;   /* the fence's first and latest timed-out wait */
/* How long fences stayed pending from their first timed-out wait to their last: [i] under 1 << i ms for i < 7, [7] 64 ms or more. */
extern "C" {volatile uint32_t native_adreno_fence_pending[8];}
/* What the stall handler tried: [0] attempts, [1] the fence of the last attempt, [2] attempts after which that fence completed, [3] the
 * timed-out waits that fence had seen when it did. */
/* [4] bus drains, [5] client halt acknowledge, [6] arbiter halt acknowledge, [7] the fence of the last drain, [8] drains after which that fence completed */
extern "C" {volatile uint32_t native_adreno_unstick[15];}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
extern "C" {volatile uint32_t native_adreno_ib_log[4][4+28];volatile uint32_t native_adreno_ib_log_count;}
extern "C" volatile uint32_t native_adreno_table_switches[8];
/* Where the last recover() stopped: [0] the step (1 fence read, 2 restart hook, 3 table write, 4 nop encode, 5 replay vm, 6 replay submit), [1] the IOReturn or value there, [2] fence, [3] last submitted. */
extern "C" {volatile uint32_t native_adreno_recover_trace[4];}
/* The jobs' timeline (the newest native_adreno_tl_cap retired, allocated at attach): [0] fence | indirect buffers << 32 | ring status << 58, [1] submit
 * time (us), [2] the CP's always-on counter before the job's buffers, [3] after them (19.2 MHz ticks; 0 when not stamped), [4] GPU level at submit |
 * the submitting pid << 32, [5] when the driver saw the job retired (us, the submission clock), [6] ring | (priority + 1) << 4 | GPU role << 8 |
 * ring the CP ran at submit << 16 | switch in flight << 20 | restarts that replayed it << 24, [7] when it was last replayed (us, 0 never).
 * native_adreno_tl_count is the total. */
/* sheng-diag=1, set by the service before any client attaches: the driver's diagnostics (the jobs' timeline, the CP stamps) run only then. */
extern "C" {volatile uint32_t native_adreno_diag;}
extern "C" {volatile uint64_t (*native_adreno_tl)[8]=nullptr;volatile uint32_t native_adreno_tl_cap=0;volatile uint32_t native_adreno_tl_count;}
/* The stall handler's actions (the newest 32): [0] time (us), [1] fence, [2] event (1 write pointer given again and translations dropped, 3 bus drain,
 * 4 snapshot, 0 the stall ended) | timed-out waits << 8, [3] time the fence had been pending (us), [4] read and write pointer before | after << 32 for
 * events 1 and 3, [5] client queue, [6] the job's first buffer handle | its buffer count << 48, [7] the sequence number. */
extern "C" {volatile uint64_t native_adreno_stall_log[32][8];volatile uint32_t native_adreno_stall_log_count;}
extern "C" volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    Job **items=IONewData(Job *,jobs_.count+1);
    Job *job=items?new Job:nullptr;
    if(!job) {if(items)IODeleteData(items,Job *,jobs_.count+1);return nullptr;}
    for(size_t i=0;i<jobs_.count;++i)items[i]=jobs_.items[i];
    items[jobs_.count]=job;
    if(jobs_.items)IODeleteData(jobs_.items,Job *,jobs_.count);
    jobs_.items=items;++jobs_.count;
    return job;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;rings_[0]=ring;ring_count_=1;attached_=true;stats_={};table_vm_=vm;
    if(!native_adreno_tl && native_adreno_diag) {   /* the jobs' timeline is a diagnostic (sheng-diag=1) */
        const uint32_t cap=16384;
        volatile uint64_t (*memory)[8]=(volatile uint64_t (*)[8])IOMallocZero(sizeof(uint64_t)*8*cap);
        if(memory) {native_adreno_tl=memory;native_adreno_tl_cap=cap;}
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::setTableSwitching(unsigned mode,IOReturn (*write)(void *,uint64_t),void *cookie)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(mode>2 || (mode && !write) || outstanding())return kIOReturnBadArgument;
    table_mode_=mode;write_table_=write;table_cookie_=cookie;table_vm_=vm_;
    if(mode==2)ring(0)->tableInstalled(vm_->ttbr0());
    return kIOReturnSuccess;
}
/* Makes vm the table the GPU runs for the next submission. Mode 1: once every earlier job has finished, the CPU writes TTBR0 (the bank
 * caches translations per ASID, so the switch needs no TLB operation). Mode 2: the ring's next prologue carries the switch, which the CP
 * runs after every earlier submission. */
IOReturn NativeAdrenoSubmit::useVm(NativeAdrenoVm *vm,const NativeAdrenoRingClock *clock)
{
    if(!table_mode_ || vm==table_vm_)return kIOReturnSuccess;
    if(table_mode_==2) {ring(0)->useTable(vm->ttbr0());table_vm_=vm;return kIOReturnSuccess;}
    if(!clock)return kIOReturnBadArgument;
    IOReturn status=waitAll(clock,2000000);
    if(status)return status;
    status=write_table_(table_cookie_,vm->ttbr0());
    if(status)return poison();
    native_adreno_table_switches[1]=native_adreno_table_switches[1]+1;
    /* The bank now runs this process's table: printed for the first switches so a stalled first submission can be read against it. */
    static uint32_t switch_prints;
    if(switch_prints<6) {
        ++switch_prints;
        printf("PICASSO_GPU table switch: vm=%p ttbr0=%llx\n", vm, (unsigned long long)vm->ttbr0());
    }
    table_vm_=vm;return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::forgetVm(NativeAdrenoVm *vm,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!vm || vm==vm_)return kIOReturnBadArgument;
    IOReturn status=waitAll(clock,timeout_us);
    if(status)return status;
    if(table_vm_!=vm && !(table_mode_==2 && ring(0)->table()==vm->ttbr0()))return kIOReturnSuccess;
    /* The GPU is idle and every queued switch has run: the CPU puts the default table back. */
    status=write_table_?write_table_(table_cookie_,vm_->ttbr0()):kIOReturnNotReady;
    if(status)return poison();
    native_adreno_table_switches[1]=native_adreno_table_switches[1]+1;
    if(table_mode_==2)ring(0)->tableInstalled(vm_->ttbr0());
    table_vm_=vm_;return kIOReturnSuccess;
}
/* The two rings share one fence numbering: the fence's low bits name its ring in a table the switch fills as each ring issues
 * (FenceMap entries; a fence is 32 bits, so the table is indexed by its low bits and checked against the fence it holds). Fences of
 * the first ring alone (no preemption) go to it without a lookup. */
void NativeAdrenoSubmit::noteFence(uint32_t fence,uint8_t index)
{
    if(!fence_tag_) {
        fence_tag_=static_cast<uint32_t *>(IOMallocZero(sizeof(uint32_t)*FenceMap));
        fence_ring_=static_cast<uint8_t *>(IOMallocZero(FenceMap));
        if(!fence_tag_ || !fence_ring_) {
            if(fence_ring_) {IOFree(fence_ring_,FenceMap);fence_ring_=nullptr;}
            if(fence_tag_) {IOFree(fence_tag_,sizeof(uint32_t)*FenceMap);fence_tag_=nullptr;}
            return;
        }
    }
    const uint32_t slot=fence&(FenceMap-1);
    fence_tag_[slot]=fence;fence_ring_[slot]=index;
}
NativeAdrenoRing *NativeAdrenoSubmit::ringOfFence(uint32_t fence) const
{
    if(!preemption())return rings_[0];
    if(!fence_tag_)return nullptr;
    const uint32_t slot=fence&(FenceMap-1);
    if(fence_tag_[slot]==fence) {
        const uint8_t index=fence_ring_[slot];
        return index<ring_count_?rings_[index]:rings_[0];
    }
    /* Older than the table: the fence still names its ring by which ring issued it last (each ring's numbers rise). */
    if((int32_t)(fence-rings_[0]->lastSubmitted())<=0)return rings_[0];
    return ring_count_>1?rings_[1]:rings_[0];
}
/* The ring a submission of this priority goes to: high (0), normal-high (4) and real-time (5) to the first, the rest to the second.
 * A ring alone takes everything. */
unsigned NativeAdrenoSubmit::ringForPriority() const
{
    if(!preemption() || preempt_dead_)return 0;
    /* Only the low and the background levels (2, 3) take the second ring: everything else, the compositor's default included, stays
     * on the ring the CP already runs, so a switch that fails costs the background work and not the screen. */
    const int32_t p=next_priority_;
    return p==2 || p==3 || (preempt_setup_.mode>=2 && p==1)?1:0;
}
/* Whether `handles` shares no buffer with any job outstanding on `index`: the conservative test that lets a queued job overtake the
 * running ring. A job writes what it names (the driver cannot see its binds), so any sharing keeps the order. */
bool NativeAdrenoSubmit::ringDisjoint(unsigned index,const uint64_t *handles,size_t count) const
{
    for(const auto &job:jobs_) {
        if(!job.active || job.ring!=index)continue;
        for(size_t i=0;i<count;++i)
            for(size_t j=0;j<job.count;++j)if(handles[i]==job.handles[j])return false;
    }
    return true;
}
const NativeAdrenoSubmit::Job *NativeAdrenoSubmit::pendingOn(unsigned index)
{
    NativeAdrenoRing *const r=ring(index);
    uint32_t done=0;
    if(!r || r->fenceValue(&done))return nullptr;
    const Job *first=nullptr;
    for(const auto &job:jobs_)
        if(job.active && job.ring==index && (int32_t)(job.fence-done)>0 && (!first || (int32_t)(job.fence-first->fence)<0))first=&job;
    return first;
}
/* Moves the CP to the other ring when that ring has work queued, its buffers do not overlap the running ring's, and the CP is not
 * already switching. msm's a6xx_preempt_trigger, with the ring choice made by priority instead of round-robin. */
void NativeAdrenoSubmit::driveSwitch(const NativeAdrenoRingClock *clock)
{
    (void)clock;
    if(!preemption())return;
    /* A switch the interrupt saw finish (or that finished while its handler could not take the lock) is taken first. */
    if(preempt_.inFlight()) {
        if(!preemptIrq()) {
            /* The CP never finished a switch: no more are tried (the restart in recover() brings the CP back). */
            if(preempt_.timedOut(PreemptSwitchTimeoutUs)) {
                uint32_t progress[6]={};
                (void)ring(0)->progressState(progress);
                preempt_.noteTimeout(progress);
                markPreemptDead(2);
            }
            return;
        }
    }
    if(preempt_dead_)return;
    const uint32_t loaded=preempt_.loaded();
    /* Ring 0 is the high ring: while it has work the CP does not leave it for the low ring, and it takes the CP back from the low ring as
     * soon as it has some. The low ring's work still gets the GPU when the high ring is idle, and after it has waited PreemptAgingUs. */
    const Job *const high=pendingOn(0);
    for(unsigned i=0;i<ring_count_;++i) {
        if(i==loaded)continue;
        /* The oldest job waiting on `i` (one the ring's CP has not finished) and whether its buffers are clear of the running ring's. */
        const Job *const first=pendingOn(i);
        if(!first)continue;
        if(i!=0 && high) {
            const uint64_t now=clock_now_?clock_now_(clock_cookie_):0;
            if(!(now && first->submit_us && now-first->submit_us>PreemptAgingUs))continue;
        }
        if(!ringDisjoint(loaded,first->handles,first->count)) {preempt_.noteDisjointSkip();continue;}
        /* The incoming ring's preemption record keeps what it was given at setup and what the CP saved into it when it switched the
         * ring out (read pointer, info, errno); only the write pointer the CP resumes at is given again before every switch. */
        NativeAdrenoRing *const incoming=ring(i);
        if(!incoming || !preempt_setup_.pages.record_cpu[i] || !preempt_setup_.pages.smmu_cpu[i])continue;
        (void)incoming->writePreemptWptr(preempt_setup_.pages.record_cpu[i]);
        /* The page table the CP must run when it arrives: the waiting job's own VM (each process has one). Its ASID rides in
         * the TTBR0's top word, as NativeAdrenoVm::ttbr0() gives it. */
        const uint64_t ttbr0=first->vm?first->vm->ttbr0():table_vm_->ttbr0();
        (void)NativeAdrenoPreempt::writeSmmuInfo(preempt_setup_.pages.smmu_cpu[i],ttbr0,uint32_t(ttbr0>>48),0);
        if(preempt_.trigger(i,false)) {ring(0)->setCurrent(false);ring(1)->setCurrent(false);return;}
        /* The trigger itself refused (a register write failed): the second ring cannot be reached, so it is given up at once and the
         * jobs already queued on it are replayed by the restart the hang check asks for. */
        preempt_.noteSkipped();
        markPreemptDead(1);
        return;
    }
}
/* The interrupt's side: a completed switch makes its ring the loaded one and gives that ring's write pointer. */
bool NativeAdrenoSubmit::preemptIrq()
{
    if(!preemption() || !preempt_.inFlight())return false;
    if(!preempt_.complete())return false;
    NativeAdrenoRing *const now=ring(preempt_.loaded());
    for(unsigned i=0;i<ring_count_;++i)ring(i)->setCurrent(false);
    if(now) {
        now->setCurrent(true);
        if(now->restorePending()) {if(!now->restoreWptr())preempt_.noteRestoredWptr();}
        else {
            /* The ring that was not current appended its words without moving CP_RB_WPTR; the switch's arrival is the moment to
             * give it, whether or not it queued anything since the trigger. */
            (void)now->restoreWptr();
            preempt_.noteRestoredWptr();
        }
    }
    return true;
}
IOReturn NativeAdrenoSubmit::setPreemption(NativeAdrenoRing *high,NativeAdrenoRing *low,const PreemptSetup &setup,bool on)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(outstanding())return kIOReturnBusy;
    if(!on) {preempt_on_=false;ring_count_=1;rings_[1]=nullptr;return kIOReturnSuccess;}
    if(!high || !low || high==low)return kIOReturnBadArgument;
    if(!high->preemption() || !low->preemption())return kIOReturnBadArgument;
    for(unsigned i=0;i<NativeAdrenoPreemptRingCount;++i)
        if(!setup.pages.record_cpu[i] || !setup.pages.smmu_cpu[i] ||
           !setup.pages.record[i] || !setup.pages.smmu_info[i])return kIOReturnBadArgument;
    /* Each ring's SMMU info and record, filled once here (the switch rewrites them before every move). */
    for(unsigned i=0;i<NativeAdrenoPreemptRingCount;++i) {
        NativeAdrenoRing *const r=i==0?high:low;
        const uint64_t ttbr0=table_vm_->ttbr0();
        if(!NativeAdrenoPreempt::writeSmmuInfo(setup.pages.smmu_cpu[i],ttbr0,uint32_t(ttbr0>>48),0))return kIOReturnBadArgument;
        (void)r->writePreemptRecord(setup.pages.record_cpu[i]);
    }
    rings_[0]=high;rings_[1]=low;ring_count_=2;
    high->setCurrent(true);low->setCurrent(false);
    preempt_setup_=setup;
    const IOReturn attached_preempt=preempt_.attach(&setup.io,setup.pages,0,setup.now,setup.clock);
    if(attached_preempt) {ring_count_=1;rings_[1]=nullptr;return attached_preempt;}
    preempt_on_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    NativeAdrenoVm *vm=job.vm?job.vm:vm_;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    if(vm->acquireImported())return poison();
    {
        uint64_t start=0,end=0;
        NativeAdrenoRing *r=ringOfFence(job.fence);
        const uint32_t stamped=r?r->timeline(job.fence,&start,&end):0;
        if(native_adreno_tl_cap) {
            const uint32_t n=native_adreno_tl_count;volatile uint64_t *slot=native_adreno_tl[n&(native_adreno_tl_cap-1)];
            slot[0]=(uint64_t)job.fence|(uint64_t)job.count<<32|(uint64_t)stamped<<58;slot[1]=job.submit_us;
            slot[2]=start;slot[3]=end;slot[4]=(uint64_t)job.level|(uint64_t)(uint32_t)job.owner<<32;
            slot[5]=clock_now_?clock_now_(clock_cookie_):0;
            slot[6]=(uint64_t)job.ring|(uint64_t)uint8_t(job.priority+1)<<4|(uint64_t)job.role<<8|(uint64_t)job.loaded<<16|
                (uint64_t)job.flight<<20|(uint64_t)job.replays<<24;
            slot[7]=job.replay_us;
            native_adreno_tl_count=n+1;
        }
    }
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint64_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint64_t first,uint64_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint64_t queue,uint32_t *fence)
{
    return submit(nullptr,buffers,count,queue,fence,nullptr);
}
IOReturn NativeAdrenoSubmit::submit(NativeAdrenoVm *vm,const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint64_t queue,uint32_t *fence,const NativeAdrenoRingClock *clock)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!vm)vm=vm_;
    NativeAdrenoVm *const vm_=vm;   /* this submission's VM: every vm_ below is the job's own */
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    /* msm_ioctl_gem_submit refuses a VM a hang made unusable. */
    if(vm->unusable())return kIOReturnNotPermitted;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    native_adreno_submit_ibs[0]=native_adreno_submit_ibs[0]+1;native_adreno_submit_ibs[1]=native_adreno_submit_ibs[1]+(uint32_t)count;
    if(count>native_adreno_submit_ibs[2])native_adreno_submit_ibs[2]=(uint32_t)count;
    if(count>16)native_adreno_submit_ibs[3]=native_adreno_submit_ibs[3]+1;
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    static_assert(sizeof(scratch_words_)/sizeof(scratch_words_[0])>=SubmitMaxBuffers*(1+SubmitIbWords),"ring words");
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    IOReturn swept=retireCompleted();
    if(swept)return swept;
    Job *job=free();
    if(!job)return kIOReturnNoMemory;
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    uint32_t value=0;
    /* int-070: the first IB word as the kernel's cacheable alias and DRAM see it before any maintenance, then DRAM after the CPU
     * caches over every mapped BO are cleaned and invalidated. The board shows the last submission's record on a stall. */
    uint32_t cached=~0u,before=~0u,ignored=0,after=~0u;
    /* Write-combined stores this CPU made before the call (the client's command words) complete before anything below. */
    __asm__ volatile("dsb sy":::"memory");
    /* The first few submissions print the wire buffers as the client sent them and what the memory holds after that barrier, before the
     * ring is written: the board's own probe submissions are the control (their IB is in a board buffer). Bounded. */
    static uint32_t wire_prints;
    if(wire_prints<8 && count) {
        ++wire_prints;
        for(size_t i=0;i<count && i<2;++i) {
            const NativeAdrenoSubmitBuffer &buffer=buffers[i];
            uint64_t miova=0;size_t mbytes=0;unsigned permissions=0;
            const IOReturn described=vm_->describe(buffer.handle,&miova,&mbytes,&permissions);
            uint32_t cachedWord=0,dramWordV=0;
            const IOReturn sampled=vm_->sampleWord(buffer.iova,&cachedWord,&dramWordV);
            uint32_t head[8]={};
            const IOReturn read=vm_->peekWordsUncached(miova,head,8);
            printf("PICASSO_GPU wire: pid=%d vm=%p next=%llu h=%llu iova=%llx bytes=%llu map=", proc_selfpid(),
                vm_, (unsigned long long)vm_->nextHandle(), (unsigned long long)buffer.handle,
                (unsigned long long)buffer.iova, (unsigned long long)buffer.bytes);
            if(described)printf("%x",unsigned(described));
            else printf("%llx/%llu",(unsigned long long)miova,(unsigned long long)mbytes);
            printf(" sample=%x/%x/%x bo=", unsigned(sampled), cachedWord, dramWordV);
            if(read)printf("%x",unsigned(read));
            else for(uint32_t k=0;k<8;++k)printf(" %x", head[k]);
            printf("\n");
        }
        /* Where this process's command stream is, if its named buffer reads zero: every mapping and that mapping's own first words. */
        if(wire_prints<=4)vm_->dumpMappings("wire",24);
        /* The whole submitted buffer, and the whole address space, through the truly uncached DRAM reader: whether the client's
         * words are anywhere at all (an offset, another BO) or nowhere (never stored, or still in a write-combine buffer). The
         * Turnip prologue's first word (CP_EVENT_WRITE cache-invalidate) is the needle. Bounded and client submissions only. */
        static uint32_t scan_prints;
        if(scan_prints<2 && proc_selfpid()!=0) {
            ++scan_prints;
            uint64_t scanned=0,nz=0,first_off=0,needle_off=UINT64_MAX;uint32_t first_word=0;
            if(vm_->scanMapping(buffers[0].handle,0x70460001,65536,&scanned,&nz,&first_off,&first_word,&needle_off))
                printf("PICASSO_GPU scan wire: h=%llu scanned=%llu nz=%llu first=%llx:%x needle=70460001 at=%llx\n",
                    (unsigned long long)buffers[0].handle,(unsigned long long)scanned,(unsigned long long)nz,
                    (unsigned long long)first_off,first_word,(unsigned long long)needle_off);
            uint64_t sh=0,soff=0;uint32_t sw=0;
            const uint32_t hits=vm_->scanVm(0x70460001,65536,512*1024,&sh,&soff,&sw);
            printf("PICASSO_GPU scan vm: needle=70460001 hits=%u h=%llu off=%llx word=%x\n",hits,
                (unsigned long long)sh,(unsigned long long)soff,sw);
        }
    }
    /* int-098/099: with write-combined user BOs and no maintenance the CP stopped at the end of the first IB, as in int-065..069 when an
     * IB sat in a CPU cache. While write-combined BOs are trusted, every line of this submission's IBs is compared as a CPU cache and
     * DRAM hold it (bounded). A dirty line means a client view still caches its BOs: maintain every BO from now on, this submission
     * included (the audit already wrote the IB lines back; the rest of the BOs may hold more). */
    if(vm_->writeCombinedTrusted()) {
        /* int-100: the dirty IB page was back at XNU's default (cacheable) attribute. Write-combine every such page again first; the
         * pmap writes its lines back on the way, so the audit below should then find none. */
        uint32_t reset=0;
        IOReturn audited=vm_->reassertWriteCombined(&reset);
        native_adreno_wc_audit[5]=native_adreno_wc_audit[5]+reset;
        uint32_t lines=0,dirty=0,stale=0;size_t budget=SubmitAuditBytes;
        /* The re-assertion above is the correction and runs every time; the audit only detects. It found no dirty line in int-141's
         * 133798 nor in i566's 2765132 (1358 audits of every eighth submission, about 2 ms each under the publish lock, so every eighth
         * frame of every client waited), so only the first 16 submissions are compared now. */
        ++audit_tick_;
        const bool audit_now=audit_tick_<=16;
        if(!audit_now)budget=0;
        for(size_t i=0;i<count && budget && !audited;++i) {   /* a failed re-assertion skips the audit and ends the trust */
            const size_t bytes=buffers[i].bytes<budget?(size_t)buffers[i].bytes:budget;
            audited=vm_->auditRange(buffers[i].iova,bytes,&lines,&dirty,&stale);budget-=bytes;
        }
        if(audit_now && audited!=kIOReturnUnsupported) {   /* no board hook (host): nothing to compare */
            native_adreno_wc_audit[0]=native_adreno_wc_audit[0]+1;
            native_adreno_wc_audit[1]=native_adreno_wc_audit[1]+lines;
            native_adreno_wc_audit[2]=native_adreno_wc_audit[2]+dirty;
            native_adreno_wc_audit[3]=native_adreno_wc_audit[3]+stale;
            if(audited)native_adreno_wc_audit[4]=audited;
        }
        if(dirty || (audited && audited!=kIOReturnUnsupported)) {
            vm_->distrustWriteCombined();
            native_adreno_wc_distrusted=native_adreno_coherence[0]+1;
        }
    }
    /* The coherence sample reads the first IB's word through a fresh uncached mapping before and after the publication: every 64th
     * submission only (two such mappings per submission were CPU time on every frame). */
    static uint32_t coherence_turn;
    const bool sample=!(coherence_turn++&63);
    if(sample)(void)vm_->sampleWord(buffers[0].iova,&cached,&before);
    IOReturn status=vm_->publishMapped();
    if(!status && sample) {
        (void)vm_->sampleWord(buffers[0].iova,&ignored,&after);
        native_adreno_coherence[0]=native_adreno_coherence[0]+1;
        native_adreno_coherence[1]=cached;native_adreno_coherence[2]=before;native_adreno_coherence[3]=after;
        if(cached!=before || before!=after) {
            if(!native_adreno_coherence[4]) {
                native_adreno_coherence[5]=cached;native_adreno_coherence[6]=before;native_adreno_coherence[7]=after;
            }
            native_adreno_coherence[4]=native_adreno_coherence[4]+1;
        }
    }
    /* No TLB invalidation before a submission: a map fills only invalid leaves (no TLB holds an invalid descriptor), an unmap drops
     * its own pages before it returns, and the tables above the leaves never change. The TLBIALL here emptied the GPU's TLB on every
     * submission (10944 in i568), so each frame began with page walks. A stalled fence still invalidates (below). */
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    /* The ring this job goes to (the first ring alone unless the board turned the switch on). */
    const unsigned index=preemption()?ringForPriority():0;
    NativeAdrenoRing *const target=ring(index);
    {
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[28];
        (void)vm_->peekWords(buffers[0].iova,head,28);
        native_adreno_ib_log[slot][0]=target->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        for(unsigned i=0;i<28;++i)native_adreno_ib_log[slot][4+i]=head[i];
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    status=useVm(vm,clock);
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    /* One numbering for both rings: a fence tells its own ring, and a switch's arrival retires the jobs queued behind it. */
    /* The board's earlier submissions (the CP init and the probes) numbered themselves on the first ring: the next number is above
     * every ring's newest, and above the last this object handed out. */
    uint32_t base=fence_issued_;
    for(unsigned i=0;i<ring_count_;++i)if((int32_t)(ring(i)->lastSubmitted()-base)>0)base=ring(i)->lastSubmitted();
    const uint32_t number=(value=base)+1;
    status=a6xx_?target->submit(words,word_count,&value):
        preemption()?target->submitFramed(words,word_count,number,&value):target->submitFramed(words,word_count,&value);
    if(status) {
        if(!value)fence_issued_=base;
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(target->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;job->vm_next=vm_->nextHandle();job->vm=vm;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        /* No room in the ring yet: the caller waits for an earlier job and submits again. */
        return status==kIOReturnNoResources?kIOReturnBusy:status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;job->vm_next=vm_->nextHandle();job->vm=vm;
    job->context=target->context();job->word_count=word_count;job->ring=uint8_t(index);
    job->priority=int8_t(next_priority_);job->role=uint8_t(next_role_);
    job->loaded=uint8_t(preemption()?preempt_.loaded():0);job->flight=preemption() && preempt_.inFlight()?1:0;
    if(preemption()) {fence_issued_=value;noteFence(value,uint8_t(index));}
    job->submit_us=clock && clock->now_us?clock->now_us(clock->cookie):0;
    if(clock && clock->now_us) {clock_now_=clock->now_us;clock_cookie_=clock->cookie;}
    job->level=&native_gpu_dcvs?native_gpu_dcvs[1]:0;
    job->owner=next_owner_;next_owner_=0;
    for(size_t i=0;i<word_count;++i)job->words[i]=words[i];
    ++stats_.submissions;
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    *fence=value;
    driveSwitch(clock);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    NativeAdrenoRing *const r=ringOfFence(fence);
    if(!r)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=r->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    return kIOReturnSuccess;
}
/* A stage runs once per stalled fence: when the stage before it has run and the fence has been pending long enough. */
static bool stage_due(uint32_t stage,uint32_t before,uint64_t pending,uint64_t after_us)
{
    return stage==before && pending>=after_us;
}
/* The CP snapshot is taken at 50 ms and at 1 s of one stall. */
static uint32_t stall_snapshots;
static bool snapshot_due(uint32_t *taken,uint64_t pending)
{
    const uint32_t want=pending>=1000000?2:pending>=50000?1:0;
    if(want<=*taken)return false;
    *taken=want;return true;
}
void NativeAdrenoSubmit::noteStall(uint32_t observed,const NativeAdrenoRingClock *clock)
{
    const uint32_t next=observed+1;   /* the fence the CP is working on */
    const uint64_t now=clock && clock->now_us?clock->now_us(clock->cookie):0;
    /* The ring that fence's job ran on; the first ring when the fence is not one of this submit object's. */
    NativeAdrenoRing *const stalled=ringOfFence(next)?ringOfFence(next):ring(0);
    auto log=[&](uint32_t fence,uint32_t event,uint64_t pending,uint32_t before_r,uint32_t before_w) {
        const uint32_t n=native_adreno_stall_log_count;volatile uint64_t *slot=native_adreno_stall_log[n&31];
        const Job *owner=find(fence);
        slot[0]=now;slot[1]=fence;slot[2]=(uint64_t)event|(uint64_t)stall_timeouts<<8;slot[3]=pending;
        slot[4]=(uint64_t)before_r|(uint64_t)before_w<<16|(uint64_t)stalled->readPointer()<<32|(uint64_t)(stalled->writePointer()&0xffff)<<48;
        slot[5]=owner?owner->queue:0;slot[6]=owner?(owner->handles[0]&0xffffffffffffull)|(uint64_t)owner->count<<48:0;slot[7]=n;
        native_adreno_stall_log_count=n+1;
    };
    if(next!=stall_fence) {
        if(stall_timeouts && now) {
            const uint64_t ms=(stall_last_us-stall_began_us)/1000;
            uint32_t bucket=0;while(bucket<7 && ms>=(1ull<<bucket))++bucket;
            native_adreno_fence_pending[bucket]=native_adreno_fence_pending[bucket]+1;
            if(stall_last_us-stall_began_us>=1000)log(stall_fence,0,stall_last_us-stall_began_us,0,0);   /* shorter ends are ordinary jobs */
        }
        stall_began_us=now;stall_snapshots=0;
        /* The CP moved on: what the stall that just ended took to clear. [9] first-stage recoveries, [10] second, [11] third, [12] timeouts of the longest. */
        if(stall_stage && stall_timeouts) {
            native_adreno_unstick[8+stall_stage]=native_adreno_unstick[8+stall_stage]+1;
            if(stall_timeouts>native_adreno_unstick[12])native_adreno_unstick[12]=stall_timeouts;
        }
        stall_fence=next;stall_timeouts=0;stall_stage=0;
    }
    stall_last_us=now;
    const uint64_t pending=now-stall_began_us;
    /* A fence that stops while the CP sits idle with work in the ring (int-317 hung with RBBM idle, RPTR short of WPTR, both threads at the same
     * cache clean) is nearly always a write pointer the CP never took: giving it again after a few waits ends it in a couple of milliseconds
     * (i343: 206 of 213 stalls ended at the second stage, 60 to 85 ms each, most of it the 100 waits before the write). After 10 waits now:
     * i469 stalled 143 times in about 200 s, each a visible hitch. The same value written again changes nothing when the CP is busy. */
    /* After 10 waits the translations are dropped and the write pointer is given again. i494/i495 measured what ends these: a job lasts about
     * 5 ms against 1 ms waits, so most "stalls" are ordinary jobs still running; the write pointer is never dropped (fenced writes saw no
     * drop) and invalidating at map time changed nothing. i553: the handler ran on 6712 of 6911 jobs. It now waits for 50 ms without
     * progress, measured in time (a timed-out wait is one poll, 100 us or less), and so does the snapshot below. */
    if(now && stage_due(stall_stage,0,pending,50000)) {
        native_adreno_unstick[0]=native_adreno_unstick[0]+1;native_adreno_unstick[1]=stall_fence;stall_stage=1;
        const uint32_t r0=stalled->readPointer(),w0=stalled->writePointer()&0xffff;
        (void)vm_->invalidateTlb();(void)stalled->kick();
        log(stall_fence,1,pending,r0&0xffff,w0);
    }
    /* Still stopped well after that: the way a6xx drains the bus before a reset, which lets a write that was never acknowledged end. */
    if(now && stage_due(stall_stage,1,pending,200000)) {
        uint32_t ack[2]={};
        native_adreno_unstick[4]=native_adreno_unstick[4]+1;native_adreno_unstick[7]=stall_fence;stall_stage=3;
        const uint32_t r0=stalled->readPointer(),w0=stalled->writePointer()&0xffff;
        (void)stalled->drainBus(ack);
        native_adreno_unstick[5]=ack[0];native_adreno_unstick[6]=ack[1];
        log(stall_fence,3,pending,r0&0xffff,w0);
    }
    ++stall_timeouts;
    if(!now || !snapshot_due(&stall_snapshots,pending))return;
    uint32_t words[NativeAdrenoRingStallWords]={},fault[4]={};
    if(stalled->stallState(words))return;
    log(next,4,pending,words[8]&0xffff,words[9]&0xffff);
    (void)vm_->faultState(fault);
    for(uint32_t i=0;i<4;++i)native_adreno_stall_mem[i]=fault[i];
    uint64_t ib=0;uint32_t size=0;
    /* The IBs are the stalled job's: read them through its address space (the kernel's maps none of a client's BOs). */
    const Job *job=find(next);
    const NativeAdrenoVm *space=job && job->vm?job->vm:vm_;
    if(!stalled->ibBefore(words[8],&ib,&size))peekIb(space,ib,size,words[12],&native_adreno_stall_mem[4]);
    else peekFetch(space,(uint64_t)words[11]<<32|words[10],&native_adreno_stall_mem[4]);
    if(!stalled->ibBefore(words[16],&ib,&size))peekIb(space,ib,size,words[18],&native_adreno_stall_mem[4+17]);
    else peekFetch(space,(uint64_t)words[11]<<32|words[17],&native_adreno_stall_mem[4+17]);   /* BV IB1 base: the low word only */
    peekFetch(space,(uint64_t)words[21]<<32|words[20],&native_adreno_stall_mem[4+34]);
    {
        uint64_t brIb=(uint64_t)words[11]<<32|words[10],bvIb=(uint64_t)words[11]<<32|words[17];
        if(!stalled->ibBefore(words[8],&ib,&size))brIb=ib;
        if(!stalled->ibBefore(words[16],&ib,&size))bvIb=ib;
        native_adreno_stall_mem[55]=job?(uint32_t)job->vm_next:0;
        native_adreno_stall_mem[56]=(uint32_t)space->handleAt(brIb);
        native_adreno_stall_mem[57]=(uint32_t)space->handleAt(bvIb);
        native_adreno_stall_mem[58]=(uint32_t)space->handleAt((uint64_t)words[21]<<32|words[20]);
    }
    for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)native_adreno_stall[2+i]=words[i];
    native_adreno_stall[1]=stall_timeouts;
    native_adreno_stall[0]=next;
    /* The first few stalled fences print the CP and SMMU state directly: a client's first submission on its own page table either
     * never reaches the CP (RPTR behind WPTR, no fault) or faults on an address the table did not map (FSR/FAR below). Bounded, so a
     * fence that never advances cannot flood the console. */
    static uint32_t stall_prints;
    if(stall_prints<6) {
        ++stall_prints;
        uint32_t cpFence=0;
        (void)stalled->fenceValue(&cpFence);
        printf("PICASSO_GPU stall: fence=%u pending=%llu us cpFence=%u rptr=%u wptr=%u\n", next,
            (unsigned long long)pending, cpFence, stalled->readPointer(), stalled->writePointer());
        printf("PICASSO_GPU stall cp:");
        for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)printf(" %x", words[i]);
        printf("\nPICASSO_GPU stall smmu: %x %x %x %x job=%x\n", fault[0], fault[1], fault[2], fault[3], job ? job->vm_next : 0);
        /* The board's own fault registers: CP_INTERRUPT_STATUS (0x823) names an SQE error the ring's stall state does not read,
         * CP_HW_FAULT, CP_PROTECT_STATUS and one CP_SQE_STAT sample. Absent on the host. */
        if(native_adreno_cp_fault_regs) {
            uint32_t regs[4]={0xffffffff,0xffffffff,0xffffffff,0xffffffff};
            (void)native_adreno_cp_fault_regs(regs,4);
            printf("PICASSO_GPU stall cpregs: cpint=%x hwfault=%x protect=%x sqe=%x\n", regs[0],regs[1],regs[2],regs[3]);
        }
        if(job) {
            printf("PICASSO_GPU stall job: count=%u words=%u:", job->count, job->word_count);
            for(uint32_t i=0;i<job->word_count && i<32;++i)printf(" %x", job->words[i]);
            printf("\nPICASSO_GPU stall ib:");
            for(uint32_t i=0;i<17;++i)printf(" %x", native_adreno_stall_mem[4+i]);
            printf("\n");
            /* peekFetch gives up when the words 12 dwords below the fetch address lie outside the BO (an IB that starts
             * at a page boundary); read the fetch address itself, which is inside the mapping. */
            const uint64_t ib1=(uint64_t)words[11]<<32|words[10];
            uint32_t head[8]={};
            const IOReturn read=space->peekWordsUncached(ib1,head,8);
            printf("PICASSO_GPU stall ib1 %llx read=%x:", (unsigned long long)ib1, unsigned(read));
            if(!read)for(uint32_t i=0;i<8;++i)printf(" %x", head[i]);
            printf("\n");
            /* Where inside IB1 the SQE is *executing*. CP_IB1_REM_SIZE counts only dwords not yet fetched into the ROQ: the
             * front-end prefetches ahead and REM stops once the ROQ is full (a6xx_progress), so a REM-only position is where the
             * *fetch* stopped, not where the SQE is. progressState adds CP_ROQ_AVAIL_IB1's fetched-but-unconsumed dwords; the
             * packet the SQE is on is base + 4*(size - effective_rem). Dump 24 dwords on each side through the job's address
             * space: a wait packet here names the condition the CP is stuck on. */
            {
                const uint64_t base=(uint64_t)words[11]<<32|words[10];
                uint32_t total=0;
                for(uint32_t i=0;i+3<job->word_count;i+=4)
                    if(((uint64_t)job->words[i+2]<<32|job->words[i+1])==base) {total=job->words[i+3]&0xfffff;break;}
                uint32_t st[6]={};
                const IOReturn ps=stalled->progressState(st);
                printf("PICASSO_GPU stall ibnow: base=%llx size=%x rem=%x roq=%x effrem=%x ps=%x",
                    (unsigned long long)base,total,words[12],(unsigned)(st[4]>words[12]?st[4]-words[12]:0),st[4],unsigned(ps));
                if(total && ps==kIOReturnSuccess && st[4]<=total) {
                    const uint64_t at=base+4ull*(total-st[4]);
                    uint32_t now[48]={};
                    const uint64_t start=at>=base+96?at-96:base;
                    const IOReturn nowread=space->peekWordsUncached(start,now,48);
                    printf(" at=%llx read=%x:", (unsigned long long)at, unsigned(nowread));
                    if(!nowread)for(uint32_t i=0;i<48;++i)printf(" %x", now[i]);
                }
                printf("\n");
            }
            /* The packets the SQE executed in this IB up to where it stopped, with their positions: where the client's
             * stream spends its synchronization (waits, markers, events, blits, context bunches, IB calls). The stream is
             * read forward in chunks and only the last lines are kept, so a long blit prefix does not crowd out the packets
             * next to the stop: the marker there names the phase, and an unsatisfied wait packet names the condition. */
            if(stall_prints<=2) {
                const uint64_t base=(uint64_t)words[11]<<32|words[10];
                uint32_t total=0;
                for(uint32_t i=0;i+3<job->word_count;i+=4)
                    if(((uint64_t)job->words[i+2]<<32|job->words[i+1])==base) {total=job->words[i+3]&0xfffff;break;}
                uint32_t st[6]={};
                if(!stalled->progressState(st) && total && st[4]<=total) {
                    struct TraceLine {size_t at;const char *nm;uint32_t cnt,p[4];};
                    TraceLine tail[32]={};unsigned tailn=0;
                    uint32_t nblit=0,nwait=0,nevent=0,nmarker=0,ncrb=0,nib=0,ndraw=0,ncond=0,nskip=0;
                    const size_t limit=(size_t)(total-st[4]);
                    size_t pos=0;
                    uint32_t w[256];
                    while(pos<limit) {
                        size_t n=limit-pos;if(n>256)n=256;
                        if(space->peekWordsUncached(base+4ull*pos,w,n))break;
                        size_t i=0;bool crossed=false;
                        while(i<n) {
                            const uint32_t h=w[i];
                            const char *nm=nullptr;uint32_t cnt=0;
                            size_t adv=1;
                            if((h>>28)==7) {
                                const uint32_t op=(h>>16)&0x7f;cnt=h&0x7fff;adv=1+(size_t)cnt;
                                switch(op) {
                                case 0x12:nm="WAIT_MEM_WRITES";++nwait;break;
                                case 0x26:nm="WAIT_IDLE";++nwait;break;
                                case 0x3c:nm="WAIT_REG_MEM";++nwait;break;
                                case 0x14:nm="WAIT_TS";++nwait;break;
                                case 0x46:nm="EVENT";++nevent;break;
                                case 0x2c:nm="BLIT";++nblit;break;
                                case 0x3f:nm="IB";++nib;break;
                                case 0x5c:nm="CRB";++ncrb;break;
                                case 0x65:nm="MARKER";++nmarker;break;
                                case 0x1d:nm="SKIP_IB2_G";++nskip;break;
                                case 0x23:nm="SKIP_IB2_L";++nskip;break;
                                case 0x47:nm="COND_EXEC";++ncond;break;
                                case 0x38:nm="DRAW_INDX_OFF";++ndraw;break;
                                case 0x29:nm="DRAW_INDX_IND";++ndraw;break;
                                case 0x2a:nm="DRAW_IND_MULTI";++ndraw;break;
                                case 0x28:nm="DRAW_IND";++ndraw;break;
                                case 0x24:nm="DRAW_AUTO";++ndraw;break;
                                case 0x33:nm="EXEC_CS";++ndraw;break;
                                case 0x63:nm="SET_MODE";break; case 0x64:nm="VIS_OVR";break;
                                case 0x3e:nm="REG_TO_MEM";break; case 0x3d:nm="MEM_WRITE";break;
                                case 0x56:nm="PSEUDO";break; case 0x5f:nm="PROT_MODE";break;
                                case 0x53:nm="TTBR0";break; case 0x2e:nm="BIN_DATA";break;
                                }
                            } else if((h>>28)==4) {
                                adv=1+(size_t)(h&0x7f);
                            }
                            if(i+adv>n) {crossed=true;break;}
                            if(nm) {
                                TraceLine t;t.at=pos+i;t.nm=nm;t.cnt=cnt;
                                for(uint32_t k=0;k<4&&k<cnt;++k)t.p[k]=w[i+1+k];
                                if(tailn<32)tail[tailn++]=t;
                                else {for(unsigned q=1;q<32;++q)tail[q-1]=tail[q];tail[31]=t;}
                            }
                            i+=adv;
                        }
                        if(i==0 && crossed) {
                            /* A packet longer than the chunk: skip over its payload. */
                            const uint32_t h=w[0];
                            pos+=((h>>28)==7)?1+(size_t)(h&0x7fff):((h>>28)==4)?1+(size_t)(h&0x7f):1;
                            continue;
                        }
                        pos+=i;
                    }
                    printf("PICASSO_GPU trace: ib=%llx executed=%lx blit=%u draw=%u wait=%u event=%u marker=%u crb=%u ib=%u cond=%u skip=%u\n",
                        (unsigned long long)base,(unsigned long)limit,nblit,ndraw,nwait,nevent,nmarker,ncrb,nib,ncond,nskip);
                    for(unsigned q=0;q<tailn;++q) {
                        const TraceLine &t=tail[q];
                        printf("PICASSO_GPU trace %04lx %s n=%x:",(unsigned long)t.at,t.nm,t.cnt);
                        for(unsigned k=0;k<4&&k<t.cnt;++k)printf(" %x",t.p[k]);
                        printf("\n");
                    }
                }
            }
            /* A 2D blit's setup names its source/destination buffers in pkt4 writes (GRAS/RB/TPL1 A2D) that can sit in an earlier
             * chunk of this same submission. Walk the job's IBs (bounded) and print them: a blit engine that wedged needs its
             * addresses, formats and pitches on record, and the VM's own view of each base address. */
            {
                static const struct {uint32_t lo,hi;} ranges[]={{0x8400,0x840f},{0x8c17,0x8c27},{0xb4c0,0xb4cf}};
                uint32_t printed=0;
                for(uint32_t i=0;i+3<job->word_count && printed<24;i+=4) {
                    const uint64_t iba=(uint64_t)job->words[i+2]<<32|job->words[i+1];
                    size_t ibd=job->words[i+3]&0xfffff;
                    if(ibd>1024)ibd=1024;
                    uint32_t w[1024]={};
                    if(space->peekWordsUncached(iba,w,ibd))continue;
                    for(size_t at=0;at<ibd && printed<24;) {
                        const uint32_t hdr=w[at];
                        if((hdr>>28)==4) {
                            const uint32_t reg=(hdr>>8)&0x3ffff,n=hdr&0x7f;
                            for(const auto &rg:ranges)if(reg>=rg.lo && reg<=rg.hi) {
                                printf("PICASSO_GPU stall a2d: ib=%llx at=%llx reg=%x n=%u:",
                                    (unsigned long long)iba,(unsigned long long)at,reg,n);
                                for(uint32_t k=0;k<n && at+1+k<ibd;++k)printf(" %x",w[at+1+k]);
                                printf("\n");
                                ++printed;break;
                            }
                            at+=1+n;
                        } else if((hdr>>28)==7)at+=1+(hdr&0x3fff);
                        else at+=1;
                    }
                }
            }
            /* Validate the DRAM reader against a page whose content is known: the fence page's completion timestamp holds the fence the CP
             * wrote (2 here). A zero there would mean the reader, not the IB page, is wrong. And name the mapping the IB address falls in. */
            uint32_t cachedWord=0,dramWordV=0;
            const IOReturn sampled=space->sampleWord(stalled->fenceIova()+4,&cachedWord,&dramWordV);
            printf("PICASSO_GPU stall fencepage: read=%x cached=%x dram=%x handle=%llx\n", unsigned(sampled),
                cachedWord, dramWordV, (unsigned long long)space->handleAt(ib1));
            /* Which process and address space the stalled job belongs to: two connections allocate the same handles and IOVAs. */
            printf("PICASSO_GPU stall ids: pid=%d fence=%u jvm=%p vm=%p space=%p snext=%llu buf0=%llu\n", job->owner,
                job->fence, job->vm, vm_, space, (unsigned long long)space->nextHandle(),
                (unsigned long long)(job->count?job->handles[0]:0));
            /* The buffer the job itself named, while its mapping still exists: the ring's IB address reads whatever is mapped there
             * now, which after a free and reuse would be another BO. */
            uint64_t bufiova=0;size_t bufbytes=0;unsigned bufperm=0;
            const IOReturn bufstate=job->count?space->describe(job->handles[0],&bufiova,&bufbytes,&bufperm):kIOReturnBadArgument;
            uint32_t bufhead[8]={};
            const IOReturn bufread=bufstate?bufstate:space->peekWordsUncached(bufiova,bufhead,8);
            printf("PICASSO_GPU stall bufinfo: describe=%x iova=%llx bytes=%llu perm=%u read=%x:", unsigned(bufstate),
                (unsigned long long)bufiova,(unsigned long long)bufbytes,bufperm,unsigned(bufread));
            if(!bufread)for(uint32_t i=0;i<8;++i)printf(" %x", bufhead[i]);
            printf("\n");
            printf("PICASSO_GPU stall ib1c:");
            for(uint32_t i=0;i<8;++i) {
                uint32_t c=0,d=0;
                if(space->sampleWord(ib1+4ull*i,&c,&d)) {printf(" --");continue;}
                printf(" %x/%x", c, d);
            }
            printf("\n");
            /* Once: the whole address space this job ran in, each mapping's first words. Where a zero IB page's command stream is. */
            static uint32_t stall_map_dumps;
            if(stall_map_dumps<2) {
                ++stall_map_dumps;
                space->dumpMappings("stall",48);
                /* Whether the client's stream appeared in its buffer after the first snapshot, through the uncached reader. */
                if(job->count) {
                    uint64_t scanned=0,nz=0,first_off=0,needle_off=UINT64_MAX;uint32_t first_word=0;
                    if(space->scanMapping(job->handles[0],0x70460001,65536,&scanned,&nz,&first_off,&first_word,&needle_off))
                        printf("PICASSO_GPU scan stall: h=%llu scanned=%llu nz=%llu first=%llx:%x needle at=%llx\n",
                            (unsigned long long)job->handles[0],(unsigned long long)scanned,(unsigned long long)nz,
                            (unsigned long long)first_off,first_word,(unsigned long long)needle_off);
                }
            }
        }
    }
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;native_adreno_unstick[13]=timeout_us;
    NativeAdrenoRing *const r=job?ring(job->ring):ringOfFence(fence);
    if(!r)return kIOReturnBadArgument;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=r->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=r->lastSubmitted();native_adreno_fence_wait[5]=r->completedFence();
            noteStall(observed,clock);
            return kIOReturnTimeout;
        }
        return kIOReturnSuccess;
    }
    const NativeAdrenoRingFence result=r->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=r->lastSubmitted();native_adreno_fence_wait[5]=r->completedFence();
        noteStall(observed,clock);
        return kIOReturnTimeout;
    }
    IOReturn status=release(*job);
    if(status)return status;
    ++stats_.completed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint64_t first,uint64_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(auto &job:jobs_) {
        if(!job.active)continue;
        NativeAdrenoRing *const r=ringOfFence(job.fence);
    const NativeAdrenoRingFence state=r?r->observeFence(job.fence,nullptr,0,nullptr):NativeAdrenoRingFenceArgument;
        if(state==NativeAdrenoRingFencePoisoned)return poison();
        if(state!=NativeAdrenoRingFenceComplete)continue;
        const IOReturn status=release(job);
        if(status)return status;
        ++stats_.completed;
    }
    /* A ring that went idle leaves the CP to the other ring's queued work (driveSwitch decides by priority). */
    driveSwitch(nullptr);
    return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::madeProgress()
{
    if(progress_retries_>=3)return false;
    uint32_t state[6]={};
    if(ring(0)->progressState(state))return false;
    bool progress=false;
    for(unsigned i=0;i<6;++i)if(state[i]!=last_cp_[i])progress=true;
    for(unsigned i=0;i<6;++i)last_cp_[i]=state[i];
    if(!progress)return false;
    ++progress_retries_;
    return true;
}
IOReturn NativeAdrenoSubmit::hangCheck(bool *hung)
{
    if(!hung)return kIOReturnBadArgument;*hung=false;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    /* The newest submission of either ring: each ring's fence numbers rise, so the later of the two is the device's newest. */
    const uint32_t last0=ring(0)->lastSubmitted(),last1=preemption()?ring(1)->lastSubmitted():0;
    const uint32_t last=(int32_t)(last1-last0)>0?last1:last0;
    if(!last)return kIOReturnSuccess;
    /* A job on the second ring that the CP cannot reach: the ring was given up (dead), or it has waited so long that the switch is
     * not coming. The first ring's progress hides this from the check below, so it is judged on its own. */
    if(preemption()) {
        const uint64_t now=clock_now_?clock_now_(clock_cookie_):0;
        /* A busy high ring starves the low one by design (driveSwitch lets it through after PreemptAgingUs); only a low-ring job that waits
         * with the high ring idle and no switch in flight is a CP that cannot be reached. */
        const bool reachable=!preempt_.inFlight() && !pendingOn(0);
        for(auto &job:jobs_)if(job.active && job.ring==1 && job.ring!=preempt_.loaded()) {
            const bool aged=reachable && now && job.submit_us && now-job.submit_us>PreemptStrandedUs;
            if(preempt_dead_ || aged) {
                markPreemptDead(preempt_dead_?dead_reason_:3);
                stranded_=true;*hung=true;++stats_.hangs;
                return kIOReturnSuccess;
            }
        }
    }
    NativeAdrenoRing *const r=ringOfFence(last)?ringOfFence(last):ring(0);
    uint32_t fence=0;
    if(r->observeFence(last,nullptr,0,&fence)==NativeAdrenoRingFencePoisoned)return poison();
    if(fence!=hang_fence_) {hang_fence_=fence;progress_retries_=0;return kIOReturnSuccess;}
    if(fence>=last || madeProgress())return kIOReturnSuccess;
    progress_retries_=0;++stats_.hangs;*hung=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::recover(NativeAdrenoRestart restart,void *cookie,const NativeAdrenoRingClock *clock,
    NativeAdrenoRecovery *out)
{
    if(!out)return kIOReturnBadArgument;*out={};
    if(!attached_)return kIOReturnNotReady;
    if(!restart)return kIOReturnUnsupported;
    /* The newest fence of either ring and what the CP last wrote on the ring the restart left it on (the first ring). */
    const uint32_t last0=ring(0)->lastSubmitted(),last1=preemption()?ring(1)->lastSubmitted():0;
    const uint32_t last=(int32_t)(last1-last0)>0?last1:last0;
    uint32_t fence=0;
    const bool stranded=stranded_;
    stranded_=false;last_stranded_=stranded;
    if(stranded) {
        /* No job is to blame: the second ring's work never got a CP. What each ring finished retires by that ring's own fence, and the
         * replay starts below the oldest job left, whichever ring it sat on. */
        for(auto &job:jobs_)if(job.active) {
            uint32_t done=0;
            NativeAdrenoRing *const r=ringOfFence(job.fence)?ringOfFence(job.fence):ring(0);
            if(!r->fenceValue(&done) && (int32_t)(done-job.fence)>=0) {
                IOReturn status=release(job);if(status)return status;
                ++stats_.completed;
            }
        }
        bool any=false;uint32_t oldest=last;
        for(auto &job:jobs_)if(job.active && (!any || (int32_t)(job.fence-oldest)<0)) {oldest=job.fence;any=true;}
        fence=any?oldest-1:last;
        ++rescues_;
        native_adreno_recover_trace[2]=fence;native_adreno_recover_trace[3]=last;
    } else if(preemption()) {
        /* Two rings: a job is done when its own ring's fence says so (ring 0's fence alone knows nothing of what ring 1 finished, and
         * would name a finished job as the hung one). The oldest job left is the one the CP was running, the others wait behind it. */
        bool any=false;uint32_t oldest=last;
        for(auto &job:jobs_)if(job.active) {
            uint32_t done=0;
            NativeAdrenoRing *const r=ringOfFence(job.fence)?ringOfFence(job.fence):ring(0);
            if(!r->fenceValue(&done) && (int32_t)(done-job.fence)>=0) {
                IOReturn status=release(job);if(status)return status;
                ++stats_.completed;
            } else if(!any || (int32_t)(job.fence-oldest)<0) {oldest=job.fence;any=true;}
        }
        fence=any?oldest-1:last;
        native_adreno_recover_trace[2]=fence;native_adreno_recover_trace[3]=last;
    } else {
        const IOReturn fr=ring(0)->fenceValue(&fence);
        native_adreno_recover_trace[2]=fence;native_adreno_recover_trace[3]=last;
        if(fr || fence>last) {native_adreno_recover_trace[0]=1;native_adreno_recover_trace[1]=uint32_t(fr);return poison();}
        /* What already completed retires as usual. */
        for(auto &job:jobs_)if(job.active && job.fence<=fence) {
            IOReturn status=release(job);if(status)return status;
            ++stats_.completed;
        }
    }
    const uint32_t hung=fence<last?fence+1:fence;
    Job *guilty=(!stranded && fence<last)?find(hung):nullptr;
    out->fence=hung;
    if(guilty) {
        NativeAdrenoVm *vm=guilty->vm?guilty->vm:vm_;
        vm->noteHang(vm!=vm_);out->guilty=vm;
    }
    /* A hang inside a second-ring job is blamed on the second ring: it is given up (reason 4), the replay and everything after run on the first. */
    if(guilty && guilty->ring==1)markPreemptDead(4);
    recovery_info_[0]=guilty?uint32_t(guilty->ring):9;recovery_info_[1]=last0;recovery_info_[2]=last1;recovery_info_[3]=preemption()?preempt_.loaded():0;
    IOReturn status=restart(cookie,ring(0),hung);
    out->status=status;
    if(status) {native_adreno_recover_trace[0]=2;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
    poisoned_=false;hang_fence_=hung;progress_retries_=0;
    if(guilty) {status=release(*guilty);if(status)return status;}
    /* The GPU is idle after the restart's CP init: the CPU installs the kernel table, and each replayed job switches to its own. */
    if(table_mode_) {
        if(!write_table_) {native_adreno_recover_trace[0]=3;native_adreno_recover_trace[1]=0;return poison();}
        status=write_table_(table_cookie_,vm_->ttbr0());
        if(status) {native_adreno_recover_trace[0]=3;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
        if(table_mode_==2)ring(0)->tableInstalled(vm_->ttbr0());
        table_vm_=vm_;
    }
    /* The restart's CP init ran on the first ring: it is the one the CP runs and every replayed job goes there, whatever ring it
     * came from (the words are ring-agnostic and their fence numbers are kept, so each waiter still finds its value). */
    if(preemption()) {
        preempt_.restart(0);
        ring(0)->setCurrent(true);
        (void)ring(1)->restartSecondary(hung);
        ring(1)->setCurrent(false);
    }
    uint32_t nop=0;
    if(AdrenoPM4Type7(0x10,0,&nop)!=AdrenoPM4Success) {native_adreno_recover_trace[0]=4;return poison();}   /* CP_NOP */
    for(uint32_t next=hung+1;next && next<=last;++next) {
        Job *job=find(next);
        NativeAdrenoVm *vm=job && job->vm?job->vm:vm_;
        const bool run=job && !vm->unusable() && job->word_count;
        status=useVm(vm,clock);
        if(status) {native_adreno_recover_trace[0]=5;native_adreno_recover_trace[1]=uint32_t(status);return poison();}
        ring(0)->useContext(job?job->context:0);
        uint32_t value=0;
        status=preemption()?ring(0)->submitFramed(run?job->words:&nop,run?job->word_count:1,next,&value):
            ring(0)->submitFramed(run?job->words:&nop,run?job->word_count:1,&value);
        if(status || value!=next) {native_adreno_recover_trace[0]=6;native_adreno_recover_trace[1]=status?uint32_t(status):value;return poison();}
        if(job) {
            if(job->replays<255)++job->replays;
            job->replay_us=clock && clock->now_us?clock->now_us(clock->cookie):0;
        }
        if(job && preemption()) {job->ring=0;noteFence(next,0);}
        ++out->replayed;
    }
    ++stats_.restarts;
    return kIOReturnSuccess;
}
