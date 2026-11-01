// SPDX-License-Identifier: Apache-2.0
// Finger-touch processing for the NT36672C 18x36 THP matrix: baseline, delta, peaks, centroid and slot tracking.
// Adapted from ShengNovatekTouch's NT36532E core (Apache-2.0; see NOTICE), with the geometry, axis assignment and
// thresholds parameterised for the picasso panel. Plain C, no allocation.
//
// Axis assignment: the 18 X channels run across the 1080-pixel short axis, the 36 Y channels along the 2400-pixel
// long axis. Core coordinates are 0.1 panel pixels: x in 0..10799 (short), y in 0..23999 (long). TouchBoard.h maps
// them to the HID raw convention. Thresholds that were tuned for the NT36532E matrix are marked TODO in
// NvtTouchCore.c and need a hardware pass.
#ifndef NVT_TOUCH_CORE_H
#define NVT_TOUCH_CORE_H
#include <stdint.h>
#include <stddef.h>

#define NVT_ROWS 36                      // IC_Y_CFG_SIZE, long axis (picasso.dts:24504)
#define NVT_COLUMNS 18                   // IC_X_CFG_SIZE, short axis (picasso.dts:24503)
#define NVT_NODES (NVT_ROWS * NVT_COLUMNS)
#define NVT_SUPER_RESOLUTION 10
// Steps below 4 * NVT_FUZZ count as the positional noise of a resting or slow finger (jitter_*), in coordinate
// units (0.1 panel px). 60 was the NT36532E value; the picasso node spacing is 60 x 66.7 px, so 6 px is a smaller
// fraction of a node. TODO: tune on hardware.
#define NVT_FUZZ 60
// Reported positions are smoothed with an adaptive low-pass filter: cutoff NVT_MIN_CUTOFF_MHZ for a resting finger,
// rising by NVT_CUTOFF_PER_SPEED mHz per unit/s of smoothed speed. NVT_FRAME_HZ is the NT36532E rate; the
// NT36672C reporting rate must be measured. TODO.
#define NVT_FRAME_HZ 120
#define NVT_MIN_CUTOFF_MHZ 1000
#define NVT_CUTOFF_PER_SPEED 3   // mHz per (unit/s), halved: 1.5
#define NVT_SPEED_CUTOFF_MHZ 1000
// The centroid weighs a node by how far it is above this share of its blob's peak (percent).
#define NVT_CENTROID_FLOOR 30
// Maxima this many nodes apart or closer (rows and columns) are one finger.
#define NVT_MERGE_SPAN 2
// Frames a tracked finger may go unseen before it counts as lifted. At 120 Hz each is 8.3 ms.
#define NVT_HOLD_FRAMES 3
#define NVT_SLOTS 12
#define NVT_STARTUP_FRAMES 72
// Output axes before the HID mapping: x along the short (1080 px) axis, y along the long (2400 px) axis.
#define NVT_SHORT_AXIS_MAX 10800
#define NVT_LONG_AXIS_MAX 24000

// Thresholds from the NT36532E reference. TODO: tune against NT36672C raw frames; the panel and AFE differ.
#define NVT_NOISE_THRESHOLD 100
#define NVT_PEAK_THRESHOLD 220
#define NVT_DOWN_THRESHOLD 300
#define NVT_UP_THRESHOLD 220
#define NVT_COORDINATE_REDUCE 25
// Palm rejection is a node-count rule. 60 was the NT36532E value on 2400 nodes; 48 on 648 nodes is about the same
// fraction and clears a normal finger blob. TODO: tune on hardware.
#define NVT_PALM_MIN_NODES 48
// The reference follows slow baseline drift: in a frame without a node at the peak threshold every node moves
// 1/NVT_REFERENCE_RATE of the way to its value.
#define NVT_REFERENCE_RATE 32

struct nvt_contact {
    int slot, tracking_id;
    int x, y;        // x along the short axis, y along the long axis, 0 .. max-1, 0.1 panel px
    int peak, area, weight;
};

struct nvt_slot {
    int active, tracking_id, age, x, y;
    int rx, ry;      // the position last reported, after the filter (x, y stay raw: tracking uses them)
    long fx, fy;     // the filtered position, in 1/16 coordinate units
    long vx, vy;     // the smoothed speed, coordinate units per second
    int dx, dy;      // the last raw step per frame: tracking looks for the blob where the finger is heading
    int missed;      // consecutive frames without a matching blob (held while <= NVT_HOLD_FRAMES)
    int miss_reason; // why the first of those frames had no match (NVT_MISS_*)
};

// Why a tracked finger found no blob: none at all, the nearest beyond the tracking radius, below the up threshold,
// palm-sized, or already taken by another finger.
enum {NVT_MISS_NONE, NVT_MISS_FAR, NVT_MISS_WEAK, NVT_MISS_PALM, NVT_MISS_TAKEN, NVT_MISS_KINDS};

struct nvt_touch_core {
    int16_t reference[NVT_NODES];
    int have_reference;
    unsigned startup_count;
    int16_t startup[NVT_STARTUP_FRAMES][NVT_NODES];   // collected until the median reference is formed
    // Raw frame-to-frame movement of tracked contacts that moved less than 4 * NVT_FUZZ, in coordinate units.
    unsigned long jitter_frames, jitter_sum;
    int jitter_max;
    unsigned long smoothed_sum;
    int smoothed_max;
    // Maxima folded into a stronger one within NVT_MERGE_SPAN nodes (one finger seen as two peaks).
    unsigned long merges;
    // Most maxima in one frame, frames with more maxima than the table holds, frames whose quiet nodes moved the
    // reference.
    unsigned long peaks_max, peak_overflows, reference_frames;
    // Fingers that went unseen and came back within NVT_HOLD_FRAMES, fingers lifted per miss reason, frames by type.
    unsigned long holds_bridged, ends[NVT_MISS_KINDS], frames_by_type[2];
    struct nvt_slot slots[NVT_SLOTS];
    // Scratch for nvt_touch_process. Kept in the core because kernel thread stacks are 16 KiB; the struct itself is
    // allocated by the probe.
    int delta[NVT_NODES];
    int label[NVT_NODES];
};

void nvt_touch_reset(struct nvt_touch_core *core);
// Feed one touch matrix (row-major, 36x18 int16). Returns the number of visible contacts written to out[0..max)
// (-1 while the startup baseline is still being collected). frame_type 2 = baseline frame.
int nvt_touch_process(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                      struct nvt_contact *out, int max);
// Edge compensation applied to a raw centroid coordinate.
int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias);
// Row/column centre tables in panel pixels (before super resolution).
int nvt_touch_row_center(int row);
int nvt_touch_column_center(int column);
#endif
