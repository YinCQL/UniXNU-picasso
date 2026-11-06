// SPDX-License-Identifier: BSD-2-Clause-Patent
// First ADSP bring-up stage following unixnu.com/porting/power. Picasso uses
// APR (apr_audio_svc), not the reference SM8550 AudioReach/GPR firmware.
// Observe reserved SMEM only. No LPASS MMIO, PAS call, SMEM allocation or IRQ
// acknowledgement is valid until clock/rail/mailbox ownership is implemented.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include "PicassoAdsp.h"
extern "C" void picasso_adsp_poll(void);

static int adspRead(void *context,uint32_t offset,void *out,uint32_t size) {
    const volatile uint8_t *source=static_cast<const volatile uint8_t *>(context);
    for (uint32_t i=0;i<size;++i) static_cast<uint8_t *>(out)[i]=source[offset+i];
    return 0;
}

// Called by the existing one-second board worker: three bounded samples, no
// extra worker, handshake waits or retained mapping.
extern "C" void picasso_adsp_poll(void) {
    static unsigned sample;
    if (sample>30) return;
    unsigned tick=sample++;
    if (tick!=0 && tick!=5 && tick!=30) return;
    IODeviceMemory *memory=IODeviceMemory::withRange(WLAN_SMEM_BASE,WLAN_SMEM_SIZE);
    if (!memory) {IOLog("PICASSO_ADSP sample=%u map=-12\n",tick);return;}
    OSSharedPtr<IOMemoryMap> map(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    memory->release();
    uintptr_t address=map ? static_cast<uintptr_t>(map->getVirtualAddress()) : 0;
    if (!address) {IOLog("PICASSO_ADSP sample=%u map=-12\n",tick);return;}
    struct wlan_smem_io io={reinterpret_cast<void *>(address),adspRead};
    struct picasso_adsp_snapshot s;
    picasso_adsp_survey(&io,&s);
    IOLog("PICASSO_ADSP sample=%u partition_error=%d partition=%x size=%x in_error=%d slave=%x out_error=%d master=%x\n",
        tick,s.partition_error,s.partition.offset,s.partition.size,s.inbound_error,s.inbound,s.outbound_error,s.outbound);
    IOLog("PICASSO_ADSP observed ready=%u fatal=%u handover=%u stop_ack=%u glink_error=%d fifo=%x/%x/%x/%x authenticated=0 channel_open=0\n",
        !s.inbound_error && !!(s.inbound&2),!s.inbound_error && !!(s.inbound&1),
        !s.inbound_error && !!(s.inbound&4),!s.inbound_error && !!(s.inbound&8),
        s.glink_error,s.fifo[0],s.fifo[1],s.fifo[2],s.fifo[3]);
    IOLog("PICASSO_ADSP crash_error=%d previous_reason=%s\n",s.crash_error,s.crash);
    if (!tick) IOLog("PICASSO_ADSP pending=power-mailbox-pas firmware=adsp pas=1 carveout=89200000+2800000 protocol=apr channel=apr_audio_svc pcm=0\n");
}
