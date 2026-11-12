// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM8150 primary PON: disable PS_HOLD reset, wait 10 sleep-clock cycles
// with tolerance, select shutdown, re-enable and verify before dropping hold.
#pragma once
#include <stdint.h>
namespace picasso_power {
enum Stage : unsigned { prepare=0, probe=1, disable=2, select=3, enable=4, verify=5,
                        drain=6, arbiter=7, deassert=8, returned=9 };
struct Result { int error=0; unsigned stage=probe; uint8_t control=0, enabled=0; };
template<class Io> Result shutdown(Io &io) {
    Result r;
    uint8_t subtype=0;
    if ((r.error=io.read(5,subtype))) return r;
    if (subtype!=4) {r.error=-20;return r;}
    if ((r.error=io.read(0x5a,r.control)) || (r.error=io.read(0x5b,r.enabled))) return r;
    r.stage=prepare;
    if ((r.error=io.prepare())) return r;
    r.stage=disable;
    if ((r.error=io.write(0x5b,uint8_t(r.enabled&0x7f)))) return r;
    io.delay(500);
    r.stage=select;
    if ((r.error=io.write(0x5a,uint8_t((r.control&0xf0)|4)))) return r;
    r.stage=enable;
    if ((r.error=io.write(0x5b,uint8_t(r.enabled|0x80)))) return r;
    r.stage=verify;
    if ((r.error=io.read(0x5a,r.control)) || (r.error=io.read(0x5b,r.enabled))) return r;
    if ((r.control&15)!=4 || !(r.enabled&0x80)) {r.error=-21;return r;}
    r.stage=drain;
    if ((r.error=io.quiesce())) return r;
    r.stage=arbiter;
    if ((r.error=io.halt_arbiter())) return r;
    r.stage=deassert;
    io.drop_hold();
    // The platform waits for physical removal of power. A returned write alone
    // is never reported as a successful shutdown.
    io.delay(1000000);
    r.stage=returned;r.error=-22;
    return r;
}
}
