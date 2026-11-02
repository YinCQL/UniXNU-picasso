// Actual production decoder and sampling policy; the SPI reads are a model.
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern "C" {
#include "NvtProtocol.h"
}
#include "NvtTouchCore.h"
#include "TouchBoard.h"
static uint8_t samples[2][NVT_REPORT_LEN];
static unsigned reads;
static int failRead;
extern "C" int nvt_read_report(nvt_chip *, uint8_t *out) {
    ++reads;
    if (failRead == static_cast<int>(reads)) return NVT_ERR_BUS;
    assert(reads <= 2);
    memcpy(out, samples[reads - 1], NVT_REPORT_LEN);
    return 0;
}
#include "report-bodies.h"
static void point(uint8_t *raw, unsigned slot, unsigned id, unsigned x, unsigned y) {
    unsigned at = slot * 6;
    raw[at] = static_cast<uint8_t>((id << 3) | 2);
    raw[at + 1] = static_cast<uint8_t>(x >> 4);
    raw[at + 2] = static_cast<uint8_t>(y >> 4);
    raw[at + 3] = static_cast<uint8_t>((x & 15) << 4 | (y & 15));
}
static void reset() { memset(samples, 0, sizeof(samples)); reads = 0; failRead = 0; }
int main() {
    nvt_contact last[NVT_REPORT_POINTS] = {}, out[NVT_REPORT_POINTS] = {};
    int n = 0;
    // build117's first logged positions: raw 40,2124 -> 42,2129.
    // A moving press is still the same finger and must publish the latest point.
    reset(); point(samples[0], 0, 1, 40, 2124); point(samples[1], 0, 1, 42, 2129);
    assert(sampleContacts(nullptr, last, 0, out, n) == 1 && n == 1);
    assert(out[0].x == 420 && out[0].y == 21290 && out[0].tracking_id == 1 && reads == 2);
    memcpy(last, out, sizeof(last));
    // Continuously moving finger, including a larger valid movement.
    reset(); point(samples[0], 0, 1, 46, 2135); point(samples[1], 0, 1, 49, 2140);
    assert(sampleContacts(nullptr, last, 1, out, n) == 1 && out[0].x == 490 && out[0].y == 21400);
    memcpy(last, out, sizeof(last));
    reset(); point(samples[0], 0, 1, 49, 2140);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0 && reads == 1); // stationary: no extra SPI read
    // Unconfirmed press/lift or changed identifier must not erase the last contact.
    reset(); point(samples[1], 0, 1, 49, 2140);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0);
    reset(); point(samples[0], 0, 1, 50, 2140); point(samples[1], 0, 2, 50, 2140);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0);
    reset(); assert(sampleContacts(nullptr, last, 1, out, n) == 1 && n == 0); // confirmed lift
    // Reordered records with the same fingers are valid; preserve the newer identifiers.
    reset(); point(samples[0], 0, 1, 100, 200); point(samples[0], 1, 2, 300, 400);
    point(samples[1], 0, 2, 301, 402); point(samples[1], 1, 1, 101, 202);
    assert(sampleContacts(nullptr, last, 0, out, n) == 1 && n == 2 && out[0].tracking_id == 2);
    // Malformed active coordinates and duplicate IDs are rejected, not reported as a lift.
    reset(); point(samples[0], 0, 1, 1081, 200);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0);
    reset(); point(samples[0], 0, 1, 100, 2401);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0);
    reset(); point(samples[0], 0, 1, 100, 200); point(samples[0], 1, 1, 300, 400);
    assert(sampleContacts(nullptr, last, 1, out, n) == 0);
    // Exact panel edges retain the existing clamped HID mapping.
    reset(); point(samples[0], 9, 10, 1080, 2400); memcpy(samples[1], samples[0], NVT_REPORT_LEN);
    assert(sampleContacts(nullptr, last, 0, out, n) == 1 && n == 1 && out[0].tracking_id == 10);
    tb_hid_point mapped = tb_hid_from_panel(out[0].x, out[0].y);
    assert(mapped.x < TB_HID_X_MAX && mapped.y < TB_HID_Y_MAX);
    reset(); failRead = 1; assert(sampleContacts(nullptr, last, 0, out, n) == NVT_ERR_BUS);
    reset(); point(samples[0], 0, 1, 40, 2124); failRead = 2;
    assert(sampleContacts(nullptr, last, 0, out, n) == NVT_ERR_BUS);
    puts("PASS: moving press/move, latest sample, stationary read suppression, confirmed lift, changing membership, malformed records, edges and bus failures");
}
