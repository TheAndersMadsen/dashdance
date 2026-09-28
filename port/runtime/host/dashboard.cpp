// SPDX-License-Identifier: GPL-2.0-or-later
#include "dashboard.h"
#include "slippi_http_apple.h"
#include <cstdio>
#include <exception>
#include <cstdlib>

namespace host {
std::string Dashboard::rank() const {
  if (!profile_loaded || !profile.ranked) return "Unranked";
  return slippi::login::rank_name(profile.rating, profile.rating_updates, profile.global_placement);
}
std::string Dashboard::rating() const {
  if (!profile_loaded || !profile.ranked) return "—";
  char b[32]; std::snprintf(b, sizeof b, "%.1f", profile.rating); return b;
}
float Dashboard::win_rate() const {
  const int n = profile.wins + profile.losses;
  return n ? (float)profile.wins / (float)n : 0.0f;
}
std::string Dashboard::record() const {
  if (!profile_loaded || !profile.ranked) return "No ranked sets yet";
  const int n = profile.wins + profile.losses;
  char b[64]; std::snprintf(b, sizeof b, "%d W · %d L%s", profile.wins, profile.losses, n ? "" : "");
  std::string s = b;
  if (n) { std::snprintf(b, sizeof b, "  (%.0f%%)", 100.0f * win_rate()); s += b; }
  return s;
}
std::string Dashboard::placement() const {
  if (!profile_loaded || !profile.ranked) return "";
  std::string s;
  char b[48];
  if (profile.global_placement > 0) { std::snprintf(b, sizeof b, "#%d global", profile.global_placement); s += b; }
  if (profile.regional_placement > 0) {
    std::snprintf(b, sizeof b, "%s#%d %s", s.empty() ? "" : " · ", profile.regional_placement, profile.continent.empty() ? "regional" : profile.continent.c_str()); s += b;
  }
  return s;
}
std::vector<std::string> Dashboard::mains() const {
  std::vector<std::string> out;
  for (size_t i = 0; i < profile.characters.size() && i < 3; ++i) {
    char b[64]; std::snprintf(b, sizeof b, "%s · %d game%s", slippi::login::character_name(profile.characters[i].character), profile.characters[i].games, profile.characters[i].games == 1 ? "" : "s");
    out.push_back(b);
  }
  return out;
}
std::vector<GameRow> Dashboard::rows() const {
  std::vector<GameRow> out;
  for (const auto& g : games) {
    GameRow r;
    std::string vs;
    int mine = -1;
    for (const auto& p : g.players) {
      if (p.type == 3) continue;
      if (!vs.empty()) vs += " vs ";
      vs += slippi::login::character_name(p.character);
      if (!p.name.empty()) vs += " (" + p.name + ")";
      if (!code.empty() && p.code == code) mine = p.port;
    }
    r.title = vs.empty() ? "Match" : vs;
    r.path = g.path;
    char b[96];
    const int secs = g.last_frame / 60;
    std::snprintf(b, sizeof b, "%s · %d:%02d · %s", slippi::login::stage_name(g.stage), secs / 60, secs % 60, g.started_at.c_str());
    r.subtitle = b;
    if (g.winner_port > 0) {
      if (mine > 0) { r.win = g.winner_port == mine; r.loss = !r.win; r.result = r.win ? "WIN" : "LOSS"; }
      else { std::snprintf(b, sizeof b, "P%d", g.winner_port); r.result = b; }
    }
    out.push_back(r);
  }
  return out;
}

// MELEE_DASHBOARD_SAMPLE=1 fills the dashboard with sample data (screenshots and layout work without a Slippi account).
static bool sample_dashboard(Dashboard& d) {
  const char* sample = std::getenv("MELEE_DASHBOARD_SAMPLE");
  if (!sample || !*sample || *sample == '0') return false;
  d.signed_in = true; d.name = "SAMPLE"; d.code = "SMPL#123"; d.profile_loaded = true;
  d.profile.ranked = true; d.profile.rating = 1874.2f; d.profile.rating_updates = 62; d.profile.wins = 41; d.profile.losses = 27;
  d.profile.global_placement = 0; d.profile.regional_placement = 0; d.profile.continent = "EU";
  d.profile.characters = {{2, 40}, {20, 18}, {0, 6}};
  d.profile.display_name = d.name; d.profile.connect_code = d.code;
  if (d.games.empty()) {
    for (int i = 0; i < 5; ++i) {
      slippi::history::Game g; g.online = true; g.stage = (i % 2) ? 31 : 32; g.last_frame = 60 * (150 + 40 * i); g.winner_port = (i == 1 || i == 3) ? 2 : 1;
      g.started_at = "2026-09-13 20:1" + std::to_string(i);
      slippi::history::Player p1; p1.type = 0; p1.port = 1; p1.character = 2; p1.name = "SAMPLE"; p1.code = "SMPL#123"; p1.stocks = g.winner_port == 1 ? 2 : 0;
      slippi::history::Player p2; p2.type = 0; p2.port = 2; p2.character = (i * 5) % 26; p2.name = "RIVAL"; p2.code = "RIVL#456"; p2.stocks = g.winner_port == 2 ? 1 : 0;
      g.players = {p1, p2}; d.games.push_back(g);
    }
  }
  return true;
}

static bool dashboard_load_profile_unsafe(const std::string& slippi_dir, Dashboard& d);
bool dashboard_load_profile(const std::string& slippi_dir, Dashboard& d) {
  try { return dashboard_load_profile_unsafe(slippi_dir, d); }
  catch (const std::exception& e) { d.profile_loaded = false; d.profile_error = std::string("Could not read your Slippi profile (") + e.what() + ")."; return false; }
  catch (...) { d.profile_loaded = false; d.profile_error = "Could not read your Slippi profile."; return false; }
}
static bool dashboard_load_profile_unsafe(const std::string& slippi_dir, Dashboard& d) {
  if (sample_dashboard(d)) return true;
  slippi::login::Account account;
  d.signed_in = slippi::login::read_user_file(slippi_dir, account);
  if (d.signed_in) { d.name = account.display_name; d.code = account.connect_code; }
  d.profile_loaded = false; d.profile_error.clear();
  slippi::login::Session session;
  if (!d.signed_in || !slippi::login::read_session(slippi_dir, session)) { d.profile_error = d.signed_in ? "Sign in again to see ranked stats." : ""; return false; }
  std::string token, error;
  if (!slippi::login::refresh_id_token(session, token, error)) { d.profile_error = error; return false; }
  if (!slippi::login::fetch_profile(token, session.uid.empty() ? account.uid : session.uid, d.profile, error)) { d.profile_error = error; return false; }
  d.profile_loaded = true;
  if (!d.profile.display_name.empty()) d.name = d.profile.display_name;
  if (!d.profile.connect_code.empty()) d.code = d.profile.connect_code;
  return true;
}
bool dashboard_load_network(Dashboard& d) {
  int status = 0; std::string body, error;
  if (!slippi::report::apple_http("GET", "https://api4.ipify.org", "", "", "Dashdance", &status, &body, &error)) { d.ipv4_error = error.empty() ? "no connection" : error; return false; }
  if (status != 200) { d.ipv4_error = "HTTP " + std::to_string(status); return false; }
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) body.pop_back();
  if (body.empty() || body.find('.') == std::string::npos || body.size() > 15) { d.ipv4_error = "unexpected answer"; return false; }
  d.public_ipv4 = body;
  return true;
}
void dashboard_load_games(const std::string& replay_dir, Dashboard& d, size_t limit) {
  try { d.games = slippi::history::recent_games(replay_dir, limit); } catch (...) { d.games.clear(); }
}
}  // namespace host
