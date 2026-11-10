/* Navi5 — dev-only numeric socket observation, never transport policy.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#ifdef NAVI5_DEV_TOOLS
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>
namespace jf {
// Render reads without mailbox/log locks. Single writer: login worker.
struct LiveProgress {
    std::atomic<unsigned> version{0};
    std::atomic<int> phase{0}, op{0}, cmd{0}, error{0};
    std::atomic<long long> arg{0}, result{0};
    // Last kRing events, so a stall screenshot also shows send/poll before it.
    static constexpr unsigned kRing = 6;
    struct Slot {
        std::atomic<unsigned> seq{0};
        std::atomic<int> phase{0}, op{0}, cmd{0}, error{0};
        std::atomic<long long> arg{0}, result{0};
    } ring[kRing];
    void publish(int p, int o, int c, long long a, long long r, int e) {
        const unsigned seq = version.fetch_add(1) / 2;
        phase.store(p); op.store(o); cmd.store(c); arg.store(a); result.store(r); error.store(e);
        Slot &s = ring[seq % kRing];
        s.seq.store(seq); s.phase.store(p); s.op.store(o); s.cmd.store(c);
        s.arg.store(a); s.result.store(r); s.error.store(e);
        version.fetch_add(1);
    }
    // Oldest first, same numeric format as line(); "updating" on torn read.
    std::string ring_line() const {
        const unsigned before = version.load();
        const unsigned n = before / 2;
        std::string out = "DEV ring";
        for (unsigned seq = n > kRing ? n - kRing : 0; seq < n; ++seq) {
            const Slot &s = ring[seq % kRing];
            char row[96];
            std::snprintf(row, sizeof row, " %u/%d/%d/%d/%lld/%lld/%d", s.seq.load(), s.phase.load(),
                          s.op.load(), s.cmd.load(), s.arg.load(), s.result.load(), s.error.load());
            out += row;
        }
        return !(before & 1) && before == version.load() ? out : "DEV ring=updating";
    }
    std::string line() const {
        const unsigned before = version.load();
        char row[192];
        std::snprintf(row, sizeof row, "DEV live=%u/%d/%d/%d/%lld/%lld/%d", before / 2,
                      phase.load(), op.load(), cmd.load(), arg.load(), result.load(), error.load());
        return !(before & 1) && before == version.load() ? row : "DEV live=updating";
    }
};
static_assert(std::atomic<long long>::is_always_lock_free);
inline LiveProgress live_progress;
// Direct DEV sink; only numeric scalars, never pointers or request contents.
void trace_begin();
void trace_end();
void trace_live(int phase, int op, int cmd, long long arg, long long result, int error);
struct SocketTrace {
    struct Event { int op, cmd, result, error; };
    Event events[16]{};
    unsigned count = 0, dropped = 0;
    bool active = false;
    void record(int op, int cmd, int result, int error) {
        if (!active) return;
        if (count < 16) events[count++] = {op, cmd, result, error};
        else if (dropped < 999999) ++dropped;
    }
    std::string summary() const {
        // Missing observation stays -1, distinct from successful result zero.
        Event failed{-1, -1, -1, -1};
        for (unsigned i = 0; i < count; ++i)
            if (events[i].result < 0) { failed = events[i]; break; }
        char out[160];
        std::snprintf(out, sizeof out, "trace=%u/%u first=%d/%d/%d/%d", count, dropped,
                      failed.op, failed.cmd, failed.result, failed.error);
        return out;
    }
    std::string lines() const {
        std::string out;
        for (unsigned i = 0; i < count; ++i) {
            char row[128];
            const auto &e = events[i];
            std::snprintf(row, sizeof row, "sock=%u/%d/%d/%d/%d\n", i, e.op, e.cmd, e.result, e.error);
            out += row;
        }
        return out;
    }
};
inline thread_local SocketTrace socket_trace;
}
#endif
