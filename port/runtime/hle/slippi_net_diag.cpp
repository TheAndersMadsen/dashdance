// See slippi_net_diag.h. All hooks are atomics so the network thread (which
// produces and samples) and the simulation thread (which submits inputs and
// rolls back) never block each other; the file write happens on the network
// thread alone.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net_diag.h"
#include "host.h"
#include "slippi_online.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace net_diag {
namespace {
struct Counter {
  std::atomic<uint64_t> value{0};
  uint64_t operator++() { return value.fetch_add(1, std::memory_order_relaxed) + 1; }
  uint64_t add(uint64_t v) { return value.fetch_add(v, std::memory_order_relaxed); }
  uint64_t load() const { return value.load(std::memory_order_relaxed); }
};
struct Maximum {   // per-interval maximum, drained at each sample
  std::atomic<uint64_t> value{0};
  void offer(uint64_t v) {
    uint64_t prev = value.load(std::memory_order_relaxed);
    while (v > prev && !value.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
  }
  uint64_t drain() { return value.exchange(0, std::memory_order_relaxed); }
};

Counter g_pads_sent, g_pads_recv, g_input_frames_recv, g_acks, g_acks_latency_us, g_rollbacks, g_service_us;
Counter g_rollbacks_interval;
Maximum g_send_gap_us, g_recv_gap_us, g_ack_latency_us, g_queue_depth, g_service_us_max;
std::atomic<uint64_t> g_last_sent_us{0}, g_last_recv_us{0}, g_last_sample_us{0};
std::atomic<bool> g_active{false};

const char* kHeader =
    "unix_us,event,online_frame,retrace,rollbacks,rollbacks_1s,ping_ms,ping_ms_max_1s,ping_ms_avg,"
    "pads_sent,send_gap_max_us,pads_recv,recv_gap_max_us,recv_age_us,acks,ack_latency_sum_us,queue_max,"
    "service_ms_1s,service_ms_max_us\n";

std::string g_path;
FILE* g_file = nullptr;

uint64_t now_unix_us() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void row(const char* event, uint64_t us) {
  if (!g_file) return;
  const uint64_t acks = g_acks.load(), ack_us = g_acks_latency_us.load();
  const uint64_t recv_us = g_last_recv_us.load();
  std::fprintf(g_file, "%llu,%s,%d,%u,%llu,%llu,%d,%llu,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%llu\n",
               (unsigned long long)us, event,
               slippi::online::current_online_frame(), (unsigned)host::retrace_count(),
               (unsigned long long)g_rollbacks.load(), (unsigned long long)g_rollbacks_interval.value.exchange(0),
               host::online_ping_ms(),
               (unsigned long long)(g_ack_latency_us.drain() / 1000),
               acks ? (double)ack_us / (double)acks / 1000.0 : 0.0,
               (unsigned long long)g_pads_sent.load(), (unsigned long long)g_send_gap_us.drain(),
               (unsigned long long)g_pads_recv.load(), (unsigned long long)g_recv_gap_us.drain(),
               (unsigned long long)(recv_us && recv_us < us ? us - recv_us : 0),
               (unsigned long long)acks, (unsigned long long)g_acks_latency_us.load(),
               (unsigned long long)g_queue_depth.drain(),
               (double)g_service_us.value.exchange(0) / 1000.0, (unsigned long long)g_service_us_max.drain());
  std::fflush(g_file);
}
}  // namespace

void match_started(const char* cache_dir) {
  if (!g_file) {
    g_path = std::string(cache_dir) + "/network.csv";
    g_file = std::fopen(g_path.c_str(), "a");
    if (g_file) {
      std::setvbuf(g_file, nullptr, _IOFBF, 1u << 16);
      std::fseek(g_file, 0, SEEK_END);
      if (std::ftell(g_file) == 0) std::fputs(kHeader, g_file);
    } else {
      host::log("slippi: cannot open %s; network diagnostics disabled", g_path.c_str());
    }
  }
  g_active.store(true, std::memory_order_release);
  row("game_start", now_unix_us());
}

void match_ended(const char* why) {
  if (!g_active.exchange(false, std::memory_order_acq_rel)) return;
  row(why, now_unix_us());
  if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

void on_pad_sent(uint64_t us) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  const uint64_t last = g_last_sent_us.exchange(us, std::memory_order_relaxed);
  if (last) g_send_gap_us.offer(us - last);
  ++g_pads_sent;
}

void on_pad_received(uint64_t us, int frames) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  const uint64_t last = g_last_recv_us.exchange(us, std::memory_order_relaxed);
  if (last) g_recv_gap_us.offer(us - last);
  ++g_pads_recv;
  g_input_frames_recv.add((uint64_t)frames);
}

void on_ack(uint64_t latency_us) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  ++g_acks;
  g_acks_latency_us.add(latency_us);
  g_ack_latency_us.offer(latency_us);
}

void on_rollback() {
  ++g_rollbacks;
  ++g_rollbacks_interval;
}

void on_service(uint64_t duration_us) {
  g_service_us.add(duration_us);
  g_service_us_max.offer(duration_us);
}

void on_queue_depth(size_t packets) { g_queue_depth.offer((uint64_t)packets); }

void maybe_sample(uint64_t us) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  uint64_t last = g_last_sample_us.load(std::memory_order_relaxed);
  if (!last) {
    g_last_sample_us.store(us, std::memory_order_relaxed);
    return;
  }
  if (us - last < 1000000) return;
  if (g_last_sample_us.compare_exchange_strong(last, us, std::memory_order_relaxed)) row("sample", us);
}
}  // namespace net_diag
