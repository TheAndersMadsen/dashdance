// Host and network-diagnostics stubs for test binaries that compile slippi_net.cpp on its own
// (no runtime, no game). Include from exactly one translation unit.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace host { void set_online_ping_ms(int) {} }
namespace net_diag {
void match_started(const char*) {}
void match_ended(const char*) {}
void on_pad_sent(uint64_t) {}
void on_pad_received(uint64_t, int) {}
void on_ack(uint64_t) {}
void on_rollback() {}
void on_service(uint64_t) {}
void on_queue_depth(size_t) {}
void maybe_sample(uint64_t) {}
}  // namespace net_diag
