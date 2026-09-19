// One-second network diagnostics: counters for input production, packet flow,
// acknowledgement latency, send-queue residence and ENet service time, appended
// as CSV rows to <cache-dir>/network.csv during online matches. The point is to
// turn "the match stuttered" into a row that says whether the pause was before
// or after the network path. Payloads, addresses and account data never reach
// the file.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace net_diag {
// Match lifecycle: started once per game (EXI ONLINE_INPUTS frame 1), ended at
// teardown or report. `cache_dir` is the run's explicit output directory.
void match_started(const char* cache_dir);
void match_ended(const char* why);

// Producer hooks; cheap and wait-free. `us` timestamps come from the caller's
// clock (slippi_net's time_us).
void on_pad_sent(uint64_t us);            // a local input frame was queued for sending
void on_pad_received(uint64_t us, int frames);
void on_ack(uint64_t latency_us);
void on_rollback();
void on_service(uint64_t duration_us);    // time spent inside enet_host_service
void on_queue_depth(size_t packets);      // async send queue length at drain time

// Called from the network thread's service loop at least a few times a second:
// drains interval maxima into a CSV row once per 1 s window.
void maybe_sample(uint64_t us);
}  // namespace net_diag
