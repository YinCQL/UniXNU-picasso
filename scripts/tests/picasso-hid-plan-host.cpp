// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "PicassoHidPlan.h"

int main() {
    using Plan = PicassoHidPlan;
    unsigned cases = 0;
    Plan p;
    assert(p.poll(999999, 0, true, true) == Plan::Idle); ++cases;
    p.arm(1000, 0);
    assert(p.poll(999999, 0, true, true) == Plan::Idle); ++cases;
    p = {}; p.arm(1000, 99); assert(p.limit == 8); ++cases;
    assert(p.poll(125999, 0, true, true) == Plan::Idle); ++cases;
    assert(p.poll(126000, 0, true, false) == Plan::Idle); ++cases;
    unsigned reports = 0;
    uint64_t now = 126000;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        for (unsigned frame = 0; frame < 25; ++frame) {
            assert(p.poll(now, reports, true, true) == Plan::Contact);
            assert(p.y() == 20100 - 18900 * (int)frame / 24);
            ++reports;
            assert(p.poll(now + 1, reports, true, true) == Plan::Idle);
            now += 16;
        }
        assert(p.poll(now, reports, true, true) == Plan::Release);
        assert(p.poll(now + 1, ++reports, true, true) == Plan::Idle);
        assert(!p.active && p.sent == attempt + 1);
        now += 60001;
        assert(p.poll(now - 1, reports, true, true) == Plan::Idle);
    }
    assert(p.poll(now, reports, true, true) == Plan::Idle); ++cases;
    for (unsigned failure = 0; failure < 3; ++failure) {
        p = {}; p.arm(0, 2);
        assert(p.poll(125000, 0, true, true) == Plan::Contact);
        assert(p.poll(125100, 0, true, true) == Plan::Idle);
        auto action = failure == 0 ? p.poll(125200, 0, true, true) :
            failure == 1 ? p.poll(125101, 1, false, true) : p.poll(125101, 0, true, false);
        assert(action == Plan::Release);  // Timeout, failed completion, device lost.
        assert(p.poll(125401, 0, false, false) == Plan::Idle);
        assert(!p.active && p.sent == 1);
        assert(p.poll(185400, 0, true, true) == Plan::Idle);
        assert(p.poll(185401, 0, true, true) == Plan::Contact);
        ++cases;
    }
    p = {}; p.arm(0, 1);
    reports = UINT32_MAX;
    assert(p.poll(125000, reports, true, true) == Plan::Contact);
    assert(p.expected == 0);
    assert(p.poll(125016, 0, true, true) == Plan::Contact); ++cases;
    p = {}; p.arm(0, 2);
    assert(p.poll(999999, 0, true, true) == Plan::Contact);
    assert(p.poll(999999, 1, true, true) == Plan::Idle); // No catch-up burst.
    assert(p.poll(1000015, 1, true, true) == Plan::Contact); ++cases;
    p={};p.arm(1000,1,8000);
    assert(p.poll(8999,0,true,true)==Plan::Idle);
    assert(p.poll(9000,0,true,true)==Plan::Contact);++cases;
    p={};p.arm(1000,1,0);
    assert(p.poll(1000,0,true,true)==Plan::Contact);++cases;
    p={};p.arm(0,8,0);p.stop();
    assert(p.poll(999999,0,true,true)==Plan::Idle && p.sent==0);++cases;
    p={};p.arm(0,8,0);assert(p.poll(0,0,true,true)==Plan::Contact);p.stop();
    assert(p.poll(1,0,true,true)==Plan::Idle);
    assert(p.poll(2,1,true,true)==Plan::Release);
    assert(p.poll(3,2,true,true)==Plan::Idle && !p.active);
    assert(p.poll(999999,2,true,true)==Plan::Idle && p.sent==1);++cases;
    p={};p.arm(0,8,0);assert(p.poll(0,0,true,true)==Plan::Contact);p.stop();
    assert(p.poll(200,0,true,true)==Plan::Release);
    assert(p.poll(400,0,false,false)==Plan::Idle && !p.active);++cases;
    printf("PASS %u HID pacing, completion, timeout, release and attempt-budget cases\n", cases);
}
