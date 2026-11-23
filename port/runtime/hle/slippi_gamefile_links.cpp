// SPDX-License-Identifier: GPL-2.0-or-later
// Port of slippi-rust-extensions `hsd` (archive.rs, hash.rs, link.rs) and `ffi/src/hsd.rs`.
// Function and variable names follow the Rust so the two can be read side by side.
#include "slippi_gamefile_links.h"
#include "../host/sha1.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace slippi::gamefile_links {
namespace {

using Hash = std::array<uint8_t, 20>;

std::string fmt_hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%llx", (unsigned long long)v);
  return buf;
}

uint32_t be32(const uint8_t* bytes, size_t at) {
  return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) | (uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
}
void put_be32(uint8_t* at, uint32_t v) {
  at[0] = uint8_t(v >> 24); at[1] = uint8_t(v >> 16); at[2] = uint8_t(v >> 8); at[3] = uint8_t(v);
}
void append_be32(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t b[4]; put_be32(b, v); out.insert(out.end(), b, b + 4);
}

void need(size_t have, size_t needed) {
  if (have < needed)
    throw Error("archive is truncated: needed " + std::to_string(needed) + " bytes, have " + std::to_string(have));
}

// String::from_utf8_lossy: every maximal invalid subsequence becomes U+FFFD.
std::string utf8_lossy(const uint8_t* s, size_t n) {
  std::string out;
  out.reserve(n);
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    if (c < 0x80) { out += char(c); ++i; continue; }
    size_t len = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c == 0xE0) { len = 3; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) len = 3;
    else if (c == 0xED) { len = 3; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) len = 3;
    else if (c == 0xF0) { len = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) len = 4;
    else if (c == 0xF4) { len = 4; hi = 0x8F; }
    size_t valid = 1;   // bytes of a valid prefix consumed
    bool ok = len != 0;
    if (ok) {
      for (size_t k = 1; k < len; ++k) {
        if (i + k >= n) { ok = false; break; }
        uint8_t b = s[i + k];
        uint8_t l = k == 1 ? lo : 0x80, h = k == 1 ? hi : 0xBF;
        if (b < l || b > h) { ok = false; break; }
        ++valid;
      }
    }
    if (ok) { out.append(reinterpret_cast<const char*>(s + i), len); i += len; }
    else { out += "\xEF\xBF\xBD"; i += valid; }
  }
  return out;
}

std::string read_cstr(const std::vector<uint8_t>& table, uint32_t at) {
  size_t start = at;
  need(table.size(), start);
  size_t end = start;
  while (end < table.size() && table[end] != 0) ++end;
  return utf8_lossy(table.data() + start, end - start);
}

// u32::from_str_radix / usize::from_str: an optional leading '+', then at least one digit.
bool parse_unsigned(const std::string& text, unsigned radix, uint64_t max, uint64_t& out) {
  size_t i = 0;
  if (text.empty()) return false;
  if (text[0] == '+') { if (text.size() == 1) return false; i = 1; }
  uint64_t v = 0;
  for (; i < text.size(); ++i) {
    char c = text[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = unsigned(c - '0');
    else if (c >= 'a' && c <= 'z') d = unsigned(c - 'a' + 10);
    else if (c >= 'A' && c <= 'Z') d = unsigned(c - 'A' + 10);
    else return false;
    if (d >= radix) return false;
    if (v > (max - d) / radix) return false;
    v = v * radix + d;
  }
  out = v;
  return true;
}

Hash sha1_of(const std::vector<uint8_t>& bytes) { return host::sha1(bytes.data(), bytes.size()); }

Hash combine(const std::vector<uint8_t>& data, const std::vector<std::pair<uint32_t, Hash>>& children) {
  size_t end = data.size();
  while (end > 0 && data[end - 1] == 0) --end;   // trailing zero padding is ignored
  std::vector<uint8_t> buf(data.begin(), data.begin() + end);
  for (const auto& c : children) {
    append_be32(buf, c.first);
    buf.insert(buf.end(), c.second.begin(), c.second.end());
  }
  return sha1_of(buf);
}

// hash::Hasher, optionally seeing the archive with some pointer fields of one extent removed (the
// Rust clones the archive and drops those pointers; the result is the same).
class Hasher {
 public:
  explicit Hasher(const Archive& a, size_t cut_extent = SIZE_MAX, const std::vector<uint32_t>* cut = nullptr)
      : a_(a), memo_(a.extents.size()), in_progress_(a.extents.size(), false), cut_extent_(cut_extent), cut_(cut) {}

  Hash extent(size_t idx) {
    if (memo_[idx]) return *memo_[idx];
    if (in_progress_[idx]) {
      static const uint8_t cycle[] = {'c', 'y', 'c', 'l', 'e'};
      return host::sha1(cycle, sizeof cycle);
    }
    in_progress_[idx] = true;
    const Extent& e = a_.extents[idx];
    std::vector<Pointer> pointers;
    for (const auto& p : e.pointers)
      if (!(idx == cut_extent_ && cut_ && std::find(cut_->begin(), cut_->end(), p.field) != cut_->end()))
        pointers.push_back(p);
    std::stable_sort(pointers.begin(), pointers.end(), [](const Pointer& x, const Pointer& y) { return x.field < y.field; });
    std::vector<std::pair<uint32_t, Hash>> children;
    children.reserve(pointers.size());
    for (const auto& p : pointers) children.emplace_back(p.field, extent(p.target));
    Hash h = combine(e.data, children);
    in_progress_[idx] = false;
    memo_[idx] = h;
    return h;
  }

  Hash archive() {
    std::vector<uint8_t> buf;
    for (const auto& root : a_.roots) {
      buf.insert(buf.end(), root.name.begin(), root.name.end());
      buf.push_back(0);
      Hash h = extent(root.extent);
      buf.insert(buf.end(), h.begin(), h.end());
    }
    for (const auto& r : a_.refs) {
      buf.insert(buf.end(), r.name.begin(), r.name.end());
      buf.push_back(0);
      for (const auto& loc : r.locations) {
        Hash h = extent(loc.first);
        buf.insert(buf.end(), h.begin(), h.end());
        append_be32(buf, loc.second);
      }
    }
    return sha1_of(buf);
  }

 private:
  const Archive& a_;
  std::vector<std::optional<Hash>> memo_;
  std::vector<bool> in_progress_;
  size_t cut_extent_;
  const std::vector<uint32_t>* cut_;
};

bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

}  // namespace

// ---- archive.rs ----

Archive Archive::parse(const uint8_t* bytes, size_t size) {
  need(size, kHeaderSize);
  const uint32_t file_size = be32(bytes, 0);
  const uint32_t data_size = be32(bytes, 4);
  const size_t reloc_count = be32(bytes, 8);
  const size_t root_count = be32(bytes, 12);
  const size_t ref_count = be32(bytes, 16);
  Archive out;
  std::copy(bytes + 0x14, bytes + 0x18, out.version.begin());
  std::copy(bytes + 0x18, bytes + 0x20, out.header_tail.begin());

  if (file_size != size)
    throw Error("header says the file is " + std::to_string(file_size) + " bytes but it is " + std::to_string(size));

  const size_t data_start = kHeaderSize;
  const size_t data_end = data_start + data_size;
  const size_t reloc_off = data_end;
  const size_t roots_off = reloc_off + reloc_count * 4;
  const size_t refs_off = roots_off + root_count * 8;
  const size_t strings_off = refs_off + ref_count * 8;
  need(size, strings_off);

  const uint8_t* data = bytes + data_start;
  const size_t data_len = data_size;
  std::vector<uint8_t> strings(bytes + strings_off, bytes + size);

  // Relocation table: every pointer field, and the extent starts they imply.
  std::vector<std::pair<uint32_t, uint32_t>> fields;
  fields.reserve(reloc_count);
  std::set<uint32_t> boundaries{0, data_size};
  for (size_t i = 0; i < reloc_count; ++i) {
    uint32_t field = be32(bytes, reloc_off + i * 4);
    if (size_t(field) + 4 > data_len)
      throw Error("pointer at data offset 0x" + fmt_hex(field) + " targets 0x0, outside the data section");
    uint32_t target = be32(data, field);
    if (target > data_size)
      throw Error("pointer at data offset 0x" + fmt_hex(field) + " targets 0x" + fmt_hex(target) + ", outside the data section");
    boundaries.insert(target);
    fields.emplace_back(field, target);
  }

  StringTable table;
  std::vector<std::string> root_names;
  std::vector<uint32_t> root_targets;
  for (size_t i = 0; i < root_count; ++i) {
    uint32_t target = be32(bytes, roots_off + i * 8);
    uint32_t str_off = be32(bytes, roots_off + i * 8 + 4);
    std::string name = read_cstr(strings, str_off);
    if (target > data_size)
      throw Error("root " + name + " targets 0x" + fmt_hex(target) + ", outside the data section");
    boundaries.insert(target);
    table.root_offsets.push_back(str_off);
    root_names.push_back(std::move(name));
    root_targets.push_back(target);
  }

  std::vector<std::string> ref_names;
  std::vector<std::vector<uint32_t>> ref_chains;
  for (size_t i = 0; i < ref_count; ++i) {
    uint32_t head = be32(bytes, refs_off + i * 8);
    uint32_t str_off = be32(bytes, refs_off + i * 8 + 4);
    std::string name = read_cstr(strings, str_off);
    std::vector<uint32_t> chain;
    uint32_t at = head;
    for (;;) {
      if (size_t(at) + 4 > data_len || chain.size() > data_len / 4)
        throw Error("external reference " + name + " has a malformed chain at data offset 0x" + fmt_hex(at));
      chain.push_back(at);
      uint32_t next = be32(data, at);
      if (next == kChainEnd) break;
      at = next;
    }
    table.ref_offsets.push_back(str_off);
    ref_names.push_back(std::move(name));
    ref_chains.push_back(std::move(chain));
  }

  // Cut the data section into extents. A pointer or root may target the very end of the data
  // section (an empty struct): keep it as a zero-length extent so the pointer survives.
  bool end_targeted = std::any_of(fields.begin(), fields.end(), [&](const auto& f) { return f.second == data_size; }) ||
                      std::any_of(root_targets.begin(), root_targets.end(), [&](uint32_t t) { return t == data_size; });
  std::vector<uint32_t> starts;
  for (uint32_t b : boundaries)
    if (b < data_size || (end_targeted && b == data_size)) starts.push_back(b);
  std::map<uint32_t, size_t> index_of;
  out.extents.reserve(starts.size());
  for (size_t i = 0; i < starts.size(); ++i) {
    uint32_t start = starts[i];
    uint32_t end = i + 1 < starts.size() ? starts[i + 1] : data_size;
    index_of[start] = i;
    Extent e;
    e.data.assign(data + start, data + end);
    e.phase = start % kAlign;
    e.origin = start;
    out.extents.push_back(std::move(e));
  }

  auto locate_field = [&](uint32_t field) -> std::pair<size_t, uint32_t> {
    auto it = index_of.upper_bound(field);
    if (it == index_of.begin())   // unreachable: a field is only located inside a non-empty data section
      throw Error("field at data offset 0x" + fmt_hex(field) + " straddles a struct boundary");
    --it;
    uint32_t within = field - it->first;
    if (size_t(within) + 4 > out.extents[it->second].data.size())
      throw Error("field at data offset 0x" + fmt_hex(field) + " straddles a struct boundary");
    return {it->second, within};
  };

  for (const auto& [field, target] : fields) {
    auto [idx, within] = locate_field(field);
    auto t = index_of.find(target);
    if (t == index_of.end())
      throw Error("pointer at data offset 0x" + fmt_hex(field) + " targets 0x" + fmt_hex(target) + ", outside the data section");
    std::fill_n(out.extents[idx].data.begin() + within, 4, 0);
    out.extents[idx].pointers.push_back({within, t->second});
  }

  for (size_t i = 0; i < root_names.size(); ++i) out.roots.push_back({root_names[i], index_of.at(root_targets[i])});

  for (size_t i = 0; i < ref_names.size(); ++i) {
    ExternRef r{ref_names[i], {}};
    for (uint32_t at : ref_chains[i]) {
      auto [idx, within] = locate_field(at);
      std::fill_n(out.extents[idx].data.begin() + within, 4, 0);
      r.locations.emplace_back(idx, within);
    }
    out.refs.push_back(std::move(r));
  }

  table.bytes = std::move(strings);
  out.strings_ = std::move(table);
  return out;
}

std::vector<uint32_t> Archive::layout() const {
  std::vector<uint32_t> offsets;
  offsets.reserve(extents.size());
  uint32_t cursor = 0;
  for (const auto& e : extents) {
    cursor += (e.phase + kAlign - cursor % kAlign) % kAlign;
    offsets.push_back(cursor);
    cursor += uint32_t(e.data.size());
  }
  return offsets;
}

const Archive::StringTable* Archive::reusable_strings() const {
  if (!strings_) return nullptr;
  const StringTable& t = *strings_;
  if (t.root_offsets.size() != roots.size() || t.ref_offsets.size() != refs.size()) return nullptr;
  auto same = [&](uint32_t off, const std::string& name) {
    try { return read_cstr(t.bytes, off) == name; } catch (const Error&) { return false; }
  };
  for (size_t i = 0; i < roots.size(); ++i) if (!same(t.root_offsets[i], roots[i].name)) return nullptr;
  for (size_t i = 0; i < refs.size(); ++i) if (!same(t.ref_offsets[i], refs[i].name)) return nullptr;
  return &t;
}

std::vector<uint8_t> Archive::serialize() const {
  const std::vector<uint32_t> offsets = layout();
  const uint32_t data_size = extents.empty() ? 0 : offsets.back() + uint32_t(extents.back().data.size());

  std::vector<uint8_t> data(data_size, 0);
  std::vector<uint32_t> relocs;
  for (size_t i = 0; i < extents.size(); ++i) {
    const Extent& e = extents[i];
    size_t base = offsets[i];
    std::copy(e.data.begin(), e.data.end(), data.begin() + base);
    for (const auto& p : e.pointers) {
      size_t at = base + p.field;
      put_be32(&data[at], offsets[p.target]);
      relocs.push_back(uint32_t(at));
    }
  }
  std::sort(relocs.begin(), relocs.end());

  std::vector<uint32_t> ref_heads;
  for (const auto& r : refs) {
    if (r.locations.empty()) throw Error("external reference " + r.name + " has no locations");
    auto place = [&](const std::pair<size_t, uint32_t>& loc) { return offsets[loc.first] + loc.second; };
    for (size_t n = 0; n < r.locations.size(); ++n) {
      uint32_t value = n + 1 < r.locations.size() ? place(r.locations[n + 1]) : kChainEnd;
      put_be32(&data[place(r.locations[n])], value);
    }
    ref_heads.push_back(place(r.locations[0]));
  }

  std::vector<uint8_t> strings;
  std::vector<uint32_t> root_str, ref_str;
  if (const StringTable* t = reusable_strings()) {
    strings = t->bytes; root_str = t->root_offsets; ref_str = t->ref_offsets;
  } else {
    for (const auto& r : roots) {
      root_str.push_back(uint32_t(strings.size()));
      strings.insert(strings.end(), r.name.begin(), r.name.end());
      strings.push_back(0);
    }
    for (const auto& r : refs) {
      ref_str.push_back(uint32_t(strings.size()));
      strings.insert(strings.end(), r.name.begin(), r.name.end());
      strings.push_back(0);
    }
  }

  const size_t file_size = kHeaderSize + data.size() + relocs.size() * 4 + (roots.size() + refs.size()) * 8 + strings.size();
  std::vector<uint8_t> out;
  out.reserve(file_size);
  append_be32(out, uint32_t(file_size));
  append_be32(out, data_size);
  append_be32(out, uint32_t(relocs.size()));
  append_be32(out, uint32_t(roots.size()));
  append_be32(out, uint32_t(refs.size()));
  out.insert(out.end(), version.begin(), version.end());
  out.insert(out.end(), header_tail.begin(), header_tail.end());
  out.insert(out.end(), data.begin(), data.end());
  for (uint32_t r : relocs) append_be32(out, r);
  for (size_t i = 0; i < roots.size(); ++i) { append_be32(out, offsets[roots[i].extent]); append_be32(out, root_str[i]); }
  for (size_t i = 0; i < refs.size(); ++i) { append_be32(out, ref_heads[i]); append_be32(out, ref_str[i]); }
  out.insert(out.end(), strings.begin(), strings.end());
  return out;
}

std::optional<size_t> Archive::extent_at_origin(uint32_t data_offset) const {
  for (size_t i = 0; i < extents.size(); ++i)
    if (extents[i].origin == data_offset) return i;
  return std::nullopt;
}

std::vector<size_t> Archive::reachable(const std::vector<size_t>& starts,
                                       const std::function<bool(size_t, const Pointer&)>& cut) const {
  std::vector<bool> seen(extents.size(), false);
  std::vector<size_t> order;
  std::deque<size_t> queue;
  for (size_t s : starts)
    if (!seen[s]) { seen[s] = true; queue.push_back(s); }
  while (!queue.empty()) {
    size_t idx = queue.front();
    queue.pop_front();
    order.push_back(idx);
    for (const auto& p : extents[idx].pointers) {
      if ((cut && cut(idx, p)) || seen[p.target]) continue;
      seen[p.target] = true;
      queue.push_back(p.target);
    }
  }
  return order;
}

std::vector<size_t> Archive::reachable_from_roots() const {
  std::vector<size_t> starts;
  for (const auto& r : roots) starts.push_back(r.extent);
  return reachable(starts);
}

void Archive::remove_extents(const std::vector<size_t>& remove) {
  std::vector<bool> gone(extents.size(), false);
  for (size_t i : remove) {
    if (i >= extents.size()) throw Error("extent index " + std::to_string(i) + " is out of range");
    gone[i] = true;
  }
  std::vector<size_t> new_index(extents.size(), SIZE_MAX);
  size_t next = 0;
  for (size_t i = 0; i < gone.size(); ++i)
    if (!gone[i]) new_index[i] = next++;
  for (const auto& r : roots)
    if (gone[r.extent]) throw Error("extent " + std::to_string(r.extent) + " is a root and cannot be linked");
  for (const auto& r : refs)
    for (const auto& loc : r.locations)
      if (gone[loc.first]) throw Error("external reference " + r.name + " has a location inside the subtree being linked");
  for (size_t i = 0; i < extents.size(); ++i) {
    if (gone[i]) continue;
    for (const auto& p : extents[i].pointers)
      if (gone[p.target])
        throw Error("pointer at data offset 0x" + fmt_hex(p.field) + " targets 0x" + fmt_hex(extents[p.target].origin) +
                    ", outside the data section");
  }
  std::vector<Extent> kept;
  kept.reserve(next);
  for (size_t i = 0; i < extents.size(); ++i) {
    if (gone[i]) continue;
    Extent e = std::move(extents[i]);
    for (auto& p : e.pointers) p.target = new_index[p.target];
    kept.push_back(std::move(e));
  }
  extents = std::move(kept);
  for (auto& r : roots) r.extent = new_index[r.extent];
  for (auto& r : refs)
    for (auto& loc : r.locations) loc.first = new_index[loc.first];
}

// ---- hash.rs ----

std::string hex(const uint8_t* bytes, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += digits[bytes[i] >> 4]; s += digits[bytes[i] & 15]; }
  return s;
}

std::array<uint8_t, 20> extent_hash(const Archive& archive, size_t extent) { return Hasher(archive).extent(extent); }
std::array<uint8_t, 20> archive_hash(const Archive& archive) { return Hasher(archive).archive(); }

std::string asset_sha(const Archive& archive, size_t extent, const std::vector<uint32_t>& cut) {
  Hash h = cut.empty() ? Hasher(archive).extent(extent) : Hasher(archive, extent, &cut).extent(extent);
  return hex(h.data(), h.size()).substr(0, 8);
}

// ---- link.rs ----

namespace {

uint32_t parse_hex(const std::string& symbol, const std::string& text) {
  std::string digits = text;
  if (text.compare(0, 2, "0x") == 0 || text.compare(0, 2, "0X") == 0) digits = text.substr(2);
  uint64_t v;
  if (!parse_unsigned(digits, 16, 0xFFFFFFFFull, v))
    throw Error("malformed link symbol " + symbol + ": bad number " + text);
  return uint32_t(v);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t at = s.find(sep, start);
    if (at == std::string::npos) { parts.push_back(s.substr(start)); return parts; }
    parts.push_back(s.substr(start, at - start));
    start = at + 1;
  }
}

std::string anchor_text(const Anchor& a) {
  if (a.is_offset) return "0x" + fmt_hex(a.offset);
  std::string text = a.root + "/";
  size_t i = 0;
  while (i < a.hops.size()) {
    size_t n = 1;
    while (i + n < a.hops.size() && a.hops[i + n] == a.hops[i]) ++n;
    if (i > 0) text += '/';
    text += fmt_hex(a.hops[i]);
    if (n > 1) text += "*" + std::to_string(n);
    i += n;
  }
  return text;
}

struct Unresolved {   // std::result::Result<usize, String> from follow / at_anchor
  std::optional<size_t> extent;
  std::string reason;
};

Unresolved follow(const Archive& a, const std::string& root, const std::vector<uint32_t>& hops) {
  auto r = std::find_if(a.roots.begin(), a.roots.end(), [&](const Root& x) { return x.name == root; });
  if (r == a.roots.end()) return {std::nullopt, "no root symbol " + root};
  size_t cur = r->extent;
  for (size_t n = 0; n < hops.size(); ++n) {
    const auto& ps = a.extents[cur].pointers;
    auto p = std::find_if(ps.begin(), ps.end(), [&](const Pointer& x) { return x.field == hops[n]; });
    if (p == ps.end()) return {std::nullopt, "hop " + std::to_string(n) + " (field 0x" + fmt_hex(hops[n]) + ") is not a pointer"};
    cur = p->target;
  }
  return {cur, {}};
}

Unresolved at_anchor(const Archive& a, const Anchor& anchor) {
  if (anchor.is_offset) {
    auto e = a.extent_at_origin(anchor.offset - kHeaderSize);   // wrapping_sub
    if (!e) return {std::nullopt, "no struct starts at file offset 0x" + fmt_hex(anchor.offset)};
    return {e, {}};
  }
  return follow(a, anchor.root, anchor.hops);
}

std::optional<std::string> regional_twin(const std::string& file) {
  size_t dot = file.rfind('.');
  if (dot == std::string::npos) return std::nullopt;
  std::string stem = file.substr(0, dot), ext = file.substr(dot + 1);
  if (ext == "usd") return stem + ".dat";
  if (ext == "dat") return stem + ".usd";
  return std::nullopt;
}

Hash cut_hash(const Archive& a, const std::vector<Hash>& hashes, size_t extent, const std::vector<uint32_t>& cut) {
  if (cut.empty()) return hashes[extent];
  const Extent& e = a.extents[extent];
  std::vector<std::pair<uint32_t, Hash>> children;
  for (const auto& p : e.pointers)
    if (!contains(cut, p.field)) children.emplace_back(p.field, hashes[p.target]);
  std::stable_sort(children.begin(), children.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  return combine(e.data, children);
}

struct Source {
  Archive archive;
  std::optional<std::vector<Hash>> hashes;

  std::optional<size_t> find_by_hash(const Locator& locator) {
    if (!locator.sha) return std::nullopt;
    const std::string& sha = *locator.sha;
    if (locator.at) {
      auto extent = archive.extent_at_origin(*locator.at - kHeaderSize);
      if (extent && asset_sha(archive, *extent, locator.cut) == sha) return extent;
    }
    if (!hashes) {
      Hasher hasher(archive);
      std::vector<Hash> all;
      all.reserve(archive.extents.size());
      for (size_t i = 0; i < archive.extents.size(); ++i) all.push_back(hasher.extent(i));
      hashes = std::move(all);
    }
    std::vector<uint8_t> prefix;
    for (size_t i = 0; i < sha.size() / 2; ++i) {
      uint64_t v = 0;
      if (!parse_unsigned(sha.substr(2 * i, 2), 16, 0xFF, v)) v = 0;
      prefix.push_back(uint8_t(v));
    }
    // Hashes memoized across the whole file can differ from a fresh one for an asset that loops
    // back into itself, so they only pick candidates.
    for (size_t i = 0; i < archive.extents.size(); ++i) {
      Hash h = cut_hash(archive, *hashes, i, locator.cut);
      if (!std::equal(prefix.begin(), prefix.end(), h.begin())) continue;
      if (asset_sha(archive, i, locator.cut) == sha) return i;
    }
    return std::nullopt;
  }
};

struct Entry {
  std::unique_ptr<Source> source;   // null when the file could not be read
  std::string error;
};

struct Sources {
  const LoadFn& load;
  std::map<std::string, Entry> files;
  std::vector<std::string> read;

  // Returns the source, or null with `reason` set.
  Source* get(const std::string& file, std::string* reason) {
    auto it = files.find(file);
    if (it == files.end()) {
      Entry entry;
      std::vector<uint8_t> bytes;
      std::string error;
      if (!load(file, bytes, error)) {
        entry.error = error;
      } else {
        try {
          entry.source = std::make_unique<Source>(Source{Archive::parse(bytes), std::nullopt});
        } catch (const Error& e) {
          entry.error = e.what();
        }
      }
      if (entry.source) read.push_back(file);
      it = files.emplace(file, std::move(entry)).first;
    }
    if (!it->second.source && reason) *reason = it->second.error;
    return it->second.source.get();
  }
};

struct Found {
  std::string file;
  size_t extent;
  LinkStatus status;
};

Found find_asset(Sources& sources, const Locator& locator, const std::string& symbol, std::vector<std::string>& warnings) {
  const std::string& file = locator.file;
  std::vector<std::string> searched;
  std::string problem;
  std::optional<std::string> unreadable;
  std::string read_error;
  if (Source* src = sources.get(file, &read_error); !src) {
    problem = file + " could not be read (" + read_error + ")";
    unreadable = read_error;
  } else {
    Unresolved anchored = at_anchor(src->archive, locator.anchor);
    if (anchored.extent) {
      size_t extent = *anchored.extent;
      LinkStatus status;
      if (!locator.sha) {
        status = LinkStatus::Unchecked;
      } else {
        std::string actual = asset_sha(src->archive, extent, locator.cut);
        if (actual == *locator.sha) {
          status = LinkStatus::Exact;
        } else if (locator.strict) {
          throw Error("linked asset " + symbol + " does not match its recorded hash (expected " + *locator.sha +
                      ", found " + actual + ")");
        } else {
          warnings.push_back(symbol + ": the asset on the disc differs from the recorded one (" + *locator.sha + " vs " +
                             actual + "); using the disc's version");
          status = LinkStatus::Modified;
        }
      }
      return {file, extent, status};
    }
    const std::string& reason = anchored.reason;
    if (auto extent = src->find_by_hash(locator)) {
      uint32_t offset = src->archive.extents[*extent].origin + kHeaderSize;
      warnings.push_back(symbol + ": " + reason + "; found the recorded asset at 0x" + fmt_hex(offset) + " in " + file);
      return {file, *extent, LinkStatus::Fallback};
    }
    if (locator.sha) {
      searched.push_back(file);
    } else if (locator.at) {
      if (auto extent = src->archive.extent_at_origin(*locator.at - kHeaderSize)) {
        // Without a hash the fallback offset cannot be checked, only trusted.
        warnings.push_back(symbol + ": " + reason + "; used the fallback offset");
        return {file, *extent, LinkStatus::Fallback};
      }
    }
    problem = reason;
  }

  if (auto twin = regional_twin(file)) {
    if (Source* src = sources.get(*twin, nullptr)) {
      // An offset means nothing in another file, but a path names the same asset.
      std::optional<size_t> slot;
      if (!locator.anchor.is_offset) slot = at_anchor(src->archive, locator.anchor).extent;
      bool slot_is_recorded = slot && locator.sha && asset_sha(src->archive, *slot, locator.cut) == *locator.sha;
      std::optional<size_t> recorded = slot_is_recorded ? slot : src->find_by_hash(locator);
      if (recorded) {
        warnings.push_back(symbol + ": " + problem + "; found the recorded asset in " + *twin);
        return {*twin, *recorded, LinkStatus::Fallback};
      }
      if (slot && !locator.strict) {
        warnings.push_back(symbol + ": " + problem + "; used the same asset of " + *twin +
                           ", which differs from the recorded one");
        return {*twin, *slot, LinkStatus::Regional};
      }
      if (locator.sha) searched.push_back(*twin);
    }
  }

  if (unreadable) throw Error("could not read disc file " + file + ": " + *unreadable);
  if (locator.anchor.is_offset)
    throw Error("disc file " + file + " has no struct starting at file offset 0x" + fmt_hex(locator.anchor.offset));
  std::string why = problem;
  if (!searched.empty()) {
    why += "; the recorded asset is not in ";
    for (size_t i = 0; i < searched.size(); ++i) why += (i ? " or " : "") + searched[i];
  }
  throw Error("link " + symbol + " does not resolve in the disc file: " + why);
}

}  // namespace

bool Locator::is_link(const std::string& symbol) { return symbol.compare(0, 4, kLinkPrefix) == 0; }

Locator Locator::parse(const std::string& symbol) {
  auto bad = [&](const std::string& reason) { return Error("malformed link symbol " + symbol + ": " + reason); };
  if (!is_link(symbol)) throw bad("missing slp: prefix");
  std::vector<std::string> parts = split(symbol.substr(4), ':');
  if (parts[0].empty()) throw bad("missing file name");
  Locator locator;
  locator.file = parts[0];
  if (parts.size() < 2) throw bad("missing anchor");
  const std::string& text = parts[1];
  size_t slash = text.find('/');
  if (slash != std::string::npos) {
    std::string root = text.substr(0, slash);
    if (root.empty()) throw bad("missing root symbol");
    locator.anchor.root = root;
    for (const std::string& hop : split(text.substr(slash + 1), '/')) {
      if (hop.empty()) continue;
      std::string field = hop;
      uint64_t count = 1;
      size_t star = hop.find('*');
      if (star != std::string::npos) {
        field = hop.substr(0, star);
        std::string n = hop.substr(star + 1);
        // Rust accepts any usize here and would abort allocating an absurd count; refuse instead.
        if (!parse_unsigned(n, 10, UINT64_MAX, count) || count > (1u << 24)) throw bad("bad repeat count " + n);
      }
      uint32_t f = parse_hex(symbol, field);
      locator.anchor.hops.insert(locator.anchor.hops.end(), size_t(count), f);
    }
  } else {
    uint32_t offset = parse_hex(symbol, text);
    if (offset < kHeaderSize) throw bad("offset is inside the header");
    locator.anchor.is_offset = true;
    locator.anchor.offset = offset;
  }
  for (size_t i = 2; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    size_t eq = part.find('=');
    std::string key = eq == std::string::npos ? std::string() : part.substr(0, eq);
    std::string value = eq == std::string::npos ? std::string() : part.substr(eq + 1);
    if (eq != std::string::npos && key == "at") {
      uint32_t offset = parse_hex(symbol, value);
      if (offset < kHeaderSize) throw bad("at offset is inside the header");
      locator.at = offset;
    } else if (eq != std::string::npos && key == "cut") {
      for (const std::string& item : split(value, ',')) locator.cut.push_back(parse_hex(symbol, item));
    } else if (eq != std::string::npos && key == "sha" && value.size() == 8 &&
               std::all_of(value.begin(), value.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; })) {
      std::string lower = value;
      for (char& c : lower) c = char(std::tolower((unsigned char)c));
      locator.sha = lower;
    } else if (eq != std::string::npos && key == "strict" && value == "1") {
      locator.strict = true;
    } else {
      throw bad("unknown option " + part);
    }
  }
  return locator;
}

std::string Locator::symbol() const {
  std::string s = std::string(kLinkPrefix) + file + ":" + anchor_text(anchor);
  if (at) s += ":at=0x" + fmt_hex(*at);
  if (!cut.empty()) {
    s += ":cut=";
    for (size_t i = 0; i < cut.size(); ++i) s += (i ? "," : "") + fmt_hex(cut[i]);
  }
  if (sha) s += ":sha=" + *sha;
  if (strict) s += ":strict=1";
  return s;
}

const char* status_name(LinkStatus status) {
  switch (status) {
    case LinkStatus::Exact: return "exact";
    case LinkStatus::Modified: return "modified";
    case LinkStatus::Fallback: return "fallback";
    case LinkStatus::Regional: return "regional";
    case LinkStatus::Unchecked: return "unchecked";
  }
  return "?";
}

Locator locate(const Archive& archive, const std::string& file, size_t extent, const std::vector<uint32_t>& cut) {
  const uint32_t offset = archive.extents[extent].origin + kHeaderSize;
  const size_t n = archive.extents.size();
  std::vector<std::optional<std::pair<size_t, uint32_t>>> parent(n);
  std::vector<std::optional<size_t>> root_of(n);
  std::deque<size_t> queue;
  for (size_t ri = 0; ri < archive.roots.size(); ++ri) {
    size_t e = archive.roots[ri].extent;
    if (!root_of[e]) { root_of[e] = ri; queue.push_back(e); }
  }
  while (!queue.empty()) {
    size_t idx = queue.front();
    queue.pop_front();
    if (idx == extent) break;
    std::vector<Pointer> pointers = archive.extents[idx].pointers;
    std::stable_sort(pointers.begin(), pointers.end(), [](const Pointer& x, const Pointer& y) { return x.field < y.field; });
    for (const auto& p : pointers) {
      if (!root_of[p.target]) {
        root_of[p.target] = root_of[idx];
        parent[p.target] = std::make_pair(idx, p.field);
        queue.push_back(p.target);
      }
    }
  }
  Locator l;
  l.file = file;
  if (root_of[extent]) {
    size_t cur = extent;
    while (parent[cur]) { l.anchor.hops.push_back(parent[cur]->second); cur = parent[cur]->first; }
    std::reverse(l.anchor.hops.begin(), l.anchor.hops.end());
    l.anchor.root = archive.roots[*root_of[extent]].name;
  } else {
    l.anchor.is_offset = true;
    l.anchor.offset = offset;
  }
  l.at = offset;
  l.cut = cut;
  l.sha = asset_sha(archive, extent, cut);
  return l;
}

void link(Archive& archive, size_t extent, const std::string& symbol) {
  if (extent >= archive.extents.size()) throw Error("extent index " + std::to_string(extent) + " is out of range");
  for (const auto& r : archive.roots)
    if (r.extent == extent) throw Error("extent " + std::to_string(extent) + " is a root and cannot be linked");
  std::vector<size_t> before = archive.reachable_from_roots();
  std::vector<std::pair<size_t, uint32_t>> locations;
  for (size_t i = 0; i < archive.extents.size(); ++i) {
    auto& ps = archive.extents[i].pointers;
    std::vector<Pointer> kept;
    for (const auto& p : ps) {
      if (p.target == extent) locations.emplace_back(i, p.field);
      else kept.push_back(p);
    }
    ps = std::move(kept);
  }
  if (locations.empty()) throw Error("extent " + std::to_string(extent) + " is not referenced by any pointer");
  auto existing = std::find_if(archive.refs.begin(), archive.refs.end(), [&](const ExternRef& r) { return r.name == symbol; });
  if (existing != archive.refs.end()) existing->locations.insert(existing->locations.end(), locations.begin(), locations.end());
  else archive.refs.push_back({symbol, std::move(locations)});
  std::vector<bool> still(archive.extents.size(), false);
  for (size_t i : archive.reachable_from_roots()) still[i] = true;
  std::vector<size_t> removed;
  for (size_t i : before)
    if (!still[i]) removed.push_back(i);
  archive.remove_extents(removed);
  archive.invalidate_strings();
}

Resolution resolve(const Archive& archive, const LoadFn& load) {
  Resolution resolution;
  Archive& out = resolution.archive;
  out = archive;
  out.refs.clear();
  Sources sources{load, {}, {}};
  bool resolved_any = false;

  for (const auto& r : archive.refs) {
    if (!Locator::is_link(r.name)) { out.refs.push_back(r); continue; }
    Locator locator = Locator::parse(r.name);
    Found found = find_asset(sources, locator, r.name, resolution.warnings);
    const Archive& src = sources.get(found.file, nullptr)->archive;
    const size_t root = found.extent;

    std::vector<size_t> ordered =
        src.reachable({root}, [&](size_t e, const Pointer& p) { return e == root && contains(locator.cut, p.field); });
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](size_t x, size_t y) { return src.extents[x].origin < src.extents[y].origin; });
    std::map<size_t, size_t> new_index;
    for (size_t i : ordered) {
      new_index[i] = out.extents.size();
      Extent e;
      e.data = src.extents[i].data;
      e.phase = src.extents[i].phase;
      e.origin = src.extents[i].origin;
      out.extents.push_back(std::move(e));
    }
    for (size_t i : ordered) {
      std::vector<Pointer> pointers;
      for (const auto& p : src.extents[i].pointers) {
        if (i == root && contains(locator.cut, p.field)) continue;
        auto t = new_index.find(p.target);
        if (t != new_index.end()) pointers.push_back({p.field, t->second});
      }
      out.extents[new_index[i]].pointers = std::move(pointers);
    }
    const size_t target = new_index[root];
    for (const auto& loc : r.locations) out.extents[loc.first].pointers.push_back({loc.second, target});
    resolution.links.push_back({r.name, found.status, found.file});
    resolved_any = true;
  }

  if (resolved_any) out.invalidate_strings();
  resolution.sources = sources.read;
  return resolution;
}

// ---- ffi/src/hsd.rs: slprs_gamefile_resolve ----

Outcome resolve_links(const std::string& name, std::vector<uint8_t>& contents, const ReadDiscFile& read) {
  (void)name;   // the Rust uses it only for logging; the caller logs
  Outcome outcome;
  if (contents.size() < kHeaderSize) return outcome;
  // Cheap exit for archives without an external reference table.
  if (be32(contents.data(), 0x10) == 0) return outcome;

  Archive archive;
  try {
    archive = Archive::parse(contents);
  } catch (const Error& e) {
    outcome.error = std::string("not a parseable archive, serving it as is: ") + e.what();
    return outcome;
  }
  for (const auto& r : archive.refs)
    if (Locator::is_link(r.name)) ++outcome.links;
  if (outcome.links == 0) return outcome;

  LoadFn load = [&](const std::string& disc_file, std::vector<uint8_t>& bytes, std::string& error) {
    if (!read) { error = "no disc reader was provided"; return false; }
    bytes.clear();
    if (!read(disc_file, bytes)) { error = "not found on the disc"; return false; }
    outcome.disc_files.push_back(disc_file);
    return true;
  };

  Resolution resolution;
  try {
    resolution = resolve(archive, load);
  } catch (const Error& e) {
    outcome.status = Status::Failed;
    outcome.error = e.what();
    return outcome;
  }
  outcome.warnings = std::move(resolution.warnings);
  outcome.resolved = std::move(resolution.links);
  try {
    contents = resolution.archive.serialize();
  } catch (const Error& e) {
    outcome.status = Status::Failed;
    outcome.error = std::string("could not serialize the resolved file: ") + e.what();
    return outcome;
  }
  outcome.status = Status::Resolved;
  return outcome;
}

}  // namespace slippi::gamefile_links
