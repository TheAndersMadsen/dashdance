// SPDX-License-Identifier: GPL-2.0-or-later
// Slippi game-file links without game data: the unit tests of slippi-rust-extensions' hsd crate
// (archive.rs, hash.rs, link.rs) ported one for one, plus the slprs_gamefile_resolve front door.
#include "slippi_gamefile_links.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

using namespace slippi::gamefile_links;

static int g_failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

template <class F>
static std::string error_of(F&& f) {
  try { f(); } catch (const Error& e) { return e.what(); }
  return {};
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// root -> A (two pointers) -> B (buffer) and C (buffer, shared by A twice).
static Archive sample() {
  Archive a;
  std::memcpy(a.version.data(), "001B", 4);
  Extent e0; e0.data.assign(16, 0); e0.phase = 0; e0.origin = 0;
  e0.pointers = {{0, 1}, {4, 2}, {8, 2}};
  Extent e1; for (int i = 1; i <= 40; ++i) e1.data.push_back(uint8_t(i)); e1.phase = 16; e1.origin = 16;
  Extent e2; e2.data.assign(64, 7); e2.phase = 0; e2.origin = 64;
  a.extents = {e0, e1, e2};
  a.roots = {{"sample_root", 0}};
  return a;
}

static LoadFn files_loader(std::map<std::string, std::vector<uint8_t>> files) {
  return [files](const std::string& name, std::vector<uint8_t>& out, std::string& error) {
    auto it = files.find(name);
    if (it == files.end()) { error = "no such file"; return false; }
    out = it->second;
    return true;
  };
}
static LoadFn disc_loader(const std::vector<uint8_t>& bytes) { return files_loader({{"Sample.dat", bytes}}); }
static bool same_structure(const Archive& a, const Archive& b) { return archive_hash(a) == archive_hash(b); }
static size_t follow_path(const Archive& a, const char* root, std::vector<uint32_t> hops) {
  size_t cur = SIZE_MAX;
  for (const auto& r : a.roots) if (r.name == root) { cur = r.extent; break; }
  for (uint32_t h : hops) {
    size_t next = SIZE_MAX;
    for (const auto& p : a.extents[cur].pointers) if (p.field == h) { next = p.target; break; }
    cur = next;
  }
  return cur;
}

static void serialize_then_parse_is_identity() {
  Archive a = sample();
  auto bytes = a.serialize();
  Archive b = Archive::parse(bytes);
  CHECK(b.serialize() == bytes);
  CHECK(b.extents.size() == 3);
  CHECK(b.extents[0].pointers.size() == 3);
  CHECK(b.roots[0].name == "sample_root");
  auto offsets = b.layout();
  CHECK(offsets[1] % kAlign == 16);
  CHECK(offsets[2] % kAlign == 0);
}

static void extern_chain_round_trips() {
  Archive a = sample();
  std::vector<Pointer> kept;
  for (auto& p : a.extents[0].pointers) if (p.target != 2) kept.push_back(p);
  a.extents[0].pointers = kept;
  a.refs.push_back({"ext_sym", {{0, 4}, {0, 8}}});
  a.remove_extents({2});
  auto bytes = a.serialize();
  Archive b = Archive::parse(bytes);
  CHECK(b.refs.size() == 1);
  CHECK((b.refs[0].locations == std::vector<std::pair<size_t, uint32_t>>{{0, 4}, {0, 8}}));
  CHECK(b.serialize() == bytes);
  CHECK(bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 1);   // chain fields are not relocations
}

static void relayout_does_not_change_hash() {
  Archive a = sample();
  Archive b = a;
  std::swap(b.extents[1], b.extents[2]);
  for (auto& p : b.extents[0].pointers) p.target = p.target == 1 ? 2 : p.target == 2 ? 1 : p.target;
  b.extents[2].data.insert(b.extents[2].data.end(), 24, 0);
  CHECK(same_structure(a, b));
  CHECK(a.serialize() != b.serialize());
}

static void content_change_changes_hash() {
  Archive a = sample();
  Archive b = a;
  b.extents[2].data[3] = 9;
  CHECK(!same_structure(a, b));
  Archive c = a;
  Extent e; e.data = {1, 2, 3, 4};
  c.extents.push_back(e);
  c.extents[0].pointers.push_back({12, 3});
  CHECK(!same_structure(a, c));
}

static void locator_round_trip() {
  Locator l = Locator::parse("slp:GmTou1p.dat:0x1a2b0:cut=c,4:sha=deadbeef");
  CHECK(l.file == "GmTou1p.dat");
  CHECK(l.anchor.is_offset && l.anchor.offset == 0x1a2b0);
  CHECK((l.cut == std::vector<uint32_t>{0xc, 0x4}));
  CHECK(l.sha && *l.sha == "deadbeef");
  CHECK(l.symbol() == "slp:GmTou1p.dat:0x1a2b0:cut=c,4:sha=deadbeef");

  Locator p = Locator::parse("slp:MnSlChr.usd:MnSelectChrDataTable/68/0/4*3/8:at=0x1234:sha=0badf00d:strict=1");
  CHECK(!p.anchor.is_offset && p.anchor.root == "MnSelectChrDataTable");
  CHECK((p.anchor.hops == std::vector<uint32_t>{0x68, 0, 4, 4, 4, 8}));
  CHECK(p.at && *p.at == 0x1234);
  CHECK(p.strict);
  CHECK(p.symbol() == "slp:MnSlChr.usd:MnSelectChrDataTable/68/0/4*3/8:at=0x1234:sha=0badf00d:strict=1");
  Locator empty = Locator::parse("slp:GmTitle.usd:ScTitle_fog_foganim/");
  CHECK(!empty.anchor.is_offset && empty.anchor.root == "ScTitle_fog_foganim" && empty.anchor.hops.empty());

  CHECK(has(error_of([] { Locator::parse("slp:GmTou1p.dat"); }), "missing anchor"));
  CHECK(has(error_of([] { Locator::parse("slp:GmTou1p.dat:0x10"); }), "offset is inside the header"));
  CHECK(has(error_of([] { Locator::parse("slp:GmTou1p.dat:0x40:bogus=1"); }), "unknown option bogus=1"));
  CHECK(has(error_of([] { Locator::parse("slp:GmTou1p.dat:/4"); }), "missing root symbol"));
  CHECK(has(error_of([] { Locator::parse("slp::0x40"); }), "missing file name"));
  CHECK(has(error_of([] { Locator::parse("slp:A.dat:0x40:sha=123"); }), "unknown option sha=123"));
  CHECK(has(error_of([] { Locator::parse("slp:A.dat:0x40:"); }), "unknown option "));
  CHECK(has(error_of([] { Locator::parse("slp:A.dat:r/4*x"); }), "bad repeat count x"));
  CHECK(has(error_of([] { Locator::parse("slp:A.dat:0x"); }), "bad number 0x"));
  CHECK(Locator::parse("slp:A.dat:+40").anchor.offset == 0x40);   // Rust's from_str_radix takes a leading +
  CHECK(Locator::parse("slp:A.dat:0x40:sha=DEADBEEF").sha == std::optional<std::string>("deadbeef"));
  CHECK(!Locator::is_link("ScTitle_fog_foganim"));
}

static void locate_prefers_a_path() {
  Archive a = sample();
  Locator l = locate(a, "Sample.dat", 2, {});
  CHECK(!l.anchor.is_offset && l.anchor.root == "sample_root" && (l.anchor.hops == std::vector<uint32_t>{4}));
  CHECK(l.at && *l.at == kHeaderSize + 64);
  CHECK(l.sha && *l.sha == asset_sha(a, 2, {}));
}

static void link_then_resolve_restores_structure() {
  Archive original = sample();
  auto disc = original.serialize();
  Archive linked = original;
  std::string symbol = locate(original, "Sample.dat", 2, {}).symbol();
  link(linked, 2, symbol);
  CHECK(linked.extents.size() == 2);
  CHECK(linked.refs.size() == 1);
  CHECK(linked.refs[0].locations.size() == 2);
  auto bytes = linked.serialize();
  std::vector<uint8_t> sevens(64, 7);
  CHECK(std::search(bytes.begin(), bytes.end(), sevens.begin(), sevens.end()) == bytes.end());

  Archive reparsed = Archive::parse(bytes);
  Resolution r = resolve(reparsed, disc_loader(disc));
  CHECK(r.warnings.empty());
  CHECK((r.sources == std::vector<std::string>{"Sample.dat"}));
  CHECK(r.archive.refs.empty());
  CHECK(r.links.size() == 1 && r.links[0].status == LinkStatus::Exact && r.links[0].symbol == symbol);
  CHECK(r.archive.extents.back().data == original.extents[2].data);
  CHECK(same_structure(original, r.archive));
  CHECK(same_structure(original, Archive::parse(r.archive.serialize())));
}

static void modified_source_is_followed_by_path() {
  Archive original = sample();
  Archive modded = original;
  std::swap(modded.extents[1], modded.extents[2]);
  for (auto& p : modded.extents[0].pointers) p.target = p.target == 1 ? 2 : p.target == 2 ? 1 : p.target;
  modded.extents[0].data.insert(modded.extents[0].data.end(), 32, 0);
  modded.extents[1].data[3] = 9;
  auto modded_disc = modded.serialize();

  Archive linked = original;
  link(linked, 2, locate(original, "Sample.dat", 2, {}).symbol());
  Resolution r = resolve(linked, disc_loader(modded_disc));
  CHECK(r.warnings.size() == 1 && has(r.warnings[0], "differs"));
  CHECK(r.links[0].status == LinkStatus::Modified);
  CHECK(r.archive.extents[follow_path(r.archive, "sample_root", {4})].data[3] == 9);
  CHECK(!same_structure(original, r.archive));

  Archive strict = original;
  Locator l = locate(original, "Sample.dat", 2, {});
  l.strict = true;
  link(strict, 2, l.symbol());
  CHECK(has(error_of([&] { resolve(strict, disc_loader(modded_disc)); }), "does not match its recorded hash"));

  Archive by_offset = original;
  link(by_offset, 2, "slp:Sample.dat:0x60:strict=1:sha=00000000");
  CHECK(!error_of([&] { resolve(by_offset, disc_loader(modded_disc)); }).empty());
}

static void fallback_offset_needs_a_matching_hash() {
  Archive original = sample();
  auto disc = original.serialize();
  std::string sha = asset_sha(original, 2, {});
  Archive ok = original;
  link(ok, 2, "slp:Sample.dat:no_such_root/4:at=0x60:sha=" + sha);
  Resolution r = resolve(ok, disc_loader(disc));
  CHECK(r.warnings.size() == 1 && has(r.warnings[0], "found the recorded asset at 0x60"));
  CHECK(r.links[0].status == LinkStatus::Fallback);
  CHECK(same_structure(original, r.archive));

  Archive bad = original;
  link(bad, 2, "slp:Sample.dat:no_such_root/4:at=0x60:sha=00000000");
  CHECK(has(error_of([&] { resolve(bad, disc_loader(disc)); }), "does not resolve in the disc file"));
  Archive none = original;
  link(none, 2, "slp:Sample.dat:no_such_root/4");
  CHECK(has(error_of([&] { resolve(none, disc_loader(disc)); }), "does not resolve in the disc file: no root symbol no_such_root"));
}

static void moved_asset_is_found_by_hash() {
  Archive original = sample();
  Archive linked = original;
  link(linked, 2, locate(original, "Sample.dat", 2, {}).symbol());
  Archive moved = original;
  std::vector<Pointer> kept;
  for (auto& p : moved.extents[0].pointers) if (p.target != 2) kept.push_back(p);
  moved.extents[0].pointers = kept;
  moved.extents[0].pointers.push_back({0xc, 3});
  moved.extents[1].data.insert(moved.extents[1].data.end(), 32, 0);
  Extent e; e.data = {0, 0, 0, 0, 1, 1, 1, 1}; e.pointers = {{0, 2}};
  moved.extents.push_back(e);
  Resolution r = resolve(linked, disc_loader(moved.serialize()));
  CHECK(r.links[0].status == LinkStatus::Fallback && r.links[0].source == "Sample.dat");
  CHECK(r.warnings.size() == 1);
  CHECK(same_structure(original, r.archive));
}

static void regional_copy_stands_in() {
  Archive original = sample();
  Archive linked = original;
  link(linked, 2, locate(original, "Sample.usd", 2, {}).symbol());
  Archive gutted = original;
  std::vector<Pointer> kept;
  for (auto& p : gutted.extents[0].pointers) if (p.target != 2) kept.push_back(p);
  gutted.extents[0].pointers = kept;
  auto usd = gutted.serialize();
  auto dat = original.serialize();

  Resolution r = resolve(linked, files_loader({{"Sample.usd", usd}, {"Sample.dat", dat}}));
  CHECK(r.links[0].status == LinkStatus::Fallback && r.links[0].source == "Sample.dat");
  CHECK((r.sources == std::vector<std::string>{"Sample.usd", "Sample.dat"}));
  CHECK(same_structure(original, r.archive));
  CHECK(resolve(linked, files_loader({{"Sample.dat", dat}})).links[0].source == "Sample.dat");

  Archive regional = original;
  regional.extents[2].data[0] = 8;
  auto jp = regional.serialize();
  r = resolve(linked, files_loader({{"Sample.usd", usd}, {"Sample.dat", jp}}));
  CHECK(r.links[0].status == LinkStatus::Regional);
  CHECK(r.archive.extents.back().data[0] == 8);

  Archive strict = original;
  Locator l = locate(original, "Sample.usd", 2, {});
  l.strict = true;
  link(strict, 2, l.symbol());
  CHECK(has(error_of([&] { resolve(strict, files_loader({{"Sample.usd", usd}, {"Sample.dat", jp}})); }),
            "does not resolve in the disc file"));
  CHECK(has(error_of([&] { resolve(linked, files_loader({{"Sample.usd", usd}})); }), "not in Sample.usd"));
}

static void resolve_rejects_missing_file_and_bad_offset() {
  Archive original = sample();
  auto disc = original.serialize();
  Archive linked = original;
  link(linked, 2, "slp:Sample.dat:0x60");
  LoadFn gone = [](const std::string&, std::vector<uint8_t>&, std::string& e) { e = "gone"; return false; };
  CHECK(error_of([&] { resolve(linked, gone); }) == "could not read disc file Sample.dat: gone");
  Archive bad = original;
  link(bad, 2, "slp:Sample.dat:0x61");
  CHECK(error_of([&] { resolve(bad, disc_loader(disc)); }) == "disc file Sample.dat has no struct starting at file offset 0x61");
}

static void cut_stops_at_siblings() {
  Archive a = sample();
  a.extents[1].pointers.push_back({4, 2});
  std::vector<Pointer> kept;
  for (auto& p : a.extents[0].pointers) if (p.target != 2) kept.push_back(p);
  a.extents[0].pointers = kept;
  auto disc = a.serialize();
  Archive linked = a;
  link(linked, 1, "slp:Sample.dat:sample_root/0:cut=4");
  CHECK(linked.extents.size() == 1);
  Archive resolved = resolve(linked, disc_loader(disc)).archive;
  CHECK(resolved.extents.size() == 2);
  CHECK(resolved.extents[1].pointers.empty());
}

static void parse_rejects_malformed_archives() {
  auto bytes = sample().serialize();
  CHECK(has(error_of([&] { Archive::parse(bytes.data(), 16); }), "archive is truncated: needed 32 bytes, have 16"));
  auto shorter = bytes;
  shorter.pop_back();
  CHECK(has(error_of([&] { Archive::parse(shorter); }), "but it is"));
  auto wild = bytes;   // first pointer (data offset 0) aimed far outside the data section
  wild[0x20] = 0x7F;
  CHECK(has(error_of([&] { Archive::parse(wild); }), "outside the data section"));
}

// slprs_gamefile_resolve: the cheap exits, a failure that serves nothing, and a resolved file.
static void front_door() {
  ReadDiscFile none = [](const std::string&, std::vector<uint8_t>&) { return false; };
  std::vector<uint8_t> tiny = {1, 2, 3};
  CHECK(resolve_links("x.dat", tiny, none).status == Status::NoLinks && tiny.size() == 3);
  auto plain = sample().serialize();
  auto copy = plain;
  CHECK(resolve_links("x.dat", copy, none).status == Status::NoLinks && copy == plain);

  Archive original = sample();
  Archive with_other = original;   // an external symbol that is not a link is left alone
  with_other.extents[0].pointers.pop_back();
  with_other.refs.push_back({"some_symbol", {{0, 8}}});
  auto other = with_other.serialize();
  copy = other;
  CHECK(resolve_links("x.dat", copy, none).status == Status::NoLinks && copy == other);

  auto garbage = plain;
  garbage[0x10 + 3] = 1;   // a reference count, but not a parseable archive
  Outcome o = resolve_links("x.dat", garbage, none);
  CHECK(o.status == Status::NoLinks && !o.error.empty());

  Archive linked = original;
  link(linked, 2, locate(original, "Sample.dat", 2, {}).symbol());
  auto file = linked.serialize();
  copy = file;
  o = resolve_links("x.dat", copy, none);
  CHECK(o.status == Status::Failed && o.error == "could not read disc file Sample.dat: not found on the disc");
  CHECK(copy == file);

  auto disc = original.serialize();
  ReadDiscFile reader = [&](const std::string& name, std::vector<uint8_t>& out) {
    if (name != "Sample.dat") return false;
    out = disc;
    return true;
  };
  o = resolve_links("x.dat", copy, reader);
  CHECK(o.status == Status::Resolved && o.links == 1 && o.warnings.empty());
  CHECK((o.disc_files == std::vector<std::string>{"Sample.dat"}));
  CHECK(same_structure(original, Archive::parse(copy)));
}

int main() {
  serialize_then_parse_is_identity();
  extern_chain_round_trips();
  relayout_does_not_change_hash();
  content_change_changes_hash();
  locator_round_trip();
  locate_prefers_a_path();
  link_then_resolve_restores_structure();
  modified_source_is_followed_by_path();
  fallback_offset_needs_a_matching_hash();
  moved_asset_is_found_by_hash();
  regional_copy_stands_in();
  resolve_rejects_missing_file_and_bad_offset();
  cut_stops_at_siblings();
  parse_rejects_malformed_archives();
  front_door();
  if (g_failures) { std::printf("%d check(s) failed\n", g_failures); return 1; }
  std::printf("slippi_gamefile_links: all tests passed\n");
  return 0;
}
