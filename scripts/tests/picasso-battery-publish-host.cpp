// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include "PicassoBattery.h"

// The consumer models the stock iOS policy: physical input transitions reset
// the percentage settling interval, and an unplugged battery cannot rise after
// it settles. It deliberately does not use ExternalConnected or IsCharging as
// substitutes for AppleRawExternalConnected.
struct Consumer {
    int percent=0;bool raw=false;uint64_t settle=0;
    void update(const std::map<std::string,int64_t> &v){
        auto get=[&](const char *k){auto i=v.find(k);return i==v.end()?int64_t(0):i->second;};
        bool input=get("AppleRawExternalConnected");
        uint64_t now=uint64_t(get("PicassoLastSampleMS"));
        if(input!=raw)settle=now+60000;
        int maximum=int(get("MaxCapacity"));
        int next=maximum?int(std::lround(double(get("CurrentCapacity"))*100/maximum)):1;
        if(next<1)next=1;if(next>100)next=100;
        if(now>=settle && !raw && !input && next>percent && percent)next=percent;
        percent=next;raw=input;
    }
};
struct Publisher {
    std::map<std::string,int64_t> values;Consumer consumer;
    bool published=false;unsigned updates=0,registrations=0;
    void number(const char *k,uint64_t v){values[k]=int64_t(v);}
    void flag(const char *k,bool v){values[k]=v;}
    void text(const char *,const char *){}
    void erase(const char *k){values.erase(k);}
    void adapter(const picasso_battery::Snapshot &){}
    void updateStatus(){++updates;consumer.update(values);}
    void registerService(){assert(updates);++registrations;}
    void publish(const picasso_battery::Snapshot &);
    void stale();
    void setBatteryInstalled(bool v){flag("BatteryInstalled",v);}
    void setExternalConnected(bool v){flag("ExternalConnected",v);}
    void setExternalChargeCapable(bool v){flag("ExternalChargeCapable",v);}
    void setIsCharging(bool v){flag("IsCharging",v);}
    void setAtWarnLevel(bool v){flag("AtWarnLevel",v);}
    void setAtCriticalLevel(bool v){flag("AtCriticalLevel",v);}
    void setCurrentCapacity(unsigned v){number("CurrentCapacity",v);}
    void setMaxCapacity(unsigned v){number("MaxCapacity",v);}
    void setVoltage(unsigned v){number("Voltage",v);}
    void setAmperage(int v){number("Amperage",uint32_t(v));}
    void setTimeRemaining(int v){number("TimeRemaining",uint32_t(v));}
};
#include "publisher.inc"
int main(){
    Publisher p;picasso_battery::Snapshot s;
    s.present=true;s.percent=78;s.timeMs=100000;
    p.publish(s);assert(p.consumer.percent==78 && !p.consumer.raw);
    s.external=true;s.capable=true;s.charging=true;s.netCharging=true;s.timeMs+=5000;
    p.publish(s);assert(p.values.at("IsCharging"));
    // Stay connected well beyond powerd's 60-second settling interval.
    s.timeMs+=65000;s.percent=79;p.publish(s);assert(p.consumer.percent==79);
    s.timeMs+=60000;s.percent=80;p.publish(s);assert(p.consumer.percent==80);
    s.external=false;s.capable=false;s.charging=false;s.netCharging=false;s.timeMs+=5000;
    p.publish(s);assert(p.consumer.percent==80 && !p.consumer.raw);
    s.timeMs+=65000;s.percent=79;p.publish(s);assert(p.consumer.percent==79);
    s.timeMs+=5000;s.percent=80;p.publish(s);assert(p.consumer.percent==79);
    // Physical input is true even if charging is paused or the pack is full.
    s.external=true;s.charging=false;s.capable=false;s.timeMs+=5000;p.publish(s);
    assert(p.consumer.raw && !p.values.at("IsCharging"));
    s.timeMs+=65000;s.percent=81;p.publish(s);assert(p.consumer.percent==81);
    s.full=true;s.timeMs+=65000;s.percent=100;p.publish(s);assert(p.consumer.percent==100);
    assert(p.registrations==1 && p.updates==10);
    p.stale();assert(!p.values.count("AppleRawExternalConnected"));
    assert(!p.values.count("ExternalConnected") && !p.values.at("IsCharging"));
    s.external=false;s.full=false;s.percent=98;s.timeMs+=5000;p.publish(s);
    assert(p.consumer.percent==98 && !p.consumer.raw && p.registrations==1);
    std::puts("PASS charge 78 -> 79 -> 80, unplug/discharge, rebound guard, paused/full input, stale recovery");
}
