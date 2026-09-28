// Dashboard model shared by the macOS and iOS launchers: the signed-in player's ranked profile,
// local match history and formatted strings, so the two UIs only lay things out.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_history.h"
#include "slippi_login.h"
#include <string>
#include <vector>

namespace host {
struct GameRow { std::string title, subtitle, result, path; bool win = false, loss = false; };   // path: the .slp
struct Dashboard {
  bool signed_in = false;
  std::string name, code;
  bool profile_loaded = false;
  std::string profile_error;
  slippi::login::Profile profile;
  std::vector<slippi::history::Game> games;
  std::string public_ipv4, ipv4_error;   // the address Slippi's matchmaking places you by

  std::string rank() const;            // "Gold II", "Pending", "Unranked"
  std::string rating() const;          // "1734.2"
  std::string record() const;          // "38 W · 21 L  (64%)"
  float win_rate() const;              // 0..1
  std::string placement() const;       // "#412 global · #57 Europe"
  std::vector<std::string> mains() const;   // "Fox · 120 games"
  std::vector<GameRow> rows() const;   // recent games, newest first
};
// Loads the saved session, refreshes the token and fetches the profile (blocking; run off the UI thread).
bool dashboard_load_profile(const std::string& slippi_dir, Dashboard& d);
bool dashboard_load_network(Dashboard& d);   // public IPv4 (api4.ipify.org)
void dashboard_load_games(const std::string& replay_dir, Dashboard& d, size_t limit);
}  // namespace host
