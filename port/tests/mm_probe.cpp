// Manual probe of Slippi 3.7.0 matchmaking against the real service: STUN discovery, the websocket
// join, and (by default) an unranked ticket that is cancelled the moment the service queues it, so
// nobody gets paired. `direct` instead queues for a connect code nobody owns and stays in the queue
// for the whole run (liveness pings, STUN keepalives), then leaves.
// Usage: port_mm_probe <user_dir> [seconds] [unranked|direct]
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(MM_PROBE_STANDALONE)
#include "slippi_net_test_stubs.h"
namespace host {
void log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}
}  // namespace host
#else
namespace host { void log(const char* fmt, ...); }
#endif

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: port_mm_probe <user_dir> [seconds] [unranked|direct]\n"); return 2; }
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;
  const bool direct = argc > 3 && std::strcmp(argv[3], "direct") == 0;
  slippi::User user(argv[1]);
  if (!user.IsLoggedIn()) { std::fprintf(stderr, "no user.json in %s\n", argv[1]); return 1; }
  if (!slippi::enet_ready()) return 1;
  int result = 1;
  {
    slippi::Matchmaking mm(&user);
    slippi::Matchmaking::MatchSearchSettings s;
    s.mode = direct ? slippi::Matchmaking::DIRECT : slippi::Matchmaking::UNRANKED;
    if (direct) s.connect_code = "ZZZZ#999";
    mm.FindMatch(s);
    const auto t0 = std::chrono::steady_clock::now();
    slippi::Matchmaking::ProcessState last = slippi::Matchmaking::IDLE;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
      const auto st = mm.GetMatchmakeState();
      if (st != last) { std::printf("state %d %s\n", (int)st, mm.GetErrorMessage().c_str()); std::fflush(stdout); last = st; }
      if (mm.IsInQueue() && result != 0) {
        std::printf("QUEUED: the service accepted the ticket%s\n", direct ? "; holding" : "; cancelling");
        std::fflush(stdout);
        result = 0;
        if (!direct) break;
      }
      if (direct && result == 0 && !mm.IsInQueue()) { std::printf("left the queue unexpectedly\n"); result = 1; break; }
      if (st == slippi::Matchmaking::ERROR_ENCOUNTERED || st == slippi::Matchmaking::CONNECTION_SUCCESS) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("final state %d (%s)\n", (int)mm.GetMatchmakeState(), mm.GetErrorMessage().c_str());
  }   // the destructor leaves the queue
  std::this_thread::sleep_for(std::chrono::seconds(2));   // let the detached session send its cancel and close
  return result;
}
