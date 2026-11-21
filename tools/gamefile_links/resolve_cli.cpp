// SPDX-License-Identifier: GPL-2.0-or-later
// Command-line front end to Dashdance's Slippi game-file link resolver, for the golden comparison
// with slippi-rust-extensions' `slippi-hsd resolve` (tools/gamefile_links/golden.py).
//
//   resolve_cli <archive> --disc <dir of disc files> -o <out>
//
// Runs resolve_links(), the same entry point the game-file loader calls, reading disc files by
// name from <dir>. Exit 0 when resolved, 1 when the file has no links, 2 on failure.
#include "slippi_gamefile_links.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace slippi::gamefile_links;

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

int main(int argc, char** argv) {
  std::string archive, disc, out;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--disc" && i + 1 < argc) disc = argv[++i];
    else if (a == "-o" && i + 1 < argc) out = argv[++i];
    else archive = a;
  }
  if (archive.empty() || disc.empty() || out.empty()) {
    std::fprintf(stderr, "usage: resolve_cli <archive> --disc <dir> -o <out>\n");
    return 2;
  }
  std::vector<uint8_t> contents;
  if (!read_file(archive, contents)) { std::fprintf(stderr, "cannot read %s\n", archive.c_str()); return 2; }
  const size_t in = contents.size();
  ReadDiscFile reader = [&](const std::string& name, std::vector<uint8_t>& bytes) {
    return read_file(disc + "/" + name, bytes) && !bytes.empty();
  };
  Outcome o = resolve_links(archive, contents, reader);
  for (const auto& w : o.warnings) std::printf("warning: %s\n", w.c_str());
  if (o.status == Status::NoLinks) {
    std::printf("no links%s%s\n", o.error.empty() ? "" : ": ", o.error.c_str());
    return 1;
  }
  if (o.status == Status::Failed) { std::fprintf(stderr, "error: %s\n", o.error.c_str()); return 2; }
  // Status counts in first-seen order, as slippi-hsd prints them.
  std::vector<std::pair<LinkStatus, size_t>> counts;
  for (const auto& l : o.resolved) {
    auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == l.status; });
    if (it == counts.end()) counts.emplace_back(l.status, 1);
    else ++it->second;
  }
  std::string summary;
  for (const auto& [status, n] : counts)
    summary += (summary.empty() ? "" : ", ") + std::to_string(n) + " " + status_name(status);
  std::printf("links: %s\n", summary.c_str());
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char*>(contents.data()), std::streamsize(contents.size()));
  if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 2; }
  std::printf("resolved %zu links using %zu disc file(s); %zu bytes in, %zu bytes out\n", o.links, o.disc_files.size(), in,
              contents.size());
  return 0;
}
