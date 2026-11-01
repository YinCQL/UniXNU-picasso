// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded gestures: one report per callback, with a release on timeout/error.
#pragma once
#include <stdint.h>

struct PicassoHidPlan {
    enum Action { Idle, Contact, Release };
    uint64_t due = 0, deadline = 0;
    unsigned limit = 0, sent = 0, frame = 0, expected = 0;
    bool active = false, waiting = false, releasing = false;

    void arm(uint64_t now, unsigned count, uint32_t delayMs = 125000) {
        limit = count > 8 ? 8 : count;
        due = now + delayMs;
    }
    Action poll(uint64_t now, unsigned reports, bool ok, bool ready) {
        if (waiting) {
            if (reports == expected && ok) {
                waiting = false;
                if (releasing) { active = false; releasing = false; due = now + 60000; return Idle; }
            } else if (reports == expected || now >= deadline || !ready) {
                if (releasing) { waiting = false; active = false; releasing = false; due = now + 60000; return Idle; }
                releasing = true; expected = reports + 1; deadline = now + 200;
                return Release;
            } else return Idle;
        }
        if (!active) {
            if (!ready || !limit || sent >= limit || now < due) return Idle;
            active = true; frame = 0; ++sent;
        }
        if (now < due) return Idle;
        waiting = true; expected = reports + 1; deadline = now + 200;
        due = now + 16;
        if (frame == 25) { releasing = true; return Release; }
        ++frame;
        return Contact;
    }
    int y() const { return 20100 - (18900 * (int)(frame - 1)) / 24; }
};
