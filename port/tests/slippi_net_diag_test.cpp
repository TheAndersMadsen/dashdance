// Network-diagnostics semantics: interval maxima drain at each sample, ages
// measure since the last observation, counters persist across rows, and the
// CSV carries the columns it promises. The host symbols the sampler reads are
// stubbed to fixed values.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_net_diag.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace host {
void log(const char*, ...) {}
uint32_t retrace_count() { return 1234; }
int online_ping_ms() { return 42; }
}  // namespace host
namespace slippi::online {
int32_t current_online_frame() { return 5678; }
}

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "net_diag assertion failed: %s\n", what);
  }
}
std::vector<std::vector<std::string>> read_csv(const char* path) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line.rfind("unix_us,", 0) == 0) continue;
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
      const size_t comma = line.find(',', start);
      fields.push_back(line.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    rows.push_back(std::move(fields));
  }
  return rows;
}
}  // namespace

int main() {
  const char* cache = "/tmp/melee_net_diag_test";
  std::system("mkdir -p /tmp/melee_net_diag_test && rm -f /tmp/melee_net_diag_test/network.csv");

  net_diag::match_started(cache);
  net_diag::on_pad_sent(1000000);
  net_diag::on_pad_sent(1016000);          // 16 ms submission gap
  net_diag::on_pad_received(1100000, 3);
  net_diag::on_pad_received(1108000, 4);   // 8 ms receive gap, 7 frames total
  net_diag::on_ack(25000);
  net_diag::on_ack(40000);                 // peak 40 ms
  net_diag::on_rollback();
  net_diag::on_service(5000);
  net_diag::on_service(12000);             // service peak 12 ms
  net_diag::on_queue_depth(3);
  net_diag::maybe_sample(2000000);         // the first call only anchors the 1 s window
  net_diag::maybe_sample(2200000);         // inside the window: no row
  net_diag::maybe_sample(3000000);         // first row
  net_diag::maybe_sample(4000000);         // second row: cumulative counters persist, maxima drained

  net_diag::match_ended("test_end");
  net_diag::maybe_sample(5000000);         // inactive: must not add rows

  const auto rows = read_csv("/tmp/melee_net_diag_test/network.csv");
  check(rows.size() == 4, "two samples plus the game_start and test_end rows");
  const auto& start = rows[0], & first = rows[1], & second = rows[2], & end_row = rows[3];
  check(start[1] == "game_start" && first[1] == "sample" && second[1] == "sample" && end_row[1] == "test_end",
        "row events in order");
  check(second[5] == "0", "interval maxima drained at the first sample");
  check(first.size() == 19, "19 columns per row");
  check(first[9] == "2" && first[11] == "2", "send and receive packet counts");
  check(first[10] == "16000" && first[12] == "8000" && first[13] == "1892000", "interval maxima and receive age");
  check(first[14] == "2" && first[15] == "65000", "ack count and cumulative latency");
  check(first[16] == "3", "queue depth maximum");
  check(first[17] == "17.00" && first[18] == "12000", "service total and peak");
  check(first[4] == "1" && first[5] == "1", "rollback total and interval");
  check(first[2] == "5678" && first[3] == "1234" && first[6] == "42", "guest frame, retrace and ping passthrough");
  check(second[4] == "1" && second[5] == "0", "rollback total persists, interval drains");

  std::system("rm -rf /tmp/melee_net_diag_test");
  if (failures) return 1;
  std::printf("net_diag: all checks passed\n");
  return 0;
}
