// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../TransferPoll.h"

struct Sample { uint32_t db, ocs, is; };
struct FakePort {
  std::vector<Sample> samples;
  unsigned index = 0, reads = 0, polls = 0, pauses = 0;
  bool changeOnExpiry = false;
  Sample atExpiry = {};
  void beginPoll(uint32_t poll) { assert(poll == ++polls); }
  uint32_t doorbell() { return samples[index].db; }
  uint32_t interrupt() { return samples[index].is; }
  uint32_t completedOcs() {
    assert(!(samples[index].db & 1)); // Never inspect a descriptor still owned by HC.
    ++reads;
    return samples[index].ocs;
  }
  bool expired() {
    bool end = index + 1 == samples.size();
    if (end && changeOnExpiry) samples[index] = atExpiry;
    return end;
  }
  void delay() { assert(index + 1 < samples.size()); ++index; ++pauses; }
};

int main()
{
  // Exact IMG_2971 observation must not be treated as a completed command.
  FakePort noAcceptance{{{0, 15, 0}, {0, 15, 0}, {0, 15, 0}}};
  UfsTransferPoll a;
  assert(pollUfsTransfer(noAcceptance, a) == UfsTransferEnd::Timeout);
  assert(a.polls == 4 && a.deadlineSample && a.ocs == 15 && a.doorbellSeen == 0);
  assert(noAcceptance.pauses == 2);

  FakePort delayed{{{0, 15, 0}, {1, 15, 0}, {0, 15, 0}, {0, 0, 1}}};
  UfsTransferPoll b;
  assert(pollUfsTransfer(delayed, b) == UfsTransferEnd::Completed);
  assert(b.polls == 4 && b.ocs == 0 && b.doorbellSeen == 1 && delayed.reads == 3);

  FakePort immediate{{{0, 0, 0}}};
  UfsTransferPoll c;
  assert(pollUfsTransfer(immediate, c) == UfsTransferEnd::Completed);
  assert(c.doorbellSeen == 0); // Seeing DB=1 must not be mandatory.

  FakePort failedCommand{{{1, 15, 0}, {0, 4, 1}}};
  UfsTransferPoll d;
  assert(pollUfsTransfer(failedCommand, d) == UfsTransferEnd::Completed && d.ocs == 4);

  FakePort stuck{{{1, 15, 0}, {1, 15, 0}}};
  UfsTransferPoll e;
  assert(pollUfsTransfer(stuck, e) == UfsTransferEnd::Timeout && stuck.reads == 0);
  assert(e.polls == 3 && stuck.pauses == 1 && e.doorbellLast == 1);

  for (Sample last: {Sample{0, 0, 1}, Sample{0, 0, kUfsFatalStatus}, Sample{0, 15, 0}, Sample{1, 15, 0}}) {
    FakePort changed{{{1, 15, 0}}};
    changed.changeOnExpiry = true; changed.atExpiry = last;
    UfsTransferPoll final;
    UfsTransferEnd end = pollUfsTransfer(changed, final);
    assert(end == (last.is & kUfsFatalStatus ? UfsTransferEnd::Fatal :
                   last.ocs == 0 ? UfsTransferEnd::Completed : UfsTransferEnd::Timeout));
    assert(final.polls == 2 && final.deadlineSample && changed.pauses == 0);
    assert(final.doorbellLast == last.db && final.interruptLast == last.is);
    if (last.db || (last.is & kUfsFatalStatus)) assert(changed.reads == 0);
  }

  for (unsigned bit = 0; bit < 32; ++bit) {
    uint32_t mask = uint32_t(1) << bit;
    if (!(mask & kUfsFatalStatus)) continue;
    FakePort fatal{{{0, 0, mask}}};
    UfsTransferPoll f;
    assert(pollUfsTransfer(fatal, f) == UfsTransferEnd::Fatal);
    assert(fatal.reads == 0 && f.interruptSeen == mask); // Fatal wins over OCS=0.
  }
  puts("PASS: unaccepted, delayed, immediate, failed, stuck and fatal UFS completions");
}
