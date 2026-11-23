// Slippi game-file links: HAL sysdolphin archives (.dat/.usd) whose Nintendo-authored assets are
// replaced by external symbols `slp:<disc file>:<anchor>[:at=..][:cut=..][:sha=..][:strict=1]`.
// resolve_links() copies those assets in from the player's own disc at load time, exactly as
// Slippi 3.7.0 does. C++ port of the `hsd` crate of slippi-rust-extensions (archive.rs, hash.rs,
// link.rs) and of its FFI entry point slprs_gamefile_resolve (ffi/src/hsd.rs); the output is the
// same bytes the Rust produces for every input.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace slippi::gamefile_links {

// Size of the fixed header that precedes the data section.
constexpr uint32_t kHeaderSize = 0x20;
// Value that terminates an external reference chain.
constexpr uint32_t kChainEnd = 0xFFFFFFFFu;
// Archives load 32-byte aligned, so a data offset modulo 32 is a struct's memory alignment.
constexpr uint32_t kAlign = 32;
// Prefix of the external symbols resolved here. Other symbols are left untouched.
constexpr const char* kLinkPrefix = "slp:";

// Every failure of the Rust crate (its `Error` enum); what() is the same text Rust displays.
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A pointer field inside an extent, targeting the start of another extent.
struct Pointer {
  uint32_t field;   // offset of the 4-byte field within the extent
  size_t target;    // index of the target extent
  bool operator==(const Pointer& o) const { return field == o.field && target == o.target; }
};

// A run of bytes between two pointer targets. Pointer and chain fields are zero in `data`.
struct Extent {
  std::vector<uint8_t> data;
  uint32_t phase = 0;    // data offset modulo 32 this extent must be placed at
  std::vector<Pointer> pointers;
  uint32_t origin = 0;   // data offset in the archive it was parsed from (informational)
};

struct Root {
  std::string name;
  size_t extent;
};

// An imported symbol and the pointer fields that receive its address, in chain order.
struct ExternRef {
  std::string name;
  std::vector<std::pair<size_t, uint32_t>> locations;   // (extent, field)
};

struct Archive {
  std::array<uint8_t, 4> version{};
  std::array<uint8_t, 8> header_tail{};   // header bytes 0x18..0x20, verbatim
  std::vector<Extent> extents;
  std::vector<Root> roots;
  std::vector<ExternRef> refs;

  // Lossless: serializing an unmodified archive reproduces the input byte for byte.
  static Archive parse(const uint8_t* bytes, size_t size);   // throws Error
  static Archive parse(const std::vector<uint8_t>& bytes) { return parse(bytes.data(), bytes.size()); }
  std::vector<uint8_t> serialize() const;                    // throws Error
  void invalidate_strings() { strings_.reset(); }
  std::optional<size_t> extent_at_origin(uint32_t data_offset) const;
  std::vector<uint32_t> layout() const;
  // Breadth-first from `starts`; `cut(extent, pointer)` may veto a pointer.
  std::vector<size_t> reachable(const std::vector<size_t>& starts,
                                const std::function<bool(size_t, const Pointer&)>& cut = nullptr) const;
  std::vector<size_t> reachable_from_roots() const;
  void remove_extents(const std::vector<size_t>& remove);    // throws Error

 private:
  struct StringTable {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> root_offsets, ref_offsets;
  };
  std::optional<StringTable> strings_;
  const StringTable* reusable_strings() const;
};

// Structural hashes (hash.rs): layout-independent SHA-1 of an extent and everything it points to.
std::array<uint8_t, 20> extent_hash(const Archive& archive, size_t extent);
std::array<uint8_t, 20> archive_hash(const Archive& archive);
std::string hex(const uint8_t* bytes, size_t n);
// First eight hex digits of the asset's structural hash with `cut` fields of its first struct removed.
std::string asset_sha(const Archive& archive, size_t extent, const std::vector<uint32_t>& cut);

struct Anchor {
  bool is_offset = false;
  uint32_t offset = 0;             // absolute file offset (is_offset)
  std::string root;                // root symbol (path)
  std::vector<uint32_t> hops;      // pointer fields to follow from it (path)
};

struct Locator {
  std::string file;
  Anchor anchor;
  std::optional<uint32_t> at;
  std::vector<uint32_t> cut;
  std::optional<std::string> sha;
  bool strict = false;

  static bool is_link(const std::string& symbol);
  static Locator parse(const std::string& symbol);   // throws Error
  std::string symbol() const;
};

// A locator for `extent` of `archive` as the Rust `locate` builds it (path from the nearest root,
// offset fallback, hash), and `link`, which replaces the extent with a `symbol` reference. Used to
// make linked test archives; the loader only resolves.
Locator locate(const Archive& archive, const std::string& file, size_t extent, const std::vector<uint32_t>& cut);
void link(Archive& archive, size_t extent, const std::string& symbol);

enum class LinkStatus { Exact, Modified, Fallback, Regional, Unchecked };
const char* status_name(LinkStatus status);

struct ResolvedLink {
  std::string symbol;
  LinkStatus status;
  std::string source;   // disc file the asset came from
};

// Reads a disc file by name. Returns false when it does not exist (or is empty).
using ReadDiscFile = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;

struct Resolution {
  Archive archive;
  std::vector<std::string> warnings;
  std::vector<std::string> sources;   // disc files read successfully, in order
  std::vector<ResolvedLink> links;
};

// Loads a disc file for resolve(): fills `out` and returns true, or sets `error` and returns false.
using LoadFn = std::function<bool(const std::string& name, std::vector<uint8_t>& out, std::string& error)>;
// link::resolve: every slp: reference replaced by its asset. Throws Error.
Resolution resolve(const Archive& archive, const LoadFn& load);

// slprs_gamefile_resolve, the entry point the game-file loader calls.
enum class Status {
  NoLinks,    // not an archive, or no slp: links: serve the contents as they are (unchanged)
  Resolved,   // contents now hold the resolved archive
  Failed,     // a link could not be resolved: serve nothing; `error` says why
};
struct Outcome {
  Status status = Status::NoLinks;
  std::string error;                   // Failed: the reason (also why an archive did not parse, for NoLinks)
  std::vector<std::string> warnings;   // links that resolved to something other than the recorded asset
  std::vector<std::string> disc_files; // disc files read
  size_t links = 0;
  std::vector<ResolvedLink> resolved;
};
Outcome resolve_links(const std::string& name, std::vector<uint8_t>& contents, const ReadDiscFile& read);

}  // namespace slippi::gamefile_links
