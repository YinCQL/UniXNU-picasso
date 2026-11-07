// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoTfa.h"
static uint32_t mem[7][4096],original[7][4096];
static unsigned reads,writes,failRead,failWrite,dropWrite,delays,op,currentReg,commands,injectCommand;
static unsigned nack,arb,timeout,shortRead,overflow,badId,unstable,abortFail,stopFail;
static bool heldBus,active,irqAsserted,realRevision,playbackMode,noClocks,noOperating;
static unsigned resets,statusFailure;
static uint16_t amp[256];static unsigned ampWrites,failAmpWrite,writeControl;
static int rd(void *,unsigned bank,unsigned reg,uint32_t *v) {
    assert(bank<7 && !(reg&3));
    if(++reads==failRead)return -5;
    *v=mem[bank][reg/4];return 0;
}
static int wr(void *,unsigned bank,unsigned reg,uint32_t v) {
    assert(bank<7 && !(reg&3));++writes;
    if(writes==dropWrite)return 0;
    mem[bank][reg/4]=v;
    if(bank==3 && reg==8) { mem[1][0x26c/4]=(v&(1U<<24))?0:0x80000000;assert((v&0x3c0000)==0x3c0000); }
    if(playbackMode && bank==6 && reg==4 && (v&2)) {
        ++resets;amp[0]=1;amp[1]=0;amp[2]=0x25c8;
        amp[0x20]=0x2890;amp[0x21]=0xc1f1;amp[0x22]=0x43c;
    }
    if(bank==0 && reg==0xe1c && v)irqAsserted=true;
    if(bank==0 && reg==0x618)mem[0][0x610/4]&=~v;
    if(bank==0 && reg==0x604 && v==2) {
        mem[0][0x610/4]=abortFail?0:1U<<5;mem[0][0x40/4]=abortFail?1:0;active=!!abortFail;
    }
    if(bank==0 && reg==0x600) {
        op=v>>27;assert((v&0xfe00)==0x6800);assert(op==1 || op==2 || op==7);
        assert(mem[0][0x260/4]==0x7f8fe && mem[0][0x264/4]==0xffefe);
        assert(mem[0][0x284/4]==0x7f8fe && mem[0][0x288/4]==0xffefe);
        assert(mem[0][0x48/4]==0x71 && mem[0][0x278/4]==((10U<<20)|(11U<<10)|26));
        ++commands;active=true;mem[0][0x40/4]=1;
        if(commands==injectCommand && (nack || arb)) {
            mem[0][0x610/4]=8|(nack?1U<<10:1U<<13);return 0;
        }
        if((commands==injectCommand && timeout) || (op==7 && stopFail)){mem[0][0x610/4]=0;return 0;}
        if(op==1) {
            assert(((v&4) && mem[0][0x26c/4]==1) || (!(v&4) && mem[0][0x26c/4]==3));writeControl=v;mem[0][0x610/4]=1U<<30;
        } else if(op==2) {
            assert(heldBus && !(v&4) && mem[0][0x270/4]==2);
            if(playbackMode) {
                amp[0x10]=0x16;
                if(statusFailure==1)amp[0x10]&=~4U;
                if(statusFailure==2)amp[0x10]|=0x40;
                amp[0x11]=(!noClocks && !(amp[0]&1)?0x80:0)|(!noOperating && (amp[0]&8)?0x4000:0);
                amp[0x13]=0x10f|(!noOperating && (amp[0]&8)?0x8000:0);
                if(statusFailure==3)amp[0x13]&=~0x100U;
                amp[0x14]=(amp[0]&1)?0:((amp[0]&8)?9:4)<<3;
            }
            uint16_t val=realRevision?amp[currentReg]:currentReg==3?(badId?0x1234:unstable && commands==4?0x2a74:0x1a74):uint16_t(0x1200|currentReg);
            mem[0][0x780/4]=(val>>8)|((val&255)<<8);
            mem[0][0x804/4]=((commands==injectCommand && overflow)?2U:1U)|0x80000000|((commands==injectCommand && shortRead)?0x10000000:0x20000000);
            mem[0][0x610/4]=(1U<<27)|1;mem[0][0x40/4]=0;heldBus=active=false;
        } else {mem[0][0x610/4]=1;mem[0][0x40/4]=0;heldBus=active=false;}
    }
    if(bank==0 && reg==0x700) {
        assert(op==1);currentReg=v&255;active=false;
        if(mem[0][0x26c/4]==3) {
            assert(!(writeControl&4));
            if(playbackMode) {
                // No OTP programming/copy or calibration writes. F8 is a
                // volatile shadow in the original container, never committed.
                assert(currentReg!=0xa3 && currentReg!=0xa7 && currentReg!=0xa8 && currentReg!=0xf0 && currentReg!=0xf4);
            } else assert(currentReg>=0x20 && currentReg<=0x22);
            ++ampWrites;heldBus=false;uint16_t value=uint16_t(((v>>8)&255)<<8|((v>>16)&255));
            if(ampWrites!=failAmpWrite)amp[currentReg]=value;
        } else {assert(v<=255 && (writeControl&4));heldBus=true;}
        mem[0][0x40/4]=0;mem[0][0x610/4]=1;
    }
    return writes==failWrite?-5:0; // fail after the write reached hardware
}
static int readIo(void *c,unsigned b,unsigned reg,uint32_t *v) {
    int e=rd(c,b,reg,v);if(!e && b==0 && reg==0x780)mem[0][0x804/4]=0;return e;
}
static void delay(void *,unsigned us) {assert(us==10 || us==1000 || us==10000);delays+=us;}
static PicassoTfaIo io={nullptr,readIo,wr,delay};
static void setup() {
    memset(mem,0,sizeof mem);reads=writes=failRead=failWrite=dropWrite=delays=op=currentReg=commands=injectCommand=0;
    nack=arb=timeout=shortRead=overflow=badId=unstable=abortFail=stopFail=0;heldBus=active=irqAsserted=realRevision=false;
    ampWrites=failAmpWrite=writeControl=resets=statusFailure=0;playbackMode=noClocks=noOperating=false;memset(amp,0,sizeof amp);
    mem[3][2]=0xbc0000;mem[1][0x26c/4]=0x80000000;
    mem[0][0x68/4]=0x301;mem[0][0xe24/4]=0x20100000;mem[0][0xe28/4]=0x20100000;
    mem[0][0x908/4]=3;mem[1][0x284/4]=1;mem[1][0x28c/4]=1;
    for(unsigned i=4;i<7;++i) { mem[i][0]=0x2800;mem[i][1]=1; }
    memcpy(original,mem,sizeof mem);
}
static void checkRestored(const PicassoTfa &o) {
    assert(o.restored && !o.dirty && !o.uncertain);
    assert(mem[3][2]==original[3][2]);
    for(unsigned i=4;i<7;++i)assert(mem[i][0]==original[i][0] && (mem[i][1]&2)==(original[i][1]&2));
    for(unsigned i=0;i<20;++i)assert(mem[0][picasso_tfa_se_reg(i)/4]==original[0][picasso_tfa_se_reg(i)/4]);
}
static void realAmp() {
    setup();realRevision=true;const uint16_t regs[]={0,1,2,3,4,0x20,0x21,0x22,0x23,0x26};
    const uint16_t values[]={1,0,0x25c8,0x0c74,0,0x2890,0xc1f1,0x43c,1,0x10};
    for(unsigned i=0;i<10;++i)amp[regs[i]]=values[i];
}
static void digitalTests() {
    realAmp();PicassoTfa o={};picasso_tfa_run(io,o);
    assert(o.complete && o.digitalAttempted && o.digitalApplied && o.digitalRestored && !o.digitalDirty);
    assert(o.digital[0]==0x890 && o.digital[1]==0xc0f1 && o.digital[2]==0x43c);
    assert(o.registerWrites==9 && ampWrites==9 && o.reads==21 && o.commands==51);
    assert(amp[0]==1 && amp[0x20]==0x2890 && amp[0x21]==0xc1f1 && amp[0x22]==0x43c);checkRestored(o);
    unsigned readCount=reads,writeCount=writes;
    for(unsigned n=1;n<=readCount;++n) {
        realAmp();o={};failRead=n;picasso_tfa_run(io,o);
        assert(o.error || o.cleanupError || o.digitalError || o.digitalRestoreError);
        assert(o.restored || o.dirty || o.uncertain);assert(amp[0]==1);
    }
    for(unsigned n=1;n<=writeCount;++n) {
        realAmp();o={};failWrite=n;picasso_tfa_run(io,o);
        assert(o.error || o.cleanupError || o.digitalError || o.digitalRestoreError);
        assert(o.restored || o.dirty || o.uncertain);assert(amp[0]==1);
    }
    realAmp();o={};failAmpWrite=1;picasso_tfa_run(io,o);
    assert(o.digitalError==-84 && !o.digitalApplied && o.digitalRestored && ampWrites==5);checkRestored(o);
    // Refuse to configure an active amplifier or an unvalidated revision.
    realAmp();o={};amp[0]=0x19;picasso_tfa_run(io,o);assert(o.digitalError==-95 && !ampWrites);checkRestored(o);
    realAmp();o={};amp[3]=0x0b74;picasso_tfa_run(io,o);assert(o.digitalError==-95 && !ampWrites);checkRestored(o);
    puts("PASS real c74 snapshot: bounded S16 TDM writes, endian/readback, power gate, all I/O failures and inverse restore");
}
static void amplifierTests() {
    realAmp();playbackMode=true;
    PicassoTfa o={};PicassoTfaPlayback a={};PicassoTfaSession session={io,o,{},{},0,0,false,false,false,false};
    assert(!picasso_tfa_play_prepare(session,a));
    assert(a.configured && a.clockReady && !a.enabled && !(amp[0]&8) && (amp[0]&0x10));
    assert(amp[0x20]==0x890 && amp[0x21]==0xc0f1 && amp[0x22]==0x43c);
    assert(amp[2]==0x21e8 && amp[0x83]==0x715 && amp[0x76]==0x7e00 && a.registers==49);
    assert(!amp[0xa1] && !amp[0xa0] && !amp[0xf]);
    assert(!picasso_tfa_play_enable(session,a) && a.enabled && a.wasEnabled && (amp[0]&0x19)==0x18);
    assert(!picasso_tfa_play_status(session,a,true));
    assert(!picasso_tfa_play_finish(session,a));
    assert(a.stopped && a.resetSafe && !a.enabled && resets==2 && amp[0]==1);checkRestored(o);
    unsigned totalReads=reads,totalWrites=writes;
    for(unsigned mode=0;mode<4;++mode) {
        realAmp();playbackMode=true;o={};a={};
        PicassoTfaSession s={io,o,{},{},0,0,false,false,false,false};
        if(mode==0)amp[3]=0xb74;
        if(mode==1)noClocks=true;
        int e=picasso_tfa_play_prepare(s,a);
        if(mode==2)noOperating=true;
        if(!e)e=picasso_tfa_play_enable(s,a);
        if(mode==3) {injectCommand=commands+1;timeout=1;}
        int cleanup=picasso_tfa_play_finish(s,a);
        assert(mode==3?(!e && cleanup):e);
        assert(a.stopped && a.resetSafe && amp[0]==1);checkRestored(o);
    }
    for(unsigned fault=1;fault<=3;++fault) {
        realAmp();playbackMode=true;o={};a={};PicassoTfaSession s={io,o,{},{},0,0,false,false,false,false};
        assert(!picasso_tfa_play_prepare(s,a) && !picasso_tfa_play_enable(s,a));
        statusFailure=fault;assert(picasso_tfa_play_status(s,a,true)==-5);
        assert(!picasso_tfa_play_finish(s,a) && a.stopped && amp[0]==1);
    }
    // Inject every actual MMIO read/write, including a write that reached HW
    // before reporting failure; check power/reset and explicit dirty ownership.
    for(unsigned kind=0;kind<2;++kind)for(unsigned n=1;n<=(kind?totalWrites:totalReads);++n) {
        realAmp();playbackMode=true;o={};a={};
        PicassoTfaSession s={io,o,{},{},0,0,false,false,false,false};
        if(kind)failWrite=n;else failRead=n;
        int e=picasso_tfa_play_prepare(s,a);if(!e)e=picasso_tfa_play_enable(s,a);if(!e)e=picasso_tfa_play_status(s,a,true);
        int cleanup=picasso_tfa_play_finish(s,a);
        assert(e || cleanup || o.cleanupError);
        assert(a.stopped || cleanup || o.dirty || o.uncertain);
        if(a.stopped)assert(amp[0]==1 && o.restored);
    }
    puts("PASS 0c74 speaker profile, held S16, real clock/amp state, bounded power/reset and every MMIO failure");
}
int main() {
    digitalTests();amplifierTests();
    assert(picasso_tfa_base(0)==0x988000 && picasso_tfa_base(6)==0xf53a000);
    setup();PicassoTfa o={};picasso_tfa_run(io,o);
    assert(o.complete && o.identified && o.revision==0x1a74 && o.reads==11 && o.commands==22 && !o.error && !o.cleanupError);
    assert(!irqAsserted && !heldBus && !active && delays>=20000 && delays<21000);checkRestored(o);
    unsigned totalReads=reads,totalWrites=writes;
    for(unsigned i=0;i<9;++i)assert(o.values[i]==(0x1200|picasso_tfa_register(i)));
    unsigned before=writes;picasso_tfa_run(io,o);assert(writes==before);
    // No transfer on malformed engine, busy DMA/peer, wrong GPIO or missing XO.
    for(unsigned mode=0;mode<9;++mode) {
        setup();o={};
        if(mode==0)mem[0][0x68/4]=0x101;
        if(mode==1)mem[0][0x64/4]=1;
        if(mode==2)mem[0][0x40/4]=1;
        if(mode==3)mem[0][0xe18/4]=1;
        if(mode==4)mem[0][0x258/4]=1;
        if(mode==5)mem[4][0]=4;
        if(mode==6)for(unsigned i=0;i<8;++i)mem[1][(0x28c+4*i)/4]=0x601;
        if(mode==7)mem[0][0xe24/4]=0;
        if(mode==8)mem[0][0x804/4]=1;
        memcpy(original,mem,sizeof mem);picasso_tfa_run(io,o);assert(o.error && !o.complete && !commands);checkRestored(o);
    }
    setup();o={};mem[3][2]|=1U<<24;mem[1][0x26c/4]=0;memcpy(original,mem,sizeof mem);
    picasso_tfa_run(io,o);assert(o.complete);checkRestored(o);
    setup();o={};mem[1][0x284/4]=0;mem[1][0x274/4]=1;picasso_tfa_run(io,o);assert(o.complete);
    for(unsigned mode=0;mode<5;++mode) {
        setup();o={};injectCommand=2;
        if(mode==0)nack=1;else if(mode==1)arb=1;else if(mode==2)timeout=1;else if(mode==3)shortRead=1;else overflow=1;
        picasso_tfa_run(io,o);assert(o.error && !o.complete && !o.identified);checkRestored(o);
        assert(delays<=50000 && !heldBus && !active);
    }
    setup();o={};badId=1;picasso_tfa_run(io,o);assert(o.error==-19 && !o.identified && o.reads==2);checkRestored(o);
    setup();o={};unstable=1;picasso_tfa_run(io,o);assert(o.error==-19 && !o.identified);checkRestored(o);
    // Every I/O failure is explicit, including partial snapshots and cleanup.
    for(unsigned n=1;n<=totalReads;++n) {
        setup();o={};failRead=n;picasso_tfa_run(io,o);assert(o.error || o.cleanupError);
        assert(!o.complete || o.cleanupError);assert(o.restored || o.dirty || o.uncertain);
    }
    for(unsigned n=1;n<=totalWrites;++n) {
        setup();o={};failWrite=n;picasso_tfa_run(io,o);assert(o.error || o.cleanupError);
        assert(!o.complete || o.cleanupError);assert(o.restored || o.dirty || o.uncertain);
    }
    // A failed restore must keep the IRQ route disconnected and clock owned.
    setup();o={};mem[0][0xe1c/4]=15;failWrite=totalWrites-3;picasso_tfa_run(io,o);
    assert(o.cleanupError && !o.restored && (o.dirty&6)==6 && !irqAsserted && mem[0][0xe1c/4]==0);
    setup();o={};dropWrite=1;picasso_tfa_run(io,o);assert(o.error==-5 && !commands);
    setup();o={};injectCommand=2;timeout=abortFail=1;picasso_tfa_run(io,o);
    assert(o.error==-60 && o.cleanupError==-60 && o.uncertain && !o.restored && (mem[3][2]&(1U<<24)));
    setup();o={};injectCommand=2;timeout=stopFail=1;picasso_tfa_run(io,o);
    assert(o.error==-60 && o.cleanupError==-60 && o.uncertain && !o.restored);
    puts("PASS TFA9874 SE8 FIFO reads, repeated START, endian/count/ID checks, inherited votes, IRQ isolation, failures and bounded cleanup");
}
