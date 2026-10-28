// SPDX-License-Identifier: BSD-2-Clause-Patent
// The production helper compiles without any clock, delay or scheduler API.
#include "../PollPause.h"
#include <cassert>
#include <cstdio>

int main()
{
  unsigned total=0;
  for (unsigned run=0; run<8; ++run) {
    unsigned hints=0;
    ufsPollPause([&] {
      // An accidentally unbounded loop fails on its first excess iteration.
      assert(++hints<=64);
      ++total;
    });
    assert(hints==64);
  }
  assert(total==512);
  puts("PASS: production pause returns after 64 hints without clock or legacy wait dependencies; no claim of transfer timeout with a frozen clock");
}
