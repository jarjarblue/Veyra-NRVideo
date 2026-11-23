// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/engine/CapturePrediction.h"
#include <iostream>
#include <limits>
using namespace veyra::engine;
namespace { unsigned checks=0, failures=0;
void check(bool v,const char* label) { ++checks; if(!v){++failures;std::cerr<<"FAIL "<<label<<'\n';} }
CapturePredictionClock::Source frame(uint64_t id) {
    return {1,1,id,int64_t(id-1)*333333,100000000+int64_t(id-1)*333333};
}
}
int main() {
    CapturePredictionClock clock; CapturePredictionSettings s;
    auto a=frame(1), b=frame(2), c=frame(3);
    check(!clock.decide(s,a.arrival100ns).active(),"off by default");
    s.enabled=true;
    check(!clock.decide(s,a.arrival100ns).active(),"no source");
    clock.accept(a); check(!clock.decide(s,a.arrival100ns).active(),"first input is exact source");
    clock.accept(b);
    auto d=clock.decide(s,b.arrival100ns);
    check(d.active()&&!d.acceleration,"two inputs allow velocity only");
    check(std::abs(d.horizonMs-8)<1e-6,"explicit compensation");
    clock.accept(c); d=clock.decide(s,c.arrival100ns);
    check(d.active()&&d.acceleration,"three inputs allow bounded acceleration");
    check(std::abs(d.intervalRatio-1)<1e-6,"interval ratio");
    d=clock.decide(s,c.arrival100ns+83333);
    check(d.active()&&std::abs(d.horizonMs-16.3333)<.0001,"late refresh adds only observed local age");
    d=clock.decide(s,c.arrival100ns+250000);
    check(d.active()&&d.horizonMs<=25,"bounded total horizon");
    check(clock.decide(s,c.arrival100ns+333333).reason==PredictionReason::Stale,"no predictions across missing input deadline");
    check(clock.decide(s,c.arrival100ns-1).reason==PredictionReason::InvalidClock,"future arrival rejected");
    auto duplicate=c; duplicate.arrival100ns+=500000;
    check(clock.accept(duplicate)==PredictionReason::Duplicate,"duplicate identity rejected");
    check(clock.decide(s,duplicate.arrival100ns).reason==PredictionReason::Stale,"duplicate cannot refresh stale source");
    for(unsigned event=0;event<7;++event) {
        clock.clear(); clock.accept(a); clock.accept(b); auto next=c;
        if(event==0)next.epoch++;
        if(event==1)next.revision++;
        if(event==2)next.reset=true;
        if(event==3)next.id+=3;
        if(event==4)next.pts100ns+=2000000;
        if(event==5)next.pts100ns=b.pts100ns;
        if(event==6)next.arrival100ns=b.arrival100ns;
        clock.accept(next);check(!clock.decide(s,next.arrival100ns).active(),"history invalidated on lifecycle discontinuity");
    }
    clock.clear();clock.accept(a);auto file=b;file.liveCapture=false;clock.accept(file);
    check(clock.decide(s,b.arrival100ns).reason==PredictionReason::UnsupportedSource,"files/export excluded");
    clock.clear();clock.accept(a);auto owned=b;owned.providerOwned=true;clock.accept(owned);
    check(clock.decide(s,b.arrival100ns).reason==PredictionReason::ProviderOwned,"XeSS-owned presentation excluded");
    for(double value:{-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        clock.clear();clock.accept(a);clock.accept(b);auto invalid=s;invalid.compensationMs=value;
        check(!clock.decide(invalid,b.arrival100ns).active(),"invalid compensation rejected");
    }
    clock.clear();clock.accept(a);clock.accept(b);clock.gpuCost(3);
    check(clock.decide(s,b.arrival100ns).reason==PredictionReason::Budget,"over-budget GPU result bypasses immediately");
    for(unsigned id=3;id<10;++id){auto next=frame(id);clock.accept(next);check(!clock.decide(s,next.arrival100ns).active(),"cooldown remains bounded");}
    auto ten=frame(10);clock.accept(ten);check(clock.decide(s,ten.arrival100ns).active(),"budget probe retries after cooldown");
    clock.gpuCost(.3);check(clock.decide(s,ten.arrival100ns).active(),"within budget resumes");
    clock.gpuCost(std::numeric_limits<double>::infinity());check(clock.decide(s,ten.arrival100ns).active(),"invalid GPU samples ignored");
    clock.clear();clock.accept(a);clock.accept(b);
    s.compensationMs=1000;s.maxHorizonMs=1000;s.maxIntervalFraction=100;
    check(clock.decide(s,b.arrival100ns).horizonMs<=33.3333,"untrusted settings cannot extrapolate beyond one interval");
    // A 30 -> 120 refresh walk never waits for a not-yet-arrived frame.
    s={};s.enabled=true;clock.clear();unsigned forecasts=0;
    for(unsigned source=1;source<=300;++source) {
        auto f=frame(source);clock.accept(f);
        for(unsigned refresh=0;refresh<4;++refresh) {
            auto prediction=clock.decide(s,f.arrival100ns+int64_t(refresh)*83333);
            if(source>1){check(prediction.active(),"each fresh refresh has a bounded decision");++forecasts;}
            check(prediction.horizonMs<=25,"refresh horizon cap");
        }
    }
    check(forecasts==1196,"latest-input-only refresh count");
    std::cout<<"Capture prediction clock: "<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
