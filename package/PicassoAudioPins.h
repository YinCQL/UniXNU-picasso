// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_PINS_H
#define PICASSO_AUDIO_PINS_H
#include <stdint.h>
// Picasso lito-pinctrl.dtsi / pinctrl-lito.c: WEST, mux 1, 8mA,
// no pull, SCK/WS/SD1 output-high and SD0 input. IRQ registers are untouched.
struct PicassoAudioPins {
    void *context;
    int (*read)(void *,unsigned,unsigned,uint32_t *);
    int (*write)(void *,unsigned,unsigned,uint32_t);
    uint32_t saved[4][2], observed[4], dirty, events;
    bool attempted,active,restored;
    int error,restoreError;
};
inline unsigned picasso_audio_gpio(unsigned i) { const unsigned p[4]={70,71,72,74};return p[i]; }
inline uint64_t picasso_audio_pin_pa(unsigned i) { return 0x0f500000ULL+picasso_audio_gpio(i)*0x1000ULL; }
inline uint32_t picasso_audio_pin_ctl(uint32_t old,unsigned i) {
    return (old&~0x3ffU)|4U|(3U<<6)|(i==2?0:1U<<9)|((old&(1U<<11))?1U<<12:0);
}
inline int picasso_audio_pin_update(PicassoAudioPins &p,unsigned i,unsigned reg,uint32_t mask,uint32_t value) {
    uint32_t old=0,got=0;int e=p.read(p.context,i,reg,&old);if(e)return e;
    e=p.write(p.context,i,reg,(old&~mask)|(value&mask));if(e)return e;
    e=p.read(p.context,i,reg,&got);if(e)return e;
    return (got&mask)==(value&mask)?0:-5;
}
inline void picasso_audio_pins_restore(PicassoAudioPins &p) {
    if(!p.dirty)return;
    for(unsigned n=4;n>0;--n) {
        unsigned i=n-1;if(!(p.dirty&(1U<<i)))continue;
        // First remove output enable, restore the saved latch, then restore
        // owned control bits. Read-only input bits are never compared/written.
        int e=picasso_audio_pin_update(p,i,0,1U<<9,0);
        if(!e)e=picasso_audio_pin_update(p,i,4,2,p.saved[i][1]);
        if(!e)e=picasso_audio_pin_update(p,i,0,0x13ff,p.saved[i][0]);
        if(e) { if(!p.restoreError)p.restoreError=e; }
        else p.dirty&=~(1U<<i);
    }
    p.active=false;p.restored=!p.dirty;++p.events;
}
inline int picasso_audio_pins_activate(PicassoAudioPins &p) {
    if(p.attempted)return p.active?0:(p.error?p.error:-16);
    p.attempted=true;++p.events;
    if(!p.read || !p.write)return p.error=-6;
    // Snapshot all pins before any mutation; reject a different peripheral.
    for(unsigned i=0;i<4;++i) {
        int e=p.read(p.context,i,0,&p.saved[i][0]);
        if(!e)e=p.read(p.context,i,4,&p.saved[i][1]);
        if(e)return p.error=e;
        p.observed[i]=p.saved[i][0];
        if(((p.saved[i][0]>>2)&15)>1)return p.error=-16;
    }
    for(unsigned i=0;i<4;++i) {
        p.dirty|=1U<<i; // A failed publication can still have reached hardware.
        uint32_t ctl=picasso_audio_pin_ctl(p.saved[i][0],i);
        int e=picasso_audio_pin_update(p,i,0,0x103c,ctl);
        if(!e && i!=2)e=picasso_audio_pin_update(p,i,4,2,2);
        if(!e)e=picasso_audio_pin_update(p,i,0,0x13ff,ctl);
        if(!e)e=p.read(p.context,i,0,&p.observed[i]);
        if(e) { p.error=e;picasso_audio_pins_restore(p);return e; }
    }
    p.active=true;++p.events;return 0;
}
#endif
