// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../package/PicassoMicHardware.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace {
using Key = std::pair<unsigned, unsigned>;
struct Fake {
    std::map<Key, uint32_t> reg;
    std::map<unsigned, uint32_t> slave;
    std::vector<Key> writes;
    uint64_t waited = 0;
    uint32_t response = 0;
    unsigned reads = 0, commands = 0, broadcasts = 0, releases = 0;
    bool available = false, full = false, silent = false, wrongId = false;
    bool noBankAck = false, rejectRestore = false;
    int railError = 0, railCheckError = 0;
    unsigned ignoredSlave = 0;

    Fake() {
        reg[{PicassoMicSwr, 0}] = 0x01050001;
        reg[{PicassoMicSwr, 0x100}] = (8U << 10) | (8U << 15);
        reg[{PicassoMicSwr, 0x1090}] = 1U << 2;
        reg[{PicassoMicSwr, 0x538}] = 0x01170223;
        reg[{PicassoMicSwr, 0x53c}] = 0x000d;
        reg[{PicassoMicTx, 0x404}] = 0xa5000000;
        reg[{PicassoMicLpi, 0}] = 0x80000000;
        slave[0x3401] = 0x38; slave[0x3402] = 0x93;
        slave[0x34ce] = 5;
    }
    static int read(void *ctx, unsigned region, uint32_t at, uint32_t *out) {
        auto &f = *static_cast<Fake *>(ctx); ++f.reads;
        if (region == PicassoMicSwr && at == 0x30c) {
            *out = (f.full ? 8U << 8 : 0) | (f.available ? 1U << 16 : 0); return 0;
        }
        if (region == PicassoMicSwr && at == 0x318) {
            assert(f.available); *out = f.response; f.available = false; return 0;
        }
        *out = f.reg[{region, at}]; return 0;
    }
    static int write(void *ctx, unsigned region, uint32_t at, uint32_t value) {
        auto &f = *static_cast<Fake *>(ctx); f.writes.push_back({region, at});
        if (region == PicassoMicSwr && at == 0x1044) {
            assert(value == 2); // CLK_START self-clears; it is never restored.
            f.reg[{region, at}] = 0; return 0;
        }
        if (region == PicassoMicSwr && at == 0x208) {
            f.reg[{region, 0x200}] &= ~value; return 0;
        }
        if (region == PicassoMicSwr && (at == 0x300 || at == 0x304)) {
            unsigned id = (value >> 16) & 15, dev = (value >> 20) & 15;
            unsigned address = value & 0xffff, byte = value >> 24;
            if (id == 15) {
                assert(at == 0x300 && dev == 15);
                assert(address == 0x60 || address == 0x70);
                ++f.broadcasts;
                if (!f.noBankAck) {
                    f.reg[{region, 0x104c}] = (address - 0x60) / 0x10;
                    f.reg[{region, 0x200}] |= 0x400;
                }
                return 0;
            }
            assert(dev == 1); ++f.commands;
            if (at == 0x304) {
                assert(byte == 1 && !f.available);
                if (!f.silent) {
                    f.response = (f.wrongId ? (id + 1) % 15 : id) << 8;
                    f.response |= f.slave[address]; f.available = true;
                    f.wrongId = false;
                }
            } else {
                if (f.rejectRestore && address == 0x3022) return -5;
                if (f.ignoredSlave == address) { f.ignoredSlave = 0; return 0; }
                f.slave[address] = byte;
            }
            return 0;
        }
        f.reg[{region, at}] = value; return 0;
    }
    static void delay(void *ctx, unsigned us) { static_cast<Fake *>(ctx)->waited += us; }
    static int rails(void *ctx, unsigned action) {
        auto &f = *static_cast<Fake *>(ctx);
        if (action == 2) { ++f.releases; return 0; }
        return action ? f.railCheckError : f.railError;
    }
    PicassoMicHardwareCore core() {
        PicassoMicHardwareCore h{};
        h.io = {this, read, write, delay, rails, nullptr}; return h;
    }
};

void completeRoute() {
    Fake f; auto h = f.core();
    assert(picasso_mic_hardware_action(h, PicassoMicPrepare) == 0);
    assert(h.status.ready && !h.status.unmuted && h.status.slaveId == 0xd01170223ULL);
    assert(f.slave[0x3022] == 0x63); // 2.75 V, micbias on
    assert(f.slave[0x3410] == 3 && f.slave[0x300e] == 12);
    assert(f.slave[0x128] == 1 && f.slave[0x138] == 1);
    assert(f.slave[0xe0] == 1 && f.slave[0xf0] == 1);
    assert(f.reg[Key(PicassoMicSwr, 0x1224)] == 0x01000001);
    assert(f.reg[Key(PicassoMicSwr, 0x1264)] == 0x01000001);
    assert(f.reg[Key(PicassoMicTx, 0x100)] == 1);
    assert(f.reg[Key(PicassoMicTx, 0x104)] == 1);
    assert(f.reg[Key(PicassoMicTx, 0x404)] == 0xa5000040);
    assert(f.reg[Key(PicassoMicTx, 0x400)] == 0x34);
    assert(f.broadcasts == 1);
    uint64_t before = f.waited;
    assert(picasso_mic_hardware_action(h, PicassoMicUnmute) == 0);
    assert(f.waited - before >= 100000 && h.status.unmuted);
    assert((f.reg[Key(PicassoMicTx, 0x400)] & 0x10) == 0);
    unsigned writes = f.writes.size();
    assert(picasso_mic_hardware_action(h, PicassoMicCheck) == 0);
    assert(f.writes.size() == writes);
    assert(picasso_mic_hardware_action(h, PicassoMicStop) == 0);
    assert(!h.savedCount && !h.status.rails && !h.status.ready && !h.status.restoreError);
    assert(f.reg[Key(PicassoMicTx, 8)] == 0);
    assert(f.reg[Key(PicassoMicVa, 0)] == 0);
    assert(f.reg[Key(PicassoMicLpi, 0)] == 0x80000000);
    assert(f.reg[Key(PicassoMicTx, 0x404)] == 0xa5000000);
    assert(f.slave[0x3022] == 0 && f.slave[0x3410] == 0 && f.slave[0x120] == 0);
    assert(f.releases == 1 && f.broadcasts == 2);
    assert(picasso_mic_hardware_action(h, PicassoMicStop) == 0 && f.releases == 1);
    assert(picasso_mic_hardware_action(h, PicassoMicPrepare) == 0);
    assert(picasso_mic_hardware_action(h, PicassoMicStop) == 0);
}
void gatesAndTransport() {
    { Fake f; auto h = f.core(); f.railError = -19;
      assert(picasso_mic_hardware_action(h, 0) == -19);
      assert(!f.reads && f.writes.empty() && h.status.failedStage == PicassoMicRails); }
    { Fake f; auto h = f.core(); f.reg[{PicassoMicSwr, 0x538}] ^= 1;
      assert(picasso_mic_hardware_action(h, 0) == -19 && !f.commands);
      assert(!h.status.ready && picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.full = true;
      assert(picasso_mic_hardware_action(h, 0) == -110 && !f.commands);
      assert(f.waited < 1000000); f.full = false;
      assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.silent = true;
      assert(picasso_mic_hardware_action(h, 0) == -110 && f.commands == 1);
      assert(!h.status.ready); f.silent = false;
      assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.wrongId = true;
      assert(picasso_mic_hardware_action(h, 0) == -71 && f.commands == 1);
      assert(!h.status.ready && picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.available = true; f.response = 0x138;
      assert(picasso_mic_hardware_action(h, 0) == -71 && !f.commands);
      f.available = false; assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.reg[{PicassoMicSwr, 0x1224}] = 0x01000001;
      assert(picasso_mic_hardware_action(h, 0) == -16 && !f.commands);
      assert(f.reg[Key(PicassoMicSwr, 0x1224)] == 0x01000001);
      assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core(); f.noBankAck = true;
      assert(picasso_mic_hardware_action(h, 0) == -110 && !h.status.ready);
      f.noBankAck = false; assert(picasso_mic_hardware_action(h, 2) == 0); }
}
void failureRestoration() {
    { Fake f; auto h = f.core(); f.ignoredSlave = 0x3022;
      assert(picasso_mic_hardware_action(h, 0) == -5 && !h.status.ready);
      assert(h.savedCount && picasso_mic_hardware_action(h, 2) == 0);
      assert(f.slave[0x3022] == 0 && !h.savedCount); }
    { Fake f; auto h = f.core();
      assert(picasso_mic_hardware_action(h, 0) == 0);
      f.rejectRestore = true;
      assert(picasso_mic_hardware_action(h, 2) == -5);
      assert(h.savedCount && h.status.restoreError == -5 && h.status.rails);
      assert(f.reg[Key(PicassoMicTx, 8)] & 1); // keep clocks for recovery
      assert(f.releases == 0 && picasso_mic_hardware_action(h, 0) == -16);
      f.rejectRestore = false;
      assert(picasso_mic_hardware_action(h, 2) == 0 && !h.savedCount && f.releases == 1); }
    { Fake f; auto h = f.core();
      assert(picasso_mic_hardware_action(h, 0) == 0);
      f.railCheckError = -5;
      assert(picasso_mic_hardware_action(h, 1) == -5 && !h.status.unmuted);
      assert(f.reg[Key(PicassoMicTx, 0x400)] & 0x10);
      assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core();
      assert(picasso_mic_hardware_action(h, 0) == 0);
      assert(picasso_mic_hardware_action(h, 1) == 0);
      f.railCheckError = -5;
      unsigned writes = f.writes.size();
      assert(picasso_mic_hardware_action(h, PicassoMicCheck) == -5);
      assert(!h.status.ready && f.writes.size() == writes);
      assert(picasso_mic_hardware_action(h, 2) == 0); }
    { Fake f; auto h = f.core();
      assert(picasso_mic_hardware_action(h, 0) == 0);
      assert(picasso_mic_hardware_action(h, 1) == 0);
      f.reg[{PicassoMicSwr, 0x1090}] = 0;
      assert(picasso_mic_hardware_action(h, PicassoMicCheck) == -19);
      assert(!h.status.ready && picasso_mic_hardware_action(h, 2) == 0); }
}
} // namespace
int main() {
    completeRoute(); gatesAndTransport(); failureRestoration();
    std::puts("picasso microphone hardware: route and fault-injection checks passed");
}
