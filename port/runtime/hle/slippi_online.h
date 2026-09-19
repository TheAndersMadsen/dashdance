// Slippi Online command handling for the EXI device: matchmaking, netplay input exchange,
// rollback savestates, chat, match state. Port of the online half of Dolphin's CEXISlippi.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::online {

struct Config {
  std::string user_dir = "runtime/slippi/User/Slippi";   // user.json, direct-codes.json (Slippi Launcher layout)
  // Set before init. Offline initialization never reads a profile, starts workers,
  // hashes the ISO, initializes ENet or contacts a service. Changes require shutdown.
  bool offline = true;
  uint32_t offline_seed = 0;     // reproducible local seed requests; no wall-clock source
  int delay = 2;                 // Slippi Online input delay (frames)
  int chat = 0;                  // 0 enabled, 1 direct only, 2 disabled
  bool show_local_rank = true, show_opponent_rank = true;
};
Config& config();

void init();
void shutdown();
// Whether this build contains an online implementation (not login or matchmaking success).
bool available();
// Handles one online command (cmd byte, payload after it); responses go to `read_queue`.
// Returns false for commands this module does not own.
bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& read_queue);
// Counts rollback loads so the renderer can treat them as discontinuities.
uint64_t rollback_count();
int32_t current_online_frame();
bool is_online_match();

}  // namespace slippi::online
