// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Only the measured initial iOS16 logo on this GOP mode may arm the hold.
// A different surface or a deadline ends it permanently. Blank swaps may
// retire while the existing logo remains in scanout; no extra image is kept.
struct NativeLegacyBootLogo {
    bool enabled = false, observed = false, holding = false, ended = false;
    uint64_t deadline = 0;

    bool needsScan(uint64_t now) {
        if (holding && now >= deadline) { holding = false; ended = true; }
        return enabled && !ended;
    }
    void observe(uint64_t now, uint32_t width, uint32_t height, const uint64_t scan[5]) {
        if (!needsScan(now)) return;
        // The boot logo fades slightly before the first UI frame (29441 in build115).
        bool countMatches = observed ? scan[0] >= 29201 && scan[0] <= 29713 : scan[0] == 29457;
        bool logo = width == 1080 && height == 2400 && countMatches &&
            scan[1] >= width / 4 && scan[2] >= height / 4 &&
            scan[3] < width * 3 / 4 && scan[4] < height * 3 / 4;
        if (!observed) {
            observed = true;
            if (logo) { holding = true; deadline = now + 300000; }
            else ended = true;
        } else if (!logo) { holding = false; ended = true; }
    }
    bool holdBlank(uint64_t now) {
        needsScan(now);
        return enabled && holding && !ended;
    }
};
