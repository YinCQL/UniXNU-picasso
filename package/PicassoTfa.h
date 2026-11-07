// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_TFA_H
#define PICASSO_TFA_H
#include <stdint.h>
// Stock lito-qupv3, gcc-lito, pinctrl-lito, i2c-qcom-geni and TFA9874.
// Single worker, FIFO-only register reads. No amp enable, MTP or calibration writes.
struct PicassoTfa {
    void (*probe)(PicassoTfa &);
    uint32_t stage,events,fw,ifDisable,hw[2],clockCfg,clockLevel,irq,rxStatus,ios;
    uint32_t pinBefore[3],pinAfter[3],dirty,commands,reads,reg,abortStatus;
    uint16_t revision,values[9];
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
inline int picasso_tfa_command(PicassoTfaSession &s,unsigned op,unsigned reg,uint16_t *value=nullptr) {
    auto &o=s.out;o.reg=reg;uint32_t status=0;int e=s.rd(0,0x610,status);if(e)return e;
    if(status && (e=s.wr(0,0x618,status)))return e;
    if(op==1)e=s.wr(0,0x26c,1);else if(op==2)e=s.wr(0,0x270,2);
    if(e)return e;
    // Separate WRITE with STOP_STRETCH, then READ: same repeated START as Linux.
    s.command=true;o.uncertain=true;
    e=s.wr(0,0x600,(op<<27)|(0x34U<<9)|(op==1?4:0));if(e)return e;
    ++o.commands;bool tx=false,rx=false;
    for(unsigned n=0;n<1000;++n) {
        e=s.rd(0,0x610,status);o.irq=status;if(e)return e;
        if(status&0x33007e3e) { // FIFO errors and command/GP error causes
            if(status&(1U<<10))return -107; // NACK
            if(status&(1U<<13))return -16;  // arbitration lost
            return -5;
        }
        if(op==1 && !tx && (status&(1U<<30))) { e=s.wr(0,0x700,reg);if(e)return e;tx=true; }
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
            s.command=false;s.stretch=op==1;o.uncertain=s.stretch;return 0;
        }
        s.delay(10);
    }
    return -60;
}
inline int picasso_tfa_read_register(PicassoTfaSession &s,unsigned reg,uint16_t &v) {
    int e=picasso_tfa_command(s,1,reg);if(!e)e=picasso_tfa_command(s,2,reg,&v);
    if(!e)++s.out.reads;return e;
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
inline void picasso_tfa_cleanup(PicassoTfaSession &s) {
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
    o.dirty=(s.pinDirty?1:0)|(s.configDirty?2:0)|(s.voteDirty?4:0);
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
    picasso_tfa_cleanup(s);
}
#endif
