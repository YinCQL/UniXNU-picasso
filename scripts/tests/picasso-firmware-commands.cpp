// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production UPIU/lock/DMA/poison path, including old regressions.
#define main rootCommandsRegressionMain
#include "../../drivers/NativeUfs/tests/RootCommands.cpp"
#undef main
enum class DescriptorFault { None, Absent, Disabled, Type, Index, Short, Long,
  Length, Echo, Error, BlockSize, Count, Capacity, CapacityBlock, Attention, NotReady };
static DescriptorFault descriptorFault;
static unsigned capacityCalls;
static uint64_t firmwareIo(unsigned op,uint64_t address,uint64_t value,void *bytes) {
  uint64_t result=io(op,address,value,bytes);
  if(op!=1 || address!=0x58 || submitted)return result;
  assert(lockDepth==1);
  uint8_t *cmd=device.data()+kUcdOffset,*rsp=cmd+kRspOffset;
  if(cmd[0]==0x16) {
    assert(cmd[2]==0 && cmd[3]==1 && cmd[5]==1 && cmd[12]==1 && cmd[13]==2 && cmd[14]==4 &&
      !cmd[15] && !cmd[18] && cmd[19]==255 && !cmd[10] && !cmd[11]);
    assert(!(*reinterpret_cast<uint32_t *>(device.data()) & (3U<<25)));
    memset(rsp+32,0,0x23);rsp[11]=rsp[19]=0x23;
    uint8_t *desc=rsp+32;desc[0]=0x23;desc[1]=2;desc[2]=4;desc[3]=1;desc[10]=12;putBe32(desc+15,1001);
    if(descriptorFault==DescriptorFault::Absent || descriptorFault==DescriptorFault::Error) {
      rsp[6]=descriptorFault==DescriptorFault::Absent?0xfc:0xf9;rsp[11]=0;
    }
    if(descriptorFault==DescriptorFault::Disabled)desc[3]=0;
    if(descriptorFault==DescriptorFault::Type)desc[1]=3;
    if(descriptorFault==DescriptorFault::Index)desc[2]=0;
    if(descriptorFault==DescriptorFault::Short)rsp[11]=0x12;
    if(descriptorFault==DescriptorFault::Long)rsp[10]=1;
    if(descriptorFault==DescriptorFault::Length)rsp[19]=0x22;
    if(descriptorFault==DescriptorFault::Echo)rsp[14]=0;
    if(descriptorFault==DescriptorFault::BlockSize)desc[10]=9;
    if(descriptorFault==DescriptorFault::Count)putBe32(desc+11,1);
  } else {
    assert(cmd[0]==1 && cmd[2]==4 && cmd[16]==0x25 && loadBe32(cmd+12)==8);
    ++capacityCalls;uint8_t *data=device.data()+kDataOffset;
    putBe32(data,descriptorFault==DescriptorFault::Capacity?999:1000);
    putBe32(data+4,descriptorFault==DescriptorFault::CapacityBlock?512:4096);
    if((descriptorFault==DescriptorFault::Attention && capacityCalls<=2) || descriptorFault==DescriptorFault::NotReady) {
      rsp[7]=2;rsp[11]=20;rsp[33]=18;rsp[34]=0x70;
      rsp[36]=descriptorFault==DescriptorFault::Attention?6:2;rsp[46]=0x29;
    }
  }
  return result;
}
int main() {
  assert(!rootCommandsRegressionMain(0,nullptr));
  for(DescriptorFault f: {DescriptorFault::None,DescriptorFault::Absent,DescriptorFault::Disabled,
      DescriptorFault::Type,DescriptorFault::Index,DescriptorFault::Short,DescriptorFault::Long,
      DescriptorFault::Length,DescriptorFault::Echo,DescriptorFault::Error,DescriptorFault::BlockSize,
      DescriptorFault::Count,DescriptorFault::Capacity,DescriptorFault::CapacityBlock,
      DescriptorFault::Attention,DescriptorFault::NotReady}) {
    reset();descriptorFault=f;capacityCalls=0;callback=firmwareIo;uint64_t blocks=999;
    int result=ufs_test_firmware_geometry(4,&blocks);
    if(f==DescriptorFault::None || f==DescriptorFault::Attention)assert(!result && blocks==1001);
    else {assert(result && !blocks);}
    if(f==DescriptorFault::Absent || f==DescriptorFault::Disabled)assert(result==-2 && !capacityCalls && !gHost.poisoned);
    if(f==DescriptorFault::BlockSize || f==DescriptorFault::Count)assert(result==-95 && !capacityCalls);
    if(f==DescriptorFault::Short || f==DescriptorFault::Long || f==DescriptorFault::Length || f==DescriptorFault::Echo)
      assert(gHost.poisoned);
    if(f==DescriptorFault::Attention)assert(capacityCalls==3);
    if(f==DescriptorFault::NotReady)assert(capacityCalls==1 && result==-5);
    assert(lockDepth==0 && lockCalls==1 && unlockCalls==1 && !runStopWrites);
  }
  for(Fault f: {Fault::Timeout,Fault::Fatal,Fault::BusyTimeout}) {
    reset(f);uint64_t blocks=1;assert(ufs_test_firmware_geometry(4,&blocks)==-5 && !blocks);
    assert(gHost.poisoned && lockDepth==0);assertQuarantined();
  }
  reset();poisonOnLock=true;uint64_t blocks=1;
  assert(ufs_test_firmware_geometry(4,&blocks)==-5 && !blocks && !doorbellWrites && !lockDepth);
  reset();assert(ufs_test_firmware_geometry(8,&blocks)==-22 && !doorbellWrites);
  puts("PASS production firmware commands: descriptor identity/length, absent LU, capacity, unit attention, lock and poison");
}
