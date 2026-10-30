// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real production commands with a deterministic DMA/HC fault fixture.
#include "RootCommandsBridge.cpp"
#include <array>
#include <algorithm>
#include <string>
enum class Fault { None, Empty, Type, Tag, Lun, Query, Residual, Underflow,
  Oversize, SenseLength, CheckCondition, Timeout, BusyTimeout, Fatal, Stop, Start, HighBase,
  BoundaryComplete, BoundaryFatal, BoundaryExpired,
  ClockComplete, ClockFatal, ClockResidual, ClockBusy, ClockUnaccepted,
  QueryDelayed, QueryDelayedMismatch, QueryDelayedFatal, QueryDelayedTimeout, QueryDelayedBusyTimeout,
  TurDelayed, TurDelayedFatal, TurDelayedCompleteFatal, TurDelayedTimeout, TurDelayedBusyTimeout };
static Fault fault;
static std::array<uint8_t,kDmaBytes> device;
static std::array<uint32_t,64> registers;
static std::array<uint32_t,64> smmuRegisters, cbRegisters;
static unsigned mmioReads;
static uint64_t now;
static unsigned writes, releases, acquires, doorbellWrites, runStopWrites, pollPauses, commandPauses;
static unsigned cpuHints, clockReads, legacyWaits;
static bool released, submitted, frozenClock, poisonOnLock;
static unsigned lockDepth, lockCalls, unlockCalls;
static constexpr unsigned kFormerKickPoll = 16384;
static constexpr unsigned kQueryPauseCount = 3;
static constexpr unsigned kTurPauseCount = 5;
// Deterministic fixture time per 64-hint batch. This is NOT a timing calibration
// or a claim that the production CPU hint batch takes ten microseconds.
static constexpr uint64_t kSimulatedPauseUs = 10;

static bool boundaryFault()
{
  return fault==Fault::BoundaryComplete || fault==Fault::BoundaryFatal || fault==Fault::BoundaryExpired;
}

static bool clockFault()
{
  return fault==Fault::ClockComplete || fault==Fault::ClockFatal || fault==Fault::ClockResidual ||
    fault==Fault::ClockBusy || fault==Fault::ClockUnaccepted;
}

static bool delayedQueryFault()
{
  return fault==Fault::QueryDelayed || fault==Fault::QueryDelayedMismatch ||
    fault==Fault::QueryDelayedFatal || fault==Fault::QueryDelayedTimeout || fault==Fault::QueryDelayedBusyTimeout;
}

static bool delayedTurFault()
{
  return fault==Fault::TurDelayed || fault==Fault::TurDelayedFatal || fault==Fault::TurDelayedCompleteFatal ||
    fault==Fault::TurDelayedTimeout || fault==Fault::TurDelayedBusyTimeout;
}

static void completeResponse()
{
  submitted=false;
  registers[0x58/4]=0;
  uint8_t *cmd=device.data()+kUcdOffset, *rsp=cmd+kRspOffset;
  std::fill(rsp,rsp+512,uint8_t(0));
  memset(device.data()+kDataOffset,0x7b,kDataMax);
  *reinterpret_cast<uint32_t *>(device.data()+8)=0;
  if (fault==Fault::Empty) return;
  memcpy(rsp,cmd,32); rsp[0]=cmd[0]+0x20; rsp[1]=0;
  if (cmd[0]==1) putBe32(rsp+12,0);
  if (cmd[0]==0x16) putBe32(rsp+20,0x11);
  if (fault==Fault::Type) rsp[0]=0x3f;
  if (fault==Fault::Tag) ++rsp[3];
  if (fault==Fault::Lun) ++rsp[2];
  if (fault==Fault::Query || fault==Fault::QueryDelayedMismatch) ++rsp[13];
  if (fault==Fault::Residual || fault==Fault::ClockResidual) putBe32(rsp+12,4096);
  if (fault==Fault::Underflow) rsp[1]=0x20;
  if (fault==Fault::Oversize) rsp[10]=0xff;
  if (fault==Fault::SenseLength) { rsp[11]=2; rsp[33]=18; }
  if (fault==Fault::CheckCondition) {
    rsp[7]=2; rsp[11]=20; rsp[33]=18; rsp[34]=0x70;
    rsp[36]=6; rsp[46]=0x29; rsp[47]=0;
  }
}

static uint64_t io(unsigned op,uint64_t address,uint64_t value,void *bytes)
{
  if (op==0) { ++mmioReads; return registers.at(address/4); }
  if (op==9) {
    assert(value==2 || value==4); ++mmioReads;
    return (value==2 ? smmuRegisters : cbRegisters).at(address/4);
  }
  if (op==1) {
    ++writes;
    if (address==0x60) ++runStopWrites;
    if (address==0x20) registers[address/4]&=~uint32_t(value);
    else if (!(address==0x60 && ((fault==Fault::Stop && !value) || (fault==Fault::Start && value))) &&
             !(address==0x54 && fault==Fault::HighBase)) registers.at(address/4)=uint32_t(value);
    if (address==0x58) {
      ++doorbellWrites;
      assert(released); released=false;
      submitted=true; commandPauses=0;
      if (delayedQueryFault() || delayedTurFault()) {
        const uint8_t *cmd=device.data()+kUcdOffset;
        // A later command must replace prior completed OCS/response bytes.
        // It remains HC-owned until the production pause batches run.
        if (delayedQueryFault()) {
          assert(cmd[0]==0x16 && cmd[3]==1 && cmd[5]==1 && cmd[12]==3 && cmd[13]==2);
          assert(cmd[14]==0 && cmd[15]==0 && loadBe32(cmd+20)==0);
        } else {
          assert(cmd[0]==1 && cmd[1]==0 && cmd[2]==0 && cmd[3]==0 && loadBe32(cmd+12)==0);
          assert(std::all_of(cmd+16,cmd+22,[](uint8_t byte){return byte==0;})); // TUR CDB.
        }
        assert(cmd[kRspOffset]==0 && (*reinterpret_cast<uint32_t *>(device.data()+8)&0xff)==15);
        return 0;
      }
      if (boundaryFault() || clockFault() || fault==Fault::BusyTimeout) return 0;
      if (fault==Fault::Fatal) { registers[0x20/4]=0x20000; return 0; }
      registers[0x58/4]=0;
      if (fault==Fault::Timeout) return 0;
      completeResponse();
    }
  }
  if (op==2 || op==3) {
    assert(address>=gHost.phys && address-gHost.phys+value<=device.size());
    auto at=device.data()+(address-gHost.phys);
    if (op==2) { ++releases; memcpy(at,bytes,value); released=true; }
    else {
      assert(!(registers[0x58/4]&1)); // Never invalidate descriptors while HC owns the slot.
      ++acquires; memcpy(bytes,at,value);
    }
  }
  if (op==4) {
    ++clockReads;
    if (submitted && clockFault() && now==0) {
      // The first hardware sample was busy. Simulate completion/preemption
      // during the subsequent clock read, at the READ's one-second deadline.
      now=1000000;
      if (fault==Fault::ClockUnaccepted) registers[0x58/4]=0;
      else if (fault!=Fault::ClockBusy) completeResponse();
      if (fault==Fault::ClockFatal) registers[0x20/4]=0x20000;
    }
    return now;
  }
  if (op==7) {
    assert(lockDepth==0); ++lockDepth; ++lockCalls;
    if (poisonOnLock) { gHost.ready=false; gHost.poisoned=true; }
  }
  if (op==8) { assert(lockDepth==1); --lockDepth; ++unlockCalls; }
  if (op==5) {
    ++legacyWaits;
    assert(!submitted); // Queue setup may use IODelay; command polling may not.
    now+=value;
  }
  if (op==6) {
    ++cpuHints;
    if (cpuHints%kUfsPollPauseHintCount!=0) return 0;
    if (frozenClock) return 0;
    now+=kSimulatedPauseUs;
    if (submitted) { ++pollPauses; ++commandPauses; }
    if (submitted && delayedQueryFault()) {
      if (commandPauses==kQueryPauseCount) {
        if (fault==Fault::QueryDelayedFatal) registers[0x20/4]=0x20000;
        else if (fault==Fault::QueryDelayedTimeout) registers[0x58/4]=0; // OCS stays invalid.
        else if (fault!=Fault::QueryDelayedBusyTimeout) completeResponse();
      }
    }
    if (submitted && delayedTurFault() && commandPauses==kTurPauseCount) {
      if (fault==Fault::TurDelayedFatal) registers[0x20/4]=0x20000;
      else if (fault==Fault::TurDelayedTimeout) registers[0x58/4]=0;
      else if (fault!=Fault::TurDelayedBusyTimeout) {
        completeResponse();
        if (fault==Fault::TurDelayedCompleteFatal) registers[0x20/4]=0x20000;
      }
    }
    if (submitted && boundaryFault() && pollPauses==kFormerKickPoll-1) {
      // Change hardware state between polls, immediately before the old kick
      // point. Also expire the deadline: completion/fatal must be observed first.
      now=200000;
      registers[0x60/4]=0;
      if (fault==Fault::BoundaryExpired) registers[0x58/4]=0; // OCS remains invalid.
      else completeResponse();
      if (fault==Fault::BoundaryFatal) registers[0x20/4]=0x20000;
    }
  }
  return 0;
}
static void reset(Fault f=Fault::None)
{
  fault=f; device.fill(0); registers.fill(0); now=0;
  smmuRegisters.fill(0); cbRegisters.fill(0); mmioReads=0;
  writes=releases=acquires=doorbellWrites=runStopWrites=pollPauses=commandPauses=0;
  cpuHints=clockReads=legacyWaits=0;
  released=submitted=frozenClock=poisonOnLock=false;
  lockDepth=lockCalls=unlockCalls=0;
  registers[0x34/4]=1; registers[0x30/4]=15; registers[0x60/4]=1;
  ufs_test_reset(io,0x8cb5e9000);
}
static void assertQuarantined()
{
  assert(ufs_test_poisoned());
  std::array<uint8_t,kDmaBytes> old; memcpy(old.data(),memory,sizeof(memory));
  unsigned oldWrites=writes, oldReleases=releases;
  uint8_t cdb[6]={}; uint32_t scsi=0, value=0xabcdef;
  assert(ufs_test_nop()!=0);
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef);
  assert(ufs_test_scsi(0,cdb,6,false,0,&scsi)!=0);
  std::array<uint8_t,4096> out; out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady);
  assert(std::all_of(out.begin(),out.end(),[](uint8_t byte){return byte==0xa5;}));
  assert(ufs_test_read(0,1,0,out.data())==kIOReturnBadArgument);
  if (!gStoreOpen) assert(ufs_test_store_open(1,1));
  assert(ufs_test_write(0,1,1,out.data())==kIOReturnNotReady);
  assert(ufs_test_sync()==kIOReturnNotReady);
  assert(writes==oldWrites && releases==oldReleases);
  assert(!memcmp(old.data(),memory,sizeof(memory)));
}
static void checkDeadlineRead()
{
  for (Fault next: {Fault::ClockComplete,Fault::ClockFatal,Fault::ClockResidual,
                   Fault::ClockBusy,Fault::ClockUnaccepted}) {
    reset(); assert(ufs_test_nop()==kIOReturnSuccess);
    const unsigned priorAcquires=acquires;
    fault=next;
    std::array<uint8_t,kDataMax> out; out.fill(0xa5);
    const IOReturn result=ufs_test_read(0,0x2d6460,32,out.data());
    const bool timeout=next==Fault::ClockBusy || next==Fault::ClockUnaccepted;
    assert(result==(next==Fault::ClockComplete ? kIOReturnSuccess :
                    timeout ? kIOReturnTimeout : kIOReturnIOError));
    assert(gRootDiagnostic.polls==2 && now==1000000 && pollPauses==0);
    assert(gRootDiagnostic.deadlineSample==1 && gRootDiagnostic.timeoutUs==1000000);
    assert(gRootDiagnostic.startTick==0 && gRootDiagnostic.lastTick==1000000 &&
           gRootDiagnostic.deadlineTick==1000000 && gRootDiagnostic.submitted==1);
    assert(doorbellWrites==2 && releases==2 && runStopWrites==0);
    assert(lockDepth==0 && lockCalls==1 && unlockCalls==1);
    if (next==Fault::ClockComplete) {
      assert(!ufs_test_poisoned() && acquires==priorAcquires+2);
      assert(failureLogs==0);
      assert(std::all_of(out.begin(),out.end(),[](uint8_t byte){return byte==0x7b;}));
      fault=Fault::None; assert(ufs_test_read(0,1,1,out.data())==kIOReturnSuccess);
    } else {
      if (next==Fault::ClockBusy || next==Fault::ClockFatal) assert(acquires==priorAcquires);
      if (next==Fault::ClockResidual) assert(gRootDiagnostic.protocol==6);
      assert(failureLogs==1 && strstr(failureLog,"NATIVE_UFS_FAILURE:")==failureLog);
      assert(strstr(failureLog,"timeout_us=1000000") && strstr(failureLog,"final_sample=1"));
      assert(std::all_of(out.begin(),out.end(),[](uint8_t byte){return byte==0xa5;}));
      assertQuarantined();
    }
  }
}

static void checkReadNotReady()
{
  for (bool duringLock: {false,true}) {
    reset();
    poisonOnLock=duringLock;
    if (!duringLock) { gHost.ready=false; gHost.poisoned=true; }
    std::array<uint8_t,4096> out; out.fill(0xa5);
    assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady);
    assert(writes==0 && releases==0 && acquires==0);
    assert(lockDepth==0 && lockCalls==1 && unlockCalls==1);
    assert(std::all_of(out.begin(),out.end(),[](uint8_t byte){return byte==0xa5;}));
    assertQuarantined();
  }
  // A writer/sync caller also needs the state check after acquiring the lock.
  for (bool write: {false,true}) {
    reset(); assert(ufs_test_store_open(1,1)); poisonOnLock=true;
    std::array<uint8_t,4096> input; input.fill(0xa5);
    assert((write ? ufs_test_write(0,1,1,input.data()) : ufs_test_sync())==kIOReturnNotReady);
    assert(writes==0 && releases==0 && acquires==0);
    assert(lockDepth==0 && lockCalls==1 && unlockCalls==1);
    assert(std::all_of(std::begin(memory),std::end(memory),[](uint8_t byte){return byte==0;}));
  }
  reset(); gHost.lock=nullptr;
  std::array<uint8_t,4096> out; out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady && lockCalls==0);
  assert(ufs_test_read(0,1,1,nullptr)==kIOReturnBadArgument && lockCalls==0);
}

static void checkFailureSnapshot()
{
  for (unsigned mask=0; mask<8; ++mask) {
    reset();
    gHost.hc=(mask&1) ? 1 : 0;
    gHost.smmu=(mask&2) ? 2 : 0;
    gHost.cb=(mask&4) ? 3 : 0;
    registers[0x50/4]=0xcb5e9000; registers[0x54/4]=8;
    registers[0x58/4]=1; registers[0x20/4]=0x20000;
    registers[0x18/4]=0x123; registers[0x38/4]=0x80000011; registers[0x48/4]=0x80000022;
    smmuRegisters[0x48/4]=0xa1; smmuRegisters[0x54/4]=0xb2;
    cbRegisters[0x58/4]=0xc3; cbRegisters[0x68/4]=0xd4;
    cbRegisters[0x60/4]=0xabcdef01; cbRegisters[0x64/4]=8;
    const auto oldRegisters=registers, oldSmmu=smmuRegisters, oldCb=cbRegisters;
    assert(recordTransport(kIOReturnTimeout,15)==kIOReturnTimeout);
    assert(!gRootDiagnosticEnabled && failureLogs==1 && gRootDiagnostic.faultSampleMask==mask);
    assert(mmioReads==((mask&1) ? 13U : 0U)+((mask&2) ? 2U : 0U)+((mask&4) ? 4U : 0U));
    if (mask&1) {
      assert(gRootDiagnostic.list==0x8cb5e9000ULL && gRootDiagnostic.doorbell==1);
      assert(gRootDiagnostic.interrupt==0x20000 && gRootDiagnostic.uic[4]==0x80000022);
      assert(gRootDiagnostic.hcs==15 && gRootDiagnostic.hce==1 && gRootDiagnostic.runStop==1);
      assert(gRootDiagnostic.ahit==0x123 && gRootDiagnostic.uic[0]==0x80000011);
    }
    if (mask&2) assert(gRootDiagnostic.gfsr==0xa1 && gRootDiagnostic.gsyn==0xb2);
    if (mask&4) assert(gRootDiagnostic.fsr==0xc3 && gRootDiagnostic.fsyn==0xd4 &&
                       gRootDiagnostic.far==0x8abcdef01ULL);
    const unsigned firstReads=mmioReads;
    const std::string firstLog=failureLog;
    for (unsigned repeat=0; repeat<20; ++repeat) recordTransport(kIOReturnIOError,15);
    assert(failureLogs==1 && mmioReads==firstReads && firstLog==failureLog);
    gRootDiagnosticEnabled=true;
    recordTransport(kIOReturnIOError,15);
    assert(failureLogs==1 && mmioReads==2*firstReads && firstLog==failureLog);
    assert(writes==0 && releases==0 && acquires==0);
    assert(registers==oldRegisters && smmuRegisters==oldSmmu && cbRegisters==oldCb);
  }
}

int main(int argc, char **argv)
{
  if (argc>1 && !strcmp(argv[1],"deadline")) { checkDeadlineRead(); return 0; }
  if (argc>1 && !strcmp(argv[1],"not-ready")) { checkReadNotReady(); return 0; }
  checkDeadlineRead();
  checkReadNotReady();
  checkFailureSnapshot();
  reset(); frozenClock=true;
  HardwareTransferPort paused{};
  paused.tracePoll=1;
  paused.delay();
  assert(cpuHints==64 && clockReads==0 && legacyWaits==0 && now==0);
  assert(writes==0 && releases==0 && acquires==0); // Pause alone has no I/O dependencies.
  // Only the pause is tested with a frozen clock: transfer deadlines still
  // depend on clock progress, and are not claimed to expire in this case.
  for (unsigned blocked: {0x58U,0x20U}) {
    reset();
    assert(ufs_test_nop()==kIOReturnSuccess);
    std::array<uint8_t,kDmaBytes> completedNop;
    memcpy(completedNop.data(),memory,sizeof(memory));
    const unsigned nopWrites=writes, nopReleases=releases, nopAcquires=acquires;
    registers[blocked/4]=blocked==0x58 ? 1U : 0x20000U;
    uint32_t power=0xabcdef;
    assert(ufs_test_query(3,2,&power)==kIOReturnNotReady && power==0xabcdef);
    assert(gRootDiagnostic.command==1); // Preflight rejection is not a second submitted command.
    assert(writes==nopWrites && releases==nopReleases && acquires==nopAcquires);
    assert(!memcmp(completedNop.data(),memory,sizeof(memory)));
    assert(doorbellWrites==1 && runStopWrites==0 && pollPauses==0);
    assertQuarantined();
  }
  for (Fault next: {Fault::QueryDelayed,Fault::QueryDelayedMismatch,Fault::QueryDelayedFatal,
                   Fault::QueryDelayedTimeout,Fault::QueryDelayedBusyTimeout}) {
    reset();
    assert(ufs_test_nop()==kIOReturnSuccess);
    assert(gRootDiagnostic.command==1 && gRootDiagnostic.polls==1 && pollPauses==0);
    assert(doorbellWrites==1 && releases==1 && runStopWrites==0);
    assert(memory[kUcdOffset+kRspOffset]==0x20); // Prior command completed in the same DMA allocation.
    const unsigned nopAcquires=acquires;
#ifdef NATIVE_UFS_TEST_TRACE
    const unsigned nopTraces=ufs_test_trace_calls();
    assert(nopTraces!=0);
#endif
    fault=next;
    uint32_t power=0xabcdef;
    IOReturn result=ufs_test_query(3,2,&power);
    assert(gRootDiagnostic.command==2 && gRootDiagnostic.type==0x16 && gRootDiagnostic.opcode==3);
    assert(doorbellWrites==2 && releases==2 && runStopWrites==0 && legacyWaits==0);
    assert(gRootDiagnostic.dbSeen==1 && pollPauses>=kQueryPauseCount);
#ifdef NATIVE_UFS_TEST_TRACE
    assert(ufs_test_trace_calls()>nopTraces); // Trace arguments execute; no string matching.
#endif
    const bool timedOut=next==Fault::QueryDelayedTimeout || next==Fault::QueryDelayedBusyTimeout;
    assert(pollPauses==(timedOut ? 200000U/kSimulatedPauseUs : kQueryPauseCount));
    assert(gRootDiagnostic.polls==pollPauses+(timedOut ? 2 : 1) && now==kSimulatedPauseUs*pollPauses);
    assert(cpuHints==kUfsPollPauseHintCount*pollPauses);
    if (next==Fault::QueryDelayed) {
      assert(result==kIOReturnSuccess && power==0x11 && !ufs_test_poisoned());
      assert(gRootDiagnostic.ocs==0 && acquires==nopAcquires+2);
      fault=Fault::None;
      assert(ufs_test_nop()==kIOReturnSuccess); // A completed Query leaves the queue reusable.
      assert(doorbellWrites==3 && releases==3 && runStopWrites==0);
    } else {
      assert(result==(timedOut ? kIOReturnTimeout : kIOReturnIOError));
      assert(power==0xabcdef); // Neither transport nor echo failure publishes a value.
      if (next==Fault::QueryDelayedFatal || next==Fault::QueryDelayedBusyTimeout)
        assert(acquires==nopAcquires);
      if (next==Fault::QueryDelayedMismatch) assert(gRootDiagnostic.protocol==5);
      else assert(gRootDiagnostic.ocs==15);
      assertQuarantined();
    }
  }
  for (Fault next: {Fault::TurDelayed,Fault::TurDelayedFatal,Fault::TurDelayedCompleteFatal,
                   Fault::TurDelayedTimeout,Fault::TurDelayedBusyTimeout}) {
    reset();
    assert(ufs_test_nop()==kIOReturnSuccess);
    uint32_t power=0;
    assert(ufs_test_query(3,2,&power)==kIOReturnSuccess && power==0x11);
    assert(gRootDiagnostic.command==2 && gRootDiagnostic.polls==1 && cpuHints==0);
    const unsigned priorAcquires=acquires;
    fault=next;
    uint8_t tur[6]={}; uint32_t scsi=0;
    IOReturn result=ufs_test_scsi(0,tur,6,false,0,&scsi);
    assert(gRootDiagnostic.command==3 && gRootDiagnostic.type==1 && gRootDiagnostic.opcode==0);
    assert(doorbellWrites==3 && releases==3 && runStopWrites==0 && legacyWaits==0);
    assert(gRootDiagnostic.dbSeen==1);
    const bool timedOut=next==Fault::TurDelayedTimeout || next==Fault::TurDelayedBusyTimeout;
    assert(pollPauses==(timedOut ? 200000U/kSimulatedPauseUs : kTurPauseCount));
    assert(gRootDiagnostic.polls==pollPauses+(timedOut ? 2 : 1) && now==kSimulatedPauseUs*pollPauses);
    assert(cpuHints==kUfsPollPauseHintCount*pollPauses);
    if (next==Fault::TurDelayed) {
      assert(result==kIOReturnSuccess && scsi==0 && gRootDiagnostic.ocs==0);
      assert(acquires==priorAcquires+2 && !ufs_test_poisoned());
      fault=Fault::None;
      assert(ufs_test_nop()==kIOReturnSuccess);
      assert(doorbellWrites==4 && releases==4 && runStopWrites==0);
    } else {
      assert(result==(timedOut ? kIOReturnTimeout : kIOReturnIOError));
      assert(scsi==0xffff && gRootDiagnostic.ocs==15); // No response status parsed on failure.
      if (next!=Fault::TurDelayedTimeout) assert(acquires==priorAcquires);
      assertQuarantined();
    }
  }
  for (Fault f: {Fault::BoundaryComplete,Fault::BoundaryFatal,Fault::BoundaryExpired}) {
    reset(f);
    IOReturn result=ufs_test_nop();
    assert(gRootDiagnostic.polls==kFormerKickPoll+(f==Fault::BoundaryExpired ? 1 : 0) && now==200000);
    assert(doorbellWrites==1 && releases==1 && runStopWrites==0);
    assert(registers[0x60/4]==0); // Never restart a stopped request list while polling.
    if (f==Fault::BoundaryComplete) {
      assert(result==kIOReturnSuccess && gRootDiagnostic.ocs==0 && !ufs_test_poisoned());
    } else {
      assert(result==(f==Fault::BoundaryFatal ? kIOReturnIOError : kIOReturnTimeout));
      assert(gRootDiagnostic.ocs==15);
      if (f==Fault::BoundaryFatal) assert(acquires==0); // Fatal takes precedence over OCS=0.
      assertQuarantined();
    }
  }
  reset(); assert(ufs_test_nop()==0);
  uint32_t value=0; assert(ufs_test_query(3,2,&value)==0 && value==0x11);
  std::array<uint8_t,131072> out;
  assert(ufs_test_read(0,1,32,out.data())==0);
  assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0x7b;}));
  assert(*reinterpret_cast<uint32_t *>(device.data()+20)==8);
  assert(*reinterpret_cast<uint32_t *>(device.data()+4096+1024+4)==8);
  assert(ufs_test_read(0,0xffffffffULL,1,out.data())==0); // Last READ(10) LBA is valid.
  auto oldWrites=writes;
  assert(ufs_test_read(0,0xffffffffULL,2,out.data())==kIOReturnBadArgument && writes==oldWrites);
  for (Fault f: {Fault::Empty,Fault::Type,Fault::Tag,Fault::Lun,Fault::Residual,
                Fault::Underflow,Fault::Oversize,Fault::SenseLength,Fault::Timeout,Fault::BusyTimeout,Fault::Fatal}) {
    reset(f); out.fill(0xa5);
    assert(ufs_test_read(0,1,1,out.data())!=0);
    assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0xa5;}));
    assert(doorbellWrites==1 && releases==1 && runStopWrites==0);
    if (f==Fault::BusyTimeout) assert(acquires==0); // Do not read HC-owned descriptors.
    assertQuarantined();
  }
  reset(Fault::Query); value=0xabcdef;
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef); assertQuarantined();
  reset(Fault::CheckCondition); out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady);
  assert(!gHost.poisoned && gRootDiagnostic.sense==0x062900);
  assert(out[0]==0xa5);
  fault=Fault::None; assert(ufs_test_read(0,1,1,out.data())==0);
  reset(); uint8_t cdb[17]={}; uint32_t scsi;
  assert(ufs_test_scsi(0,cdb,17,false,0,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,true,kDataMax+4,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,false,4096,&scsi)==kIOReturnBadArgument); // No writes to media.
  assert(writes==0 && releases==0);
  for (unsigned offset: {0x58U,0x78U}) {
    reset(); registers[offset/4]=1;
    assert(!ufs_test_stop_queue() && writes==0);
  }
  reset(); registers[0x34/4]=0; assert(!ufs_test_stop_queue() && writes==0);
  reset(); registers[0x20/4]=0x20000;
  assert(!ufs_test_stop_queue() && writes==0);
  assert(ufs_test_nop()!=0 && writes==0 && releases==0); assertQuarantined();
  reset(Fault::Stop); assert(!ufs_test_stop_queue() && now>=100000);
  reset(Fault::Start); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(Fault::HighBase); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(); assert(ufs_test_stop_queue() && ufs_test_start_queue());
  assert(registers[0x50/4]==0xcb5e9000 && registers[0x54/4]==8);
  puts("PASS: bounded production pause without clock/legacy wait dependencies; NOP/Query then delayed TUR, timeout/fatal quarantine, no resubmission, DMA ownership and command/queue regressions");
}
