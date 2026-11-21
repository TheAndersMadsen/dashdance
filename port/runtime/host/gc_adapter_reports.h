// What Slippi 3.7.0's input suggestions need from a GameCube adapter backend (gc_adapter_iokit.cpp,
// gc_adapter_libusb.cpp, gc_adapter.cpp), after Dolphin's GCAdapter.cpp at v3.7.0: every report the reader thread
// stores is stamped with how many times the game itself had polled each port, so a report can be placed before or
// after a game poll, and handed to a callback outside the backend's lock.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"
#include <atomic>
#include <cstring>

namespace host::gc_adapter_reports {

constexpr size_t REPORT_SIZE = 37;   // a report id byte followed by 9 bytes per port

struct State {
  std::atomic<GcAdapterReportCallback> callback{nullptr};
  std::atomic<bool> replaced{false};       // the game is not reading the adapter's pads as they are (the in-game menu)
  std::atomic<uint32_t> game_ports{0};     // ports the game's most recent poll took from the adapter
  uint64_t game_poll_counts[4] = {};       // guarded by the backend's report mutex
};

// Reader thread, under the report mutex right after a report was stored: copies what the callback needs. Returns
// false when there is nothing to report.
inline bool capture(State& s, const uint8_t report[REPORT_SIZE], uint64_t counts_out[4], uint8_t payload_out[REPORT_SIZE]) {
  if (!s.callback.load(std::memory_order_acquire) || s.replaced.load(std::memory_order_relaxed)) return false;
  std::memcpy(counts_out, s.game_poll_counts, sizeof s.game_poll_counts);
  std::memcpy(payload_out, report, REPORT_SIZE);
  return true;
}

// Reader thread, outside the report mutex, so the game's reads are never held up by the callback.
inline void deliver(State& s, const uint64_t counts[4], const uint8_t payload[REPORT_SIZE]) {
  if (GcAdapterReportCallback callback = s.callback.load(std::memory_order_acquire)) callback(counts, payload, REPORT_SIZE);
}

// CPU thread, under the report mutex while the report is copied out: a read the game itself makes counts as a poll of
// every port, because the game reads all four each frame.
inline void count_game_poll(State& s) {
  if (!g_game_pad_poll) return;
  for (uint64_t& count : s.game_poll_counts) ++count;
}

}  // namespace host::gc_adapter_reports
