// This module has no profile, filesystem, host or transport dependency. Its
// response layout follows the existing online handlers, with unavailable states.
// Replay recording, playback, game files and jukebox remain owned by EXI.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_offline.h"
#include <algorithm>
#include <random>
#include <cstdlib>

namespace slippi::online::offline {
namespace {
std::mt19937 g_rng{0};
uint8_t g_delay = 2;
uint8_t g_rank_visibility = 3;

void append_u32(std::vector<uint8_t>& q, uint32_t value) {
  for (int i = 3; i >= 0; --i) q.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
}  // namespace

void reset(const Config& config) {
  g_rng.seed(config.offline_seed);
  g_delay = static_cast<uint8_t>(std::clamp(config.delay, 0, 255));
  g_rank_visibility = (config.show_local_rank ? 1 : 0) | (config.show_opponent_rank ? 2 : 0);
}

bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q) {
  switch (cmd) {
    case 0xB0: // ONLINE_INPUTS: existing disconnected response
      q.assign(1, 3);
      return true;
    case 0xB3: { // GET_MATCH_STATE: ERROR_ENCOUNTERED, no ready players or identity
      // 15 header + 6*31 names + 4*10 codes + 4*29 UIDs + 241 error
      // + 312 game-info block + 51 match ID + 1 alternate-stage byte.
      q.assign(962, 0);
      q[0] = 5;
      q[4] = 1;
      q[9] = g_delay;
      q[13] = q[14] = 0xFF;
      constexpr char error[] = "Online services are disabled in offline mode";
      std::copy(error, error + sizeof(error) - 1, q.begin() + 357);
      return true;
    }
    case 0xB9: { // GET_ONLINE_STATUS: logged out, empty 31-byte name and 10-byte code
      q.assign(42, 0);
      // MELEE_FAKE_LOGIN=1: diagnostic only. Report a logged-in state so the
      // Slippi main menu enables navigation without online services; never
      // set this in normal offline play.
      static const int fake_login = std::getenv("MELEE_FAKE_LOGIN") ? std::atoi(std::getenv("MELEE_FAKE_LOGIN")) : 0;
      if (fake_login) {
        q[0] = static_cast<uint8_t>(fake_login);
        const char name[] = "PLAYER";
        std::copy_n(name, sizeof(name) - 1, q.begin() + 1);
        const char code[] = "#0001";
        std::copy_n(code, sizeof(code) - 1, q.begin() + 32);
      }
      return true;
    }
    case 0xBC: // GET_NEW_SEED is a local operation, including offline play
      q.clear();
      append_u32(q, g_rng() % 0xFFFFFFFFu);
      return true;
    case 0xBE: { // FETCH_CODE_SUGGESTION: no history; preserve a valid typed code
      q.assign(30, 0);
      if (payload && payload_len >= 31 && payload[24] <= 8) {
        std::copy_n(payload, 3 * payload[24], q.begin() + 1);
        q[25] = payload[24];
        std::copy_n(payload + 25, 4, q.begin() + 26);
      }
      return true;
    }
    case 0xC1: // GP_FETCH_STEP: no opponent result available
      q.assign(6, 0);
      return true;
    case 0xC3: // GET_PLAYER_SETTINGS: no profile-owned chat text
      q.assign(4 * 16 * 51, 0);
      return true;
    case 0xD5: // GET_DELAY: local configuration is available
      q = {1, g_delay};
      return true;
    case 0xE3: // GET_RANK: RankFetchStatus::Error, no fetched result
      q.assign(16, 0);
      q[0] = g_rank_visibility;
      q[1] = 2;
      return true;
    case 0xE5: // GET_RANK_VISIBILITY: local configuration
      q.assign(1, g_rank_visibility);
      return true;
    // Commands with no response cannot acquire a capability. In particular,
    // login/logout do not read/write user.json; reporting queues nothing.
    case 0xB1: case 0xB2: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xBA: case 0xBB: case 0xBD: case 0xBF: case 0xC0:
    case 0xC2: case 0xC4: case 0xE4:
      q.clear();
      return true;
    default:
      return false;
  }
}
}  // namespace slippi::online::offline
