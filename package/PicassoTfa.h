// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_TFA_H
#define PICASSO_TFA_H
#include <stdint.h>
// Stock lito-qupv3, gcc-lito, pinctrl-lito, i2c-qcom-geni and TFA9874.
// Single worker, bounded FIFO transactions. Probe keeps the amp off; playback
// uses a bounded speaker lease. No OTP programming or calibration execution.
struct PicassoTfa {
    void (*probe)(PicassoTfa &);
    uint32_t stage,events,fw,ifDisable,hw[2],clockCfg,clockLevel,irq,rxStatus,ios;
    uint32_t pinBefore[3],pinAfter[3],dirty,commands,reads,reg,abortStatus;
    uint16_t revision,values[9],digital[3];
    uint32_t registerWrites,digitalDirty;
    bool digitalAttempted,digitalApplied,digitalRestored;
    int digitalError,digitalRestoreError;
    bool attempted,identified,complete,restored,uncertain;
    int error,cleanupError;
};
struct PicassoTfaIo {
    void *context;
    int (*read)(void *,unsigned,unsigned,uint32_t *);
    int (*write)(void *,unsigned,unsigned,uint32_t);
    void (*delay)(void *,unsigned);
};
// Regions: SE8, GCC QUP1, GCC shared core, GCC votes, GPIO49/50/58 WEST.
inline uint64_t picasso_tfa_base(unsigned i) {
    const uint64_t base[]={0x988000,0x118000,0x123000,0x152000,0xf531000,0xf532000,0xf53a000};return base[i];
}
inline unsigned picasso_tfa_register(unsigned i) {
    const unsigned regs[]={0,1,2,4,0x20,0x21,0x22,0x23,0x26};return regs[i];
}
struct PicassoTfaSession {
    const PicassoTfaIo &io;PicassoTfa &out;
    uint32_t pins[3][2],saved[20],pinDirty,configDirty;
    bool voteDirty,command,stretch,prepared;
    int rd(unsigned bank,unsigned reg,uint32_t &v) { return io.read(io.context,bank,reg,&v); }
    int wr(unsigned bank,unsigned reg,uint32_t v) { return io.write(io.context,bank,reg,v); }
    void delay(unsigned us) { io.delay(io.context,us); }
    int update(unsigned bank,unsigned reg,uint32_t mask,uint32_t v) {
        uint32_t old=0,back=0;int e=rd(bank,reg,old);if(e)return e;
        e=wr(bank,reg,(old&~mask)|(v&mask));if(e)return e;
        e=rd(bank,reg,back);return e?e:((back&mask)==(v&mask)?0:-5);
    }
};
inline unsigned picasso_tfa_se_reg(unsigned i) {
    const unsigned regs[]={0xe1c,0xe18,0x28,0xe30,0x24,0x258,0x260,0x264,0x284,0x288,0x254,0x810,0x814,0x614,0x7c,0x48,0x278,0x80c,0x26c,0x270};return regs[i];
}
inline int picasso_tfa_wait(PicassoTfaSession &s,unsigned bank,unsigned reg,uint32_t mask,uint32_t wanted) {
    for(unsigned n=0;n<1000;++n) {
        uint32_t v=0;int e=s.rd(bank,reg,v);if(e)return e;
        if((v&mask)==wanted)return 0;s.delay(10);
    }
    return -60;
}
inline int picasso_tfa_command(PicassoTfaSession &s,unsigned op,unsigned reg,uint16_t *value=nullptr,bool writeValue=false) {
    auto &o=s.out;o.reg=reg;uint32_t status=0;int e=s.rd(0,0x610,status);if(e)return e;
    if(status && (e=s.wr(0,0x618,status)))return e;
    if(op==1)e=s.wr(0,0x26c,writeValue?3:1);else if(op==2)e=s.wr(0,0x270,2);
    if(e)return e;
    // Separate WRITE with STOP_STRETCH, then READ: same repeated START as Linux.
    s.command=true;o.uncertain=true;
    e=s.wr(0,0x600,(op<<27)|(0x34U<<9)|(op==1 && !writeValue?4:0));if(e)return e;
    ++o.commands;bool tx=false,rx=false;
    for(unsigned n=0;n<1000;++n) {
        e=s.rd(0,0x610,status);o.irq=status;if(e)return e;
        if(status&0x33007e3e) { // FIFO errors and command/GP error causes
            if(status&(1U<<10))return -107; // NACK
            if(status&(1U<<13))return -16;  // arbitration lost
            return -5;
        }
        if(op==1 && !tx && (status&(1U<<30))) { uint32_t word=reg;if(writeValue)word|=uint32_t(*value>>8)<<8|uint32_t(*value&255)<<16;
            e=s.wr(0,0x700,word);if(e)return e;tx=true; }
        if(op==2 && !rx && (status&((1U<<26)|(1U<<27)|1))) {
            uint32_t count=0,word=0;e=s.rd(0,0x804,count);o.rxStatus=count;if(e)return e;
            if(count&0x1ffffff) {
                if((count&0x1ffffff)!=1 || !(count>>31) || ((count>>28)&7)!=2)return -90;
                e=s.rd(0,0x780,word);if(e)return e;
                if(value)*value=uint16_t(((word&255)<<8)|((word>>8)&255));rx=true;
            }
        }
        if(status && (e=s.wr(0,0x618,status)))return e;
        if(status&1) {
            if((op==1 && !tx) || (op==2 && !rx))return -90;
            e=picasso_tfa_wait(s,0,0x40,1,0);if(e)return e;
            s.command=false;s.stretch=op==1 && !writeValue;o.uncertain=s.stretch;return 0;
        }
        s.delay(10);
    }
    return -60;
}
inline int picasso_tfa_read_register(PicassoTfaSession &s,unsigned reg,uint16_t &v) {
    int e=picasso_tfa_command(s,1,reg);if(!e)e=picasso_tfa_command(s,2,reg,&v);
    if(!e)++s.out.reads;return e;
}
inline int picasso_tfa_write_register(PicassoTfaSession &s,unsigned reg,uint16_t value) {
    ++s.out.registerWrites;return picasso_tfa_command(s,1,reg,&value,true);
}
inline int picasso_tfa_write_verify(PicassoTfaSession &s,unsigned reg,uint16_t value) {
    int e=picasso_tfa_write_register(s,reg,value);uint16_t back=0;
    if(!e)e=picasso_tfa_read_register(s,reg,back);return e?e:back==value?0:-84;
}
inline void picasso_tfa_digital(PicassoTfaSession &s) {
    auto &o=s.out;o.digitalAttempted=true;o.stage=7;
    // Verified Picasso revision. Keep POR powerdown, amplifier and boost off.
    // Stock tfa_dev_set_tdm_bitwidth(16): TDME off, NBCK0/SLLN15/SSIZE15, on.
    if(o.revision!=0x0c74 || (o.values[0]&0x19)!=1) { o.digitalError=-95;return; }
    const uint16_t original[]={o.values[4],o.values[5],o.values[6]};
    o.digital[0]=uint16_t((original[0]&~0xf000U)|0x10);
    o.digital[1]=uint16_t((original[1]&~0x1f0U)|0xf0);
    o.digital[2]=uint16_t((original[2]&~0x7cU)|0x3c);
    o.digitalDirty=1;
    int e=picasso_tfa_write_verify(s,0x20,uint16_t(original[0]&~0x10U));
    for(unsigned i=0;i<3 && !e;++i) {
        o.digitalDirty|=1U<<i;
        e=picasso_tfa_write_verify(s,0x20+i,i?o.digital[i]:uint16_t(o.digital[i]&~0x10U));
    }
    if(!e)e=picasso_tfa_write_verify(s,0x20,o.digital[0]);
    uint16_t power=0;
    if(!e)e=picasso_tfa_read_register(s,0,power);
    if(!e && (power&0x19)!=1)e=-84;
    o.digitalApplied=!e;o.digitalError=e;
}
inline void picasso_tfa_digital_restore(PicassoTfaSession &s) {
    auto &o=s.out;if(!o.digitalDirty)return;
    if(o.uncertain) { o.digitalRestoreError=-16;return; }
    // Disable the interface before restoring its fields; never enable after
    // a partial restore. Final full readback is the only restored evidence.
    int e=picasso_tfa_write_verify(s,0x20,uint16_t(o.values[4]&~0x10U));
    for(unsigned n=3;n>1 && !e;--n) {
        unsigned i=n-1;e=picasso_tfa_write_verify(s,0x20+i,o.values[4+i]);
    }
    if(!e)e=picasso_tfa_write_verify(s,0x20,o.values[4]);
    o.digitalRestoreError=e;o.digitalRestored=!e;
    if(!e)o.digitalDirty=0;
}
inline int picasso_tfa_prepare(PicassoTfaSession &s) {
    auto &o=s.out;o.stage=1;
    // Touch's already-established wrapper votes must exist and stay running.
    // Own only SE8/S2 bit24, never remove another QUP client's votes.
    uint32_t votes=0;int e=s.rd(3,8,votes);if(e)return e;
    if((votes&0x3c0000)!=0x3c0000)return -16;
    const unsigned banks[]={1,1,2,2},regs[]={4,8,0x138,0x140};
    for(unsigned i=0;i<4;++i) {
        uint32_t v=0;e=s.rd(banks[i],regs[i],v);if(e)return e;
        if((v&0x80000000) && !(i==1 && (v&2)))return -16;
    }
    if(!(votes&(1U<<24))) { s.voteDirty=true;e=s.update(3,8,1U<<24,1U<<24);if(e)return e; }
    e=picasso_tfa_wait(s,1,0x26c,0x80000000,0);if(e)return e;
    o.stage=2;
    if((e=s.rd(0,0x68,o.fw)) || (e=s.rd(0,0x64,o.ifDisable)) ||
       (e=s.rd(0,0xe24,o.hw[0])) || (e=s.rd(0,0xe28,o.hw[1])))return e;
    if(((o.fw>>8)&255)!=3)return -93;
    if(o.ifDisable)return -95;
    if(((o.hw[0]>>24)&63)!=32 || ((o.hw[0]>>16)&63)<4 || ((o.hw[1]>>16)&63)<4)return -19;
    uint32_t v=0;if((e=s.rd(0,0x40,v)))return e;if(v&0x1001)return -16;
    if((e=s.rd(0,0x800,v)))return e;if(v&0xfffffff)return -16;
    if((e=s.rd(0,0x804,v)))return e;if(v&0x1ffffff)return -16;
    for(unsigned i=0;i<20;++i)if((e=s.rd(0,picasso_tfa_se_reg(i),s.saved[i])))return e;
    if(s.saved[1] || s.saved[5])return -16; // GSI/DMA owner
    // Use only a firmware-provided XO 19.2MHz level; do not reprogram RCG/PLL.
    uint32_t dfs=0;if((e=s.rd(1,0x284,dfs)))return e;
    o.clockLevel=0xffffffff;
    for(unsigned i=0;i<(dfs&1?8U:1U);++i) {
        uint32_t cfg=0;e=s.rd(1,dfs&1?0x28c+4*i:0x274,cfg);if(e)return e;
        if(!(cfg&0x3700) && (cfg&31)<=1) { o.clockLevel=i;o.clockCfg=cfg;break; }
    }
    if(o.clockLevel==0xffffffff)return -34;
    o.stage=3;
    for(unsigned i=0;i<3;++i) {
        if((e=s.rd(4+i,0,s.pins[i][0])) || (e=s.rd(4+i,4,s.pins[i][1])))return e;
        o.pinBefore[i]=s.pins[i][0];unsigned mux=(s.pins[i][0]>>2)&15;
        if(i==2?mux!=0:(mux!=0 && mux!=2))return -16;
    }
    // Poll engine status without routing its interrupts to the GIC.
    const uint32_t values[]={0,0,s.saved[2]|0x7f,s.saved[3]|15,0x7f,0,
        0x7f8fe,0xffefe,0x7f8fe,0xffefe,0,1,2,0x7fc07e3f,o.clockLevel,0x71,0xa02c1a,1,s.saved[18],s.saved[19]};
    for(unsigned i=0;i<20;++i) {
        s.configDirty|=1U<<i;e=s.update(0,picasso_tfa_se_reg(i),0xffffffff,values[i]);if(e)return e;
    }
    if((e=s.wr(0,0x20,1)))return e;
    s.prepared=true;
    for(unsigned i=0;i<3;++i) {
        s.pinDirty|=1U<<i;uint32_t cfg=i==2?0x201:8;
        if(s.pins[i][0]&(1U<<11))cfg|=1U<<12;
        // Set reset latch low before enabling GPIO output; I2C OE stays stock.
        if(i==2 && (e=s.update(6,4,2,0)))return e;
        e=s.update(4+i,0,i==2?0x13ff:0x11ff,cfg);if(e)return e;
        if((e=s.rd(4+i,0,o.pinAfter[i])))return e;
    }
    o.stage=4;
    // Default HIGH polarity in stock tfa98xx_ext_reset: high10ms, low10ms.
    if((e=s.update(6,4,2,2)))return e;s.delay(10000);
    if((e=s.update(6,4,2,0)))return e;s.delay(10000);
    if((e=s.rd(0,0x908,o.ios)))return e;
    if((o.ios&3)!=3)return -16;
    return 0;
}
inline void picasso_tfa_release_bus(PicassoTfaSession &s) {
    auto &o=s.out;int e=0;
    // A lost DONE may leave the sequencer or repeated-START bus owned. Abort,
    // then STOP; never infer release from elapsed time or a register read.
    if(s.command || s.stretch) {
        if(s.command) {
            e=s.wr(0,0x604,2);
            if(!e)e=picasso_tfa_wait(s,0,0x610,1U<<5,1U<<5);
            if(!e)e=s.rd(0,0x610,o.abortStatus);
            if(!e)e=s.wr(0,0x618,o.abortStatus);
            if(!e)e=picasso_tfa_wait(s,0,0x40,1,0);
        }
        if(!e)e=picasso_tfa_command(s,7,0);
        if(!e) { s.command=s.stretch=false;o.uncertain=false; }
        else o.cleanupError=e;
    }
}
inline void picasso_tfa_cleanup(PicassoTfaSession &s) {
    auto &o=s.out;int e=0;
    picasso_tfa_release_bus(s);
    picasso_tfa_digital_restore(s);
    if(o.digitalRestoreError && !o.cleanupError)o.cleanupError=o.digitalRestoreError;
    picasso_tfa_release_bus(s); // Restore may itself lose a command ACK.
    for(unsigned n=3;n>0;--n) {
        unsigned i=n-1;if(!(s.pinDirty&(1U<<i)))continue;
        e=s.update(4+i,0,1U<<9,0);
        if(!e)e=s.update(4+i,4,2,s.pins[i][1]);
        if(!e)e=s.update(4+i,0,0x13ff,s.pins[i][0]);
        if(e) { if(!o.cleanupError)o.cleanupError=e; }else s.pinDirty&=~(1U<<i);
    }
    if(s.prepared && !o.uncertain) {
        uint32_t status=0,count=0;
        e=s.rd(0,0x804,count);
        if(!e && (count&0x1ffffff)) {
            if((count&0x1ffffff)>64)e=-90;
            else for(unsigned i=0;i<(count&0x1ffffff) && !e;++i)e=s.rd(0,0x780,status);
        }
        if(!e)e=s.rd(0,0x610,status);
        if(!e && status)e=s.wr(0,0x618,status);
        if(e) { o.uncertain=true;if(!o.cleanupError)o.cleanupError=e; }
    }
    // Keep engine config/clock if its stop could not be confirmed.
    if(!o.uncertain) {
        for(unsigned n=20;n>0;--n) {
            unsigned i=n-1;if(!(s.configDirty&(1U<<i)))continue;
            // Do not reconnect IRQ routing while another SE setting is dirty.
            if(i==0 && (s.configDirty&~1U))continue;
            e=s.update(0,picasso_tfa_se_reg(i),0xffffffff,s.saved[i]);
            if(e) { if(!o.cleanupError)o.cleanupError=e; }else s.configDirty&=~(1U<<i);
        }
        if(s.voteDirty && !s.configDirty) {
            e=s.update(3,8,1U<<24,0);
            if(e) { if(!o.cleanupError)o.cleanupError=e; }else s.voteDirty=false;
        }
    }
    o.dirty=(s.pinDirty?1:0)|(s.configDirty?2:0)|(s.voteDirty?4:0)|(o.digitalDirty?8:0);
    o.restored=!o.dirty && !o.uncertain;++o.events;
}
inline void picasso_tfa_run(const PicassoTfaIo &io,PicassoTfa &o) {
    if(o.attempted)return;o.attempted=true;++o.events;
    if(!io.read || !io.write || !io.delay) { o.error=-22;return; }
    PicassoTfaSession s={io,o,{},{},0,0,false,false,false,false};
    o.error=picasso_tfa_prepare(s);
    if(!o.error) {
        o.stage=5;uint16_t again=0;
        o.error=picasso_tfa_read_register(s,3,o.revision);
        if(!o.error)o.error=picasso_tfa_read_register(s,3,again);
        if(!o.error && (o.revision!=again || (o.revision&255)!=0x74))o.error=-19;
        o.identified=!o.error;
    }
    if(!o.error) {
        o.stage=6;
        for(unsigned i=0;i<9 && !o.error;++i)o.error=picasso_tfa_read_register(s,picasso_tfa_register(i),o.values[i]);
        o.complete=!o.error;
    }
    if(o.complete)picasso_tfa_digital(s);
    picasso_tfa_cleanup(s);
}
// One playback lease owns SE8 and the TFA format until drain or any error.
// Tables: Picasso tfa98xx.cnt SHA256 00d119e7b87a4fd2732d166f1292d43c1418d26a3328eae5a703c6f395a3e221;
// tfa_init.c revision 0c74 V1.16 and tfaRunStartup/INIT_CF/OPERATING.
// No OTP copy/program command, calibration execution or protection bypass.
struct PicassoTfaPlayback {
    uint32_t stage,registers,polls;
    uint16_t status[4],control[3]; // 10,11,13,14 and 00,01,02
    bool attempted,configured,clockReady,enabled,wasEnabled,stopped,resetSafe;
    int error,stopError;
};
inline int picasso_tfa_field(PicassoTfaSession &s,unsigned reg,uint16_t mask,uint16_t value) {
    uint16_t old=0;int e=picasso_tfa_read_register(s,reg,old);
    return e?e:picasso_tfa_write_verify(s,reg,uint16_t((old&~mask)|(value&mask)));
}
inline int picasso_tfa_play_status(PicassoTfaSession &s,PicassoTfaPlayback &a,bool operating) {
    const unsigned regs[]={0x10,0x11,0x13,0x14};
    for(unsigned i=0;i<4;++i) {int e=picasso_tfa_read_register(s,regs[i],a.status[i]);if(e)return e;}
    // Stock field names are otpOK/uvpOK/ovpOK: these bits must be ONE.
    // NOCLK is sticky from reset; use current CLKS; reject alarm/TDM faults.
    if((a.status[0]&0x68) || (a.status[0]&0x16)!=0x16 || (a.status[1]&0x100) || !(a.status[2]&0x100))return -5;
    if(!(a.status[1]&0x80))return -11;
    if(operating && (!(a.status[1]&0x4000) || !(a.status[2]&0x8000)))return -11;
    return 0;
}
inline int picasso_tfa_play_prepare(PicassoTfaSession &s,PicassoTfaPlayback &a) {
    if(a.attempted)return -16;a.attempted=true;a.stage=1;
    auto &o=s.out;o.attempted=true;
    int e=picasso_tfa_prepare(s);uint16_t id=0;
    if(!e)e=picasso_tfa_read_register(s,3,o.revision);
    if(!e)e=picasso_tfa_read_register(s,3,id);
    if(!e && (id!=0x0c74 || o.revision!=id))e=-19;
    for(unsigned i=0;i<9 && !e;++i)e=picasso_tfa_read_register(s,picasso_tfa_register(i),o.values[i]);
    if(!e && (o.values[0]&0x19)!=1)e=-16;
    if(e)return a.error=e;
    a.stage=2;
    // Original key sequence. These unlock volatile register/shadow access only;
    // never write A3/A7/A8 (OTP programming) or F0/F4 calibration data.
    uint16_t key=0;e=picasso_tfa_write_register(s,0x0f,0x5a6b);
    if(!e)e=picasso_tfa_read_register(s,0xfb,key);
    if(!e)e=picasso_tfa_write_register(s,0xa0,key^0x5a);
    if(!e)e=picasso_tfa_write_register(s,0xa1,0x5a);
    const uint16_t optimal[][2]={{2,0x22c8},{0x52,0x57dc},{0x53,0x003e},{0x56,0x0400},
        {0x61,0x0110},{0x6f,0x00a5},{0x70,0x07f8},{0x73,0x0047},{0x74,0x5098},
        {0x75,0x8d28},{0x80,0},{0x83,0x0799},{0x84,0x0081}};
    for(const auto &v:optimal)if(!e) {e=picasso_tfa_write_verify(s,v[0],v[1]);if(!e)++a.registers;}
    if(!e) {picasso_tfa_digital(s);e=o.digitalError;}
    // Stock device list, then speaker profile up to its default boundary.
    // F8 changes only the volatile temperature-gain shadow; reset discards it.
    const uint16_t profile[][3]={
        {0x21,0x000f,0x0001},
        {0x23,0x0008,0x0008},
        {0x23,0x0010,0x0010},
        {0x26,0x000f,0x0000},
        {0x26,0xf000,0x1000},
        {0x27,0x000f,0x0000},
        {0x68,0x0007,0x0002},
        {0x68,0x0018,0x0008},
        {0x68,0x0060,0x0040},
        {0x02,0x07e0,0x01e0},
        {0x64,0xc000,0x4000},
        {0x74,0x0008,0x0008},
        {0x88,0x0003,0x0002},
        {0x74,0x3e00,0x1000},
        {0x8f,0x0001,0x0000},
        {0x74,0x8000,0x0000},
        {0x70,0x0180,0x0180},
        {0xc4,0x2000,0x0000},
        {0x66,0x0001,0x0000},
        {0x58,0x01f0,0x0010},
        {0x6f,0x0380,0x0080},
        {0x85,0x0002,0x0002},
        {0x84,0x01e0,0x0080},
        {0x6f,0x0020,0x0020},
        {0x83,0x003f,0x0015},
        {0x83,0x07c0,0x0700},
        {0xf8,0xe000,0x6000},
        {0x61,0x03c0,0x0100},
        {0x61,0x003c,0x0010},
        {0x74,0x01f0,0x0090},
        {0x75,0x1f00,0x0d00},
        {0x76,0x7e00,0x7e00},
        {0x00,0x0010,0x0010},
        {0x64,0xc000,0x4000},
        {0x62,0xc000,0x0000},
        {0x02,0x000f,0x0008}
    };
    if(!e)e=picasso_tfa_write_register(s,0x0f,0x5a6b);
    if(!e)e=picasso_tfa_write_register(s,0xa1,0x5a);
    if(!e)e=picasso_tfa_write_register(s,0xa0,0x5a);
    for(const auto &v:profile)if(!e) {e=picasso_tfa_field(s,v[0],v[1],v[2]);if(!e)++a.registers;}
    // Relock keys even if an earlier command failed; preserve the first error.
    const unsigned locks[]={0xa1,0xa0,0x0f};
    if(!o.uncertain)for(unsigned reg:locks) {
        int lock=picasso_tfa_write_register(s,reg,0);if(!e)e=lock;
    }
    if(e)return a.error=e;
    a.stage=3;
    // Probus INIT_CF: I2C configured, then power up with AMPE still zero.
    e=picasso_tfa_field(s,1,4,4);
    if(!e)e=picasso_tfa_field(s,0,9,0);
    for(unsigned n=0;n<50 && !e;++n) {
        ++a.polls;int health=picasso_tfa_play_status(s,a,false);
        if(!health) {a.clockReady=true;break;}
        if(health!=-11) {e=health;break;}s.delay(1000);
    }
    if(!e && !a.clockReady)e=-60;
    for(unsigned i=0;i<3 && !e;++i)e=picasso_tfa_read_register(s,i,a.control[i]);
    a.configured=!e;a.error=e;return e;
}
inline int picasso_tfa_play_enable(PicassoTfaSession &s,PicassoTfaPlayback &a) {
    if(!a.configured || a.enabled || a.error || a.stopped)return -22;
    a.stage=4;int e=picasso_tfa_play_status(s,a,false);
    if(!e)e=picasso_tfa_field(s,0,1,0);
    if(!e)e=picasso_tfa_field(s,1,4,4);
    // The stock no-DSP mute callback is a no-op. Track the real AMPE write.
    if(!e) {a.wasEnabled=true;e=picasso_tfa_field(s,0,8,8);}
    bool ready=false;
    for(unsigned n=0;n<50 && !e;++n) {
        ++a.polls;int health=picasso_tfa_play_status(s,a,true);
        if(!health) {ready=true;break;}
        if(health!=-11) {e=health;break;}s.delay(1000);
    }
    if(!e && !ready)e=-60;
    if(!e)e=picasso_tfa_read_register(s,0,a.control[0]);
    a.enabled=!e;a.error=e;return e;
}
inline int picasso_tfa_play_finish(PicassoTfaSession &s,PicassoTfaPlayback &a) {
    if(!a.attempted || a.stopped)return a.stopError;
    bool resetRequired=!!(s.pinDirty&4);picasso_tfa_release_bus(s);int e=0;
    if(!s.out.uncertain && a.configured) {
        e=picasso_tfa_field(s,0,0x19,1); // AMPE/DCA off, PWDN on
        bool idle=false;
        for(unsigned n=0;n<50 && !e;++n) {
            uint16_t state=0;e=picasso_tfa_read_register(s,0x14,state);a.status[3]=state;
            if(!e && ((state>>3)&15)<4) {idle=true;break;}s.delay(1000);
        }
        if(!e && !idle)e=-60;
    }
    // A hardware reset also discards profile and all volatile trim shadows.
    // If I2C was lost, GPIO readback still proves that reset was asserted.
    if(s.pinDirty&4) {
        // Verify GPIO mode/OE as well as the latch: a failed prepare may have
        // marked the pin dirty before it actually became an output.
        uint32_t cfg=0;int reset=s.rd(6,0,cfg);
        if(!reset)reset=s.update(6,0,0x13ff,0x201|((cfg&(1U<<11))?1U<<12:0));
        if(!reset)reset=s.update(6,4,2,2);
        if(!reset) {s.delay(10000);a.resetSafe=true;a.enabled=false;
            reset=s.update(6,4,2,0);if(!reset)s.delay(10000);}
        if(reset && !e)e=reset;
        if(a.resetSafe)s.out.digitalDirty=0;
    }
    if(resetRequired && !a.resetSafe) {
        // Do not release the reset/I2C lease after an unconfirmed shutdown.
        s.out.dirty=(s.pinDirty?1:0)|(s.configDirty?2:0)|(s.voteDirty?4:0)|(s.out.digitalDirty?8:0);
        s.out.restored=false;a.stopError=e?e:-5;return a.stopError;
    }
    picasso_tfa_cleanup(s);
    if(s.out.cleanupError && !e)e=s.out.cleanupError;
    a.stopped=(!resetRequired || a.resetSafe) && s.out.restored;
    a.stopError=e?e:a.stopped?0:-5;return a.stopError;
}

#endif
