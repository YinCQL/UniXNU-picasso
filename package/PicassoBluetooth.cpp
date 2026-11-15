/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include "qbt.h"
#include "qbt_geni.h"
#include "qbt_transport.h"
#include "PicassoBluetoothBoard.h"
#include "PicassoBtSmmu.h"


extern "C" int armcpu_spi_attach(uint32_t,bool,void(*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int armcpu_spi_mask(uint32_t,bool);
extern "C" volatile int native_gpu_addon_stage;
extern "C" {
volatile uint64_t native_bluetooth_stats[32];
uint8_t native_bluetooth_boot_rx[512];
volatile uint32_t native_bluetooth_boot_rx_length;
uint8_t native_bluetooth_boot_tail[256];
volatile uint32_t native_bluetooth_boot_tail_length;
/* log-bluetooth=2: pins before TX, RX bytes before TX, then pins, engine status and download reports when startup returns. */
volatile uint32_t native_bluetooth_boot_state[12];
/* log-bluetooth=2: live pid and starts seen for bluetoothd, then for BlueTool. */
volatile uint32_t native_bluetooth_processes[4];
/* log-bluetooth=2: printable runs in controller output, which carry its assert text. */
char *volatile native_bluetooth_boot_text;
volatile uint32_t native_bluetooth_boot_text_length;
}
namespace {
struct Region {IOMemoryMap *map;volatile uint32_t *va;};
struct Bluetooth {
    Region gcc,uart,wrapper,tlmm,smmu;
    IOService *radio;
    IOBufferMemoryDescriptor *dma;
    struct smmu_plan domain;
    uint64_t dma_address;
    struct qbt_geni geni;
    struct qbt *core;
    uint64_t rx_sequence;
    uint32_t pending,power_request,started;
    int runtime;
    unsigned log,baud,clock_select;
    uint8_t address[6];
    bool resources,powered,gated,clocked,serial_irq,service,initializing,discard_rx;
} bt;
constexpr unsigned kSwCtrl=73;

constexpr uint32_t kDmaBytes=32768;
bool map(Region &r,uint64_t base,size_t bytes) {
    if(r.va)return true;
    IODeviceMemory *memory=IODeviceMemory::withRange(base,bytes);
    r.map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(memory)memory->release();r.va=r.map?reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()):nullptr;
    return r.va!=nullptr;
}
void barrier(){__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowNs(void *){uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
void wake(void *){__atomic_store_n(&bt.pending,1u,__ATOMIC_RELEASE);thread_wakeup((event_t)&bt.pending);}
void waitUntil(void *,uint64_t deadline) {
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))return;
    if(deadline==UINT64_MAX)assert_wait((event_t)&bt.pending,THREAD_UNINT);
    else {uint64_t absolute;nanoseconds_to_absolutetime(deadline,&absolute);assert_wait_deadline((event_t)&bt.pending,THREAD_UNINT,absolute);}
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))clear_wait(current_thread(),THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
uint32_t urd(void *,uint32_t reg){return bt.uart.va[reg/4];}
void uwr(void *,uint32_t reg,uint32_t value){bt.uart.va[reg/4]=value;barrier();}
uint32_t grd(void *,uint32_t reg){return bt.gcc.va[reg/4];}
void gwr(void *,uint32_t reg,uint32_t value){bt.gcc.va[reg/4]=value;barrier();}
void dmaBarrier(void *){barrier();}
void dmaSync(void *,const struct qbt_dma_buffer *buffer,size_t bytes,bool device,bool tx) {
    // Cache maintenance for RAM accessed through the firmware bypass bank.
    if(bytes && (device || !tx)) {
        uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
        size_t line=4u<<((ctr>>16)&15);
        uintptr_t at=reinterpret_cast<uintptr_t>(buffer->bytes)&~(line-1);
        uintptr_t end=reinterpret_cast<uintptr_t>(buffer->bytes)+bytes;
        for(;at<end;at+=line) {
            if(tx)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
            else if(device)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
            else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
        }
    }
    barrier();
}
uint32_t srd(void *,uint32_t reg){return bt.smmu.va[reg/4];}
void swr(void *,uint32_t reg,uint32_t value){bt.smmu.va[reg/4]=value;barrier();}
void irqMask(void *,bool masked) {
    if(bt.serial_irq)(void)armcpu_spi_mask(picasso_bt::Irq,masked);
}
void serialIrq(void *){qbt_geni_irq(&bt.geni);}
void pin(unsigned gpio,uint32_t cfg,int output=-1) {
    if(output>=0)bt.tlmm.va[(gpio*4096+4)/4]=output?2:0;
    bt.tlmm.va[gpio*4096/4]=cfg;barrier();
}
bool serialClock(bool enabled) {
    uint32_t votes=grd(nullptr,0x52008);
    uint32_t desired=enabled?votes|(1u<<15)|(1u<<6)|(1u<<7):votes&~(1u<<15);
    gwr(nullptr,0x52008,desired);
    if((grd(nullptr,0x52008)&(1u<<15))!=(desired&(1u<<15)))return false;
    if(!enabled){bt.clocked=false;return true;}
    for(unsigned i=0;i<1000;++i) {
        if(!(grd(nullptr,0x175fc)&(1u<<31))){bt.clocked=true;return true;}
        IODelay(1);
    }
    return false;
}
int sourceClock(uint32_t baud,uint32_t &hz) {
    picasso_bt::Clock c={};if(!picasso_bt::clock(baud,c))return -EINVAL;
    // Do not rewrite an engine under firmware DFS ownership.
    bt.clock_select=0;
    if(grd(nullptr,0x17614)&1u){
        // Firmware DFS owns the RCG table. Select an existing matching entry
        // through GENI_CLK_SEL instead of overwriting its clock configuration.
        for(unsigned i=0;i<8;++i){
            uint32_t cfg=grd(nullptr,0x1761c+4*i),m=grd(nullptr,0x1765c+4*i)&0xffff;
            uint32_t n=((~grd(nullptr,0x1769c+4*i))&0xffff)+m;
            uint32_t parent=(cfg>>8)&7,mode=(cfg>>12)&3;
            uint64_t rate=parent==0?19200000:parent==1?600000000:parent==6?300000000:0;
            rate=rate*2/((cfg&31)+1);
            if(mode){if(mode!=2 || !m || !n)continue;rate=rate*m/n;}
            if(rate==c.hz){bt.clock_select=i;hz=c.hz;return 0;}
        }
        return -ENOTSUP;
    }
    gwr(nullptr,0x17608,c.m);gwr(nullptr,0x1760c,c.n?~(c.n-c.m)&0xffffu:0);
    gwr(nullptr,0x17610,c.n?~c.n&0xffffu:0);
    gwr(nullptr,0x17604,c.cfg);gwr(nullptr,0x17600,grd(nullptr,0x17600)|1u);
    for(unsigned n=0;n<1000;++n){if(!(grd(nullptr,0x17600)&1u)){hz=c.hz;return 0;}IODelay(1);}
    return -ETIMEDOUT;
}
size_t rxBytes(void *,const uint8_t *bytes,size_t n) {
    if(bt.discard_rx)return n;
    if(bt.log>=2 && bt.initializing) {
        uint32_t at=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_RELAXED);
        size_t count=sizeof(native_bluetooth_boot_rx)-at;if(count>n)count=n;
        if(count) {
            memcpy(native_bluetooth_boot_rx+at,bytes,count);
            __atomic_store_n(&native_bluetooth_boot_rx_length,at+(uint32_t)count,__ATOMIC_RELEASE);
        }
        static char run[96];static unsigned runLength;
        for(size_t i=0;i<n;i++) {
            uint8_t c=bytes[i];
            if(c>=32 && c<127 && runLength<sizeof(run)-1){run[runLength++]=(char)c;continue;}
            if(runLength>=12 && memchr(run,' ',runLength)) {
                uint32_t at=native_bluetooth_boot_text_length;
                if(native_bluetooth_boot_text && at+runLength+1<8192) {
                    memcpy(native_bluetooth_boot_text+at,run,runLength);native_bluetooth_boot_text[at+runLength]='|';
                    __atomic_store_n(&native_bluetooth_boot_text_length,at+runLength+1,__ATOMIC_RELEASE);
                }
            }
            runLength=0;
        }
        uint32_t tail=__atomic_load_n(&native_bluetooth_boot_tail_length,__ATOMIC_RELAXED);
        for(size_t i=0;i<n;i++)native_bluetooth_boot_tail[(tail+i)%sizeof(native_bluetooth_boot_tail)]=bytes[i];
        __atomic_store_n(&native_bluetooth_boot_tail_length,tail+(uint32_t)n,__ATOMIC_RELEASE);
    }
    bt.rx_sequence+=n;return qbt_receive(bt.core,bytes,n);
}
size_t nativeRx(void *,const uint8_t *bytes,size_t n){return qbt_transport_receive(bytes,(uint32_t)n);}
size_t writeUart(void *,const uint8_t *p,size_t n){return qbt_geni_write(&bt.geni,p,n);}
bool drained(void *){return bt.gated || qbt_geni_drained(&bt.geni);}
int flow(void *,bool enabled){qbt_geni_set_flow(&bt.geni,enabled);return 0;}
void pump(void *) {
    if(bt.log>=2 && bt.initializing) {
        struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
        native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
        native_bluetooth_boot_state[10]=(uint32_t)stats.tlv_acks|((uint32_t)stats.tlv_sent<<16);
        native_bluetooth_boot_state[11]=(uint32_t)stats.hardware_errors|((uint32_t)stats.last_hardware_error<<8)|((uint32_t)stats.tlv_final_status<<16);
    }
    if(!bt.gated) {
        if(qbt_geni_service(&bt.geni))wake(nullptr);
        if(bt.geni.state==QBT_GENI_FAILED)qbt_fail(bt.core,bt.geni.error);
    }
}
int quiesce() {
    uint64_t end=nowNs(nullptr)+40000000ULL;
    for(;;) {
        int e=qbt_geni_quiesce(&bt.geni);if(!e)return 0;
        if(e!=-EINPROGRESS && e!=-EBUSY)return e;
        pump(nullptr);if(bt.geni.state==QBT_GENI_STOPPED)return 0;
        if(nowNs(nullptr)>=end)return -ETIMEDOUT;
        uint64_t next=qbt_geni_deadline(&bt.geni);if(next>end)next=end;waitUntil(nullptr,next);
    }
}
int clockVote(void *,bool enabled) {
    // RX wake routing has not yet been qualified on this board. Keep the
    // serial clock on and reject gating rather than losing a wake byte.
    return enabled?0:-EBUSY;
}
int setRate(void *,uint32_t baud) {
    int e=quiesce();if(e)return e;
    uint32_t hz=0;
    e=sourceClock(baud,hz);
    if(!e)e=qbt_geni_set_rate(&bt.geni,baud,hz,16);
    if(!e){uwr(nullptr,0x7c,bt.clock_select);e=qbt_geni_resume(&bt.geni);}
    if(!e)bt.baud=baud;return e;
}
void *allocate(void *,size_t n){return IOMalloc(n);}
void release(void *,void *p,size_t n){IOFree(p,n);}
int firmware(void *,const char *name,const uint8_t **data,size_t *size) {
    return -picasso_bt_firmware_read(name,data,size);
}
void firmwarePut(void *,const uint8_t *data,size_t size){picasso_bt_firmware_release(data,size);}
void log(void *,int level,const char *line){if(level<=3 || bt.log)printf("[bluetooth] %s\n",line);}
bool address() {
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *id=chosen?OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")):nullptr;
    if(!id || id->getLength()<8){if(chosen)chosen->release();return false;}
    uint64_t hash=0xcbf29ce484222325ULL;
    static const char domain[]="unixnu-bluetooth";
    for(unsigned i=0;i<sizeof(domain)-1;i++)hash=(hash^(uint8_t)domain[i])*0x100000001b3ULL;
    const uint8_t *seed=static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero=false;
    for(unsigned i=0;i<id->getLength();i++){nonzero|=seed[i]!=0;hash=(hash^seed[i])*0x100000001b3ULL;}
    bool published=false;
    if(nonzero) {
        for(unsigned i=0;i<6;i++)bt.address[i]=(uint8_t)(hash>>(i*8));
        bt.address[0]=(bt.address[0]&0xfe)|2;
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/bluetooth",gIODTPlane);
        if(!node) {
            IORegistryEntry *parent=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
            node=new IORegistryEntry;
            uint32_t encoding=1,speed=3000000;
            if(node && node->init() && parent) {
                node->setName("bluetooth");
                node->setProperty("device_type","bluetooth");
                node->setProperty("compatible","qcom,wcn3990-bt");
                node->setProperty("transport-encoding",&encoding,4);
                node->setProperty("transport-speed",&speed,4);
                if(!node->attachToParent(parent,gIODTPlane)){node->release();node=nullptr;}
            } else {if(node)node->release();node=nullptr;}
            if(parent)parent->release();
        }
        OSData *encoding=node?OSDynamicCast(OSData,node->getProperty("transport-encoding")):nullptr;
        OSData *speed=node?OSDynamicCast(OSData,node->getProperty("transport-speed")):nullptr;
        uint32_t wire=0,baud=0;
        if(encoding && encoding->getLength()==4)memcpy(&wire,encoding->getBytesNoCopy(),4);
        if(speed && speed->getLength()==4)memcpy(&baud,speed->getBytesNoCopy(),4);
        if(node && wire==1 && baud==3000000) {
            published=chosen->setProperty("mac-address-bluetooth0",bt.address,6) &&
                node->setProperty("local-mac-address",bt.address,6);
            if(published && !bt.radio) {
                IOService *radio=new IOService;
                if(radio && radio->init()) {
                    radio->setName("bluetooth");
                    published=radio->setProperty("device_type","bluetooth") &&
                        radio->setProperty("transport-encoding",encoding) &&
                        radio->setProperty("transport-speed",speed) &&
                        radio->setProperty("local-mac-address",bt.address,6);
                    IOService *provider=IOService::getPlatform();
                    if(published && provider && radio->attach(provider)) {
                        bt.radio=radio;radio->registerService();
                    } else {published=false;radio->release();}
                } else {published=false;if(radio)radio->release();}
            }
        }
        if(node)node->release();
    }
    chosen->release();return published;

}
int resources() {
    if(bt.resources)return 0;
    if(!map(bt.gcc,0x100000,0x80000) || !map(bt.uart,picasso_bt::Uart,0x4000) ||
        !map(bt.wrapper,picasso_bt::Wrapper,0x2000) || !map(bt.tlmm,0xf900000,0x4a000) || !map(bt.smmu,0x15000000,0xd0000))return -ENOMEM;
    const IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(!bt.dma)bt.dma=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,kDmaBytes,4096);
    if(!bt.dma)return -ENOMEM;
    IOByteCount contiguous=0;
    uint64_t pa=bt.dma->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    if(!pa || contiguous<kDmaBytes)return -EIO;
    // Data is mounted before the worker enters here: UFS takeover is complete.
    // Only this worker modifies QUP0 SID; GPU uses its separate SMMU.
    struct smmu_io io={nullptr,srd,swr};
    bt.domain=smmu_plan_bypass(&io,picasso_bt::Sid,pa,kDmaBytes);
    if(!bt.domain.error && !bt.domain.bank_ready)bt.domain.error="UFS bypass bank not initialized";
    const char *error=bt.domain.error?bt.domain.error:smmu_configure_bypass(&io,&bt.domain,picasso_bt::Sid);
    if(error) {
        printf("[bluetooth dma] slot=%u bank=%u result=%s\n",bt.domain.slot,bt.domain.bank,error);
        return -EIO;
    }
    bt.dma_address=pa;
    printf("[bluetooth dma] bypass slot=%u bank=%u pa=%llx bytes=%u\n",bt.domain.slot,bt.domain.bank,pa,kDmaBytes);
    bt.resources=true;return 0;
}
int stopEngine() {
    irqMask(nullptr,true);uwr(nullptr,0xe1c,0);
    if((urd(nullptr,0x40)&0x1001) || (urd(nullptr,0xe40)&3)) {
        uwr(nullptr,0x604,2);uwr(nullptr,0x634,2);
        unsigned i=0;for(;i<1000 && (urd(nullptr,0x40)&0x1001);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
        uwr(nullptr,0xc58,1);uwr(nullptr,0xd58,1);
        for(i=0;i<1000 && (urd(nullptr,0xe40)&3);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
    }
    uwr(nullptr,0x618,UINT32_MAX);uwr(nullptr,0x648,UINT32_MAX);
    uwr(nullptr,0xc44,UINT32_MAX);uwr(nullptr,0xd44,UINT32_MAX);
    bt.geni.state=QBT_GENI_OFF;bt.geni.rx_busy=bt.geni.tx_busy=false;
    return 0;
}
uint32_t linePins() {
    static const uint8_t pins[]={38,39,40,41,kSwCtrl};
    uint32_t bits=0;
    for(unsigned i=0;i<sizeof(pins);i++)bits|=(bt.tlmm.va[(pins[i]*4096u+4)/4]&1u)<<i;
    return bits;
}
int prepareEngine() {
    uint8_t *bytes=static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    struct qbt_dma_buffer tx={bytes,bt.dma_address,QBT_GENI_TX_SIZE},rx[QBT_GENI_RX_COUNT];
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)rx[i]={bytes+4096+i*QBT_GENI_RX_SIZE,bt.dma_address+4096+i*QBT_GENI_RX_SIZE,QBT_GENI_RX_SIZE};
    struct qbt_geni_ops ops={nullptr,urd,uwr,dmaBarrier,dmaSync,nowNs,irqMask,wake,rxBytes};
    int e=qbt_geni_prepare(&bt.geni,&ops,&tx,rx);if(e)return e;
    native_bluetooth_stats[14]=5;
    uint32_t hz;
    e=sourceClock(115200,hz);if(e)return e;
    e=qbt_geni_set_rate(&bt.geni,115200,hz,16);if(e)return e;
    uwr(nullptr,0x7c,bt.clock_select);bt.baud=115200;return 0;
}
int pulse(uint8_t value,uint32_t baud,unsigned settle) {
    int e=setRate(nullptr,baud);if(e)return e;
    qbt_geni_set_flow(&bt.geni,false);
    // CTS masking is required for WCN399x power pulses (hci_qca.c).
    uwr(nullptr,0x25c,urd(nullptr,0x25c)|2u);
    if(qbt_geni_write(&bt.geni,&value,1)!=1)return -EIO;
    uint64_t end=nowNs(nullptr)+20000000ULL;
    while(!qbt_geni_drained(&bt.geni)) {
        pump(nullptr);if(bt.geni.state==QBT_GENI_FAILED)return bt.geni.error;
        if(nowNs(nullptr)>=end)return -ETIMEDOUT;IOSleep(1);
    }
    IOSleep(settle);
    uwr(nullptr,0x25c,urd(nullptr,0x25c)&~2u);
    qbt_geni_set_flow(&bt.geni,true);return 0;
}
int powerOn() {
    if(bt.powered){struct qbt_info i;qbt_get_info(bt.core,&i);return i.state==2?0:-EIO;}
    native_bluetooth_stats[14]=1;int e=resources();if(e)return e;
    native_bluetooth_stats[14]=2;
    e=picasso_bt_power(true);if(e)return e;
    bt.powered=true;bt.gated=true;
    // GPIO73 is SW_CTRL input; WCN399x has UART power pulses, no BT_EN GPIO.
    pin(kSwCtrl,0u);
    for(unsigned gpio=38;gpio<=41;++gpio)pin(gpio,(1u<<2)|(gpio==39?1u:gpio==40?3u:0u));
    native_bluetooth_stats[14]=3;
    gwr(nullptr,0x52010,grd(nullptr,0x52010)|1u);
    for(unsigned n=0;n<1000 && !(grd(nullptr,0)&(1u<<31));++n)IODelay(1);
    if(!(grd(nullptr,0)&(1u<<31)) || !serialClock(true))return -EIO;
    bt.gated=false;
    native_bluetooth_stats[11]=urd(nullptr,0x68);native_bluetooth_stats[12]=bt.wrapper.va[1];
    printf("PICASSO_BT uart fw=%x wrapper=%x dfs=%x pins=%x\n",urd(nullptr,0x68),bt.wrapper.va[1],grd(nullptr,0x17614),linePins());
    if(((urd(nullptr,0x68)>>8)&255)!=2)return -ENXIO;
    e=stopEngine();if(e)return e;
    qbt_reset(bt.core);bt.discard_rx=true;
    e=prepareEngine();if(e)return e;
    if(!bt.serial_irq){e=armcpu_spi_attach(picasso_bt::Irq,false,serialIrq,nullptr,nullptr,nullptr);if(e)return -EIO;bt.serial_irq=true;irqMask(nullptr,true);}
    e=qbt_geni_start(&bt.geni);if(e)return e;
    native_bluetooth_stats[14]=4;
    e=pulse(0xc0,2400,10);if(!e)e=pulse(0xfc,115200,100);if(e)return e;
    e=quiesce();if(!e)e=qbt_geni_resume(&bt.geni);if(e)return e;
    native_bluetooth_stats[14]=6;bt.initializing=true;bt.discard_rx=false;
    e=qbt_boot_wcn399x(bt.core,3000000,bt.address);bt.initializing=false;
    struct qbt_info info;qbt_get_info(bt.core,&info);
    printf("PICASSO_BT init state=%u error=%d product=%x soc=%x rom=%x patch=%x baud=%u\n",
        info.state,e,info.product,info.soc,info.rom,info.patch,info.baud);
    return e;
}
int powerOff() {
    if(!bt.powered)return 0;
    qbt_set_service(bt.core,false);bt.service=false;bt.discard_rx=true;
    int e=0;
    if(bt.clocked && bt.geni.state==QBT_GENI_RUN)e=pulse(0xc0,2400,10);
    int stopped=bt.clocked?stopEngine():0;if(!e)e=stopped;
    if(stopped)return e; // Keep DMA and rails pinned if the engine has not stopped.
    qbt_reset(bt.core);
    irqMask(nullptr,true);
    if(!serialClock(false) && !e)e=-EIO;
    bt.gated=true;bt.powered=false;
    int released=picasso_bt_power(false);if(!e)e=released;
    return e;
}
int requestPower(void *,bool enabled) {
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&bt.power_request,&expected,enabled?1u:2u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    wake(nullptr);return 0;
}
void powerResult(bool enabled,int error) {
    struct qbt_info info;qbt_get_info(bt.core,&info);
    native_bluetooth_stats[17]=info.product;native_bluetooth_stats[18]=info.soc;
    native_bluetooth_stats[19]=((uint32_t)info.rom<<16)|info.patch;
    if(enabled && !error)native_bluetooth_stats[14]=7;
    qbt_transport_power_result(enabled,error<0?-error:error,info.firmware,info.baud);
    native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
    native_bluetooth_stats[21]=bt.powered;
    native_bluetooth_stats[0]=error?3:enabled?2:0;native_bluetooth_stats[1]=(uint64_t)(int64_t)error;
    printf("[bluetooth] power=%u result=%d baud=%u\n",enabled,error,info.baud);
    if(error && bt.geni.error)printf("[bluetooth uart] error=%d irq=%x/%x/%x/%x\n",bt.geni.error,bt.geni.fault_irq[0],bt.geni.fault_irq[1],bt.geni.fault_irq[2],bt.geni.fault_irq[3]);
}
void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"picasso-bluetooth");
    struct qbt_platform ops={nullptr,allocate,release,nowNs,writeUart,drained,setRate,flow,clockVote,pump,waitUntil,nativeRx,firmware,firmwarePut,log};
    bt.core=qbt_create(&ops);if(!bt.core){native_bluetooth_stats[0]=3;return;}
    struct qbt_transport_ops transport={nullptr,wake,requestPower};
    int e=qbt_transport_init(&transport);if(e){native_bluetooth_stats[0]=3;return;}
    while(!__atomic_load_n(&bt.runtime,__ATOMIC_ACQUIRE))waitUntil(nullptr,nowNs(nullptr)+100000000ULL);
    if(bt.runtime!=1){powerResult(true,bt.runtime);return;}
    e=powerOn();if(e)(void)powerOff();powerResult(true,e);
    uint64_t health=nowNs(nullptr)+1000000000ULL,closeAt=0;
    for(;;) {
        uint32_t command=__atomic_exchange_n(&bt.power_request,0u,__ATOMIC_ACQ_REL);
        if(command){e=command==1?powerOn():powerOff();if(e && command==1)(void)powerOff();powerResult(command==1,e);}
        if(bt.powered) {
            pump(nullptr);bool active=qbt_transport_active();
            if(active!=bt.service){qbt_set_service(bt.core,active);bt.service=active;closeAt=active?0:nowNs(nullptr)+500000000ULL;}
            uint8_t tx[2048];size_t room=qbt_transmit_capacity(bt.core);
            if(room>sizeof(tx))room=sizeof(tx);
            if(room){uint32_t n=qbt_transport_tx_take(tx,(uint32_t)room);if(n && qbt_transmit(bt.core,tx,n))e=-EIO;}
            qbt_step(bt.core);
            struct qbt_info info;qbt_get_info(bt.core,&info);
            if(info.state==3 || bt.geni.state==QBT_GENI_FAILED || (closeAt && nowNs(nullptr)>=closeAt)) {
                int error=info.state==3?info.error:bt.geni.error;
                (void)powerOff();powerResult(false,error);closeAt=0;
            }
        }
        uint64_t t=nowNs(nullptr);
        if(t>=health) {
            struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
            native_bluetooth_stats[2]=bt.baud;native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
            native_bluetooth_stats[5]=bt.geni.stats.interrupts;native_bluetooth_stats[6]=stats.timeouts;native_bluetooth_stats[7]=stats.clock_sleeps;
            native_bluetooth_stats[8]=bt.domain.slot;native_bluetooth_stats[9]=bt.domain.bank;
            native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
            native_bluetooth_stats[21]=bt.powered;
            for(unsigned i=0;i<4;i++)native_bluetooth_stats[22+i]=bt.geni.fault_irq[i];
            native_bluetooth_stats[26]=stats.malformed;native_bluetooth_stats[27]=stats.unmatched_events;
            native_bluetooth_stats[28]=bt.geni.stats.rx_dma;native_bluetooth_stats[29]=bt.geni.stats.tx_dma;
            native_bluetooth_stats[15]=bt.geni.stats.line_errors;native_bluetooth_stats[16]=bt.geni.stats.dma_errors;
            health=t+1000000000ULL;
        }
        uint64_t deadline=bt.powered?qbt_deadline(bt.core):UINT64_MAX;
        uint64_t geni=bt.powered && !bt.gated?qbt_geni_deadline(&bt.geni):UINT64_MAX;
        if(geni<deadline)deadline=geni;if(health<deadline)deadline=health;if(closeAt && closeAt<deadline)deadline=closeAt;
        if(!deadline){thread_yield_to_preemption();continue;}
        waitUntil(nullptr,deadline);
    }
}
}
extern "C" void picasso_bluetooth_probe(void) {
    unsigned enabled=1;PE_parse_boot_argn("picasso-bluetooth",&enabled,sizeof(enabled));
    if(!enabled)return;
    uint32_t expected=0;if(!__atomic_compare_exchange_n(&bt.started,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    bt.log=1;
    if(!address()){printf("PICASSO_BT device-tree identity unavailable\n");return;}
    thread_t thread;
    kern_return_t e=kernel_thread_start(worker,nullptr,&thread);
    if(e==KERN_SUCCESS)thread_deallocate(thread);
    printf("PICASSO_BT worker start=%x\n",e);
}

extern "C" void picasso_bt_runtime_result(int error) {
    __atomic_store_n(&bt.runtime,error?-error:1,__ATOMIC_RELEASE);
    wake(nullptr);
}
