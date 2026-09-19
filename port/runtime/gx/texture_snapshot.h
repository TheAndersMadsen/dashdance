// Immutable source bytes for deferred GX draws. No guest pointers escape capture.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_texture.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace gx {
struct TextureSnapshot {
  std::vector<uint8_t> image, palette;
  uint64_t hash;
};

class TextureSnapshotCache {
  struct Entry { std::shared_ptr<const TextureSnapshot> snapshot; uint64_t used = 0; };
  std::unordered_multimap<uint64_t, Entry> entries;
  std::unordered_map<const uint8_t*, Entry> last_source;
  uint64_t generation = 0;
  static constexpr uint64_t KEEP_FRAMES = 3;   // a texture unused for this long is dropped
  static bool equal(const TextureSnapshot& s, const uint8_t* image, size_t image_size,
                    const uint8_t* palette, size_t palette_size) {
    return s.image.size() == image_size && s.palette.size() == palette_size &&
        !std::memcmp(s.image.data(), image, image_size) &&
        (!palette_size || !std::memcmp(s.palette.data(), palette, palette_size));
  }
public:
  void clear() { entries.clear(); last_source.clear(); }
  // End of a simulation frame. The cache survives it: Melee reuses the same texture memory every
  // frame, so keeping the entries makes an unchanged texture cost one vectorized memcmp instead of
  // a full rehash and copy (that rehash was costing whole milliseconds per simulation frame).
  // Entries not seen for a few frames are dropped, so memory stays bounded.
  void end_frame() {
    ++generation;
    for (auto it = entries.begin(); it != entries.end();)
      it = it->second.used + KEEP_FRAMES < generation ? entries.erase(it) : std::next(it);
    for (auto it = last_source.begin(); it != last_source.end();)
      it = it->second.used + KEEP_FRAMES < generation ? last_source.erase(it) : std::next(it);
  }
  std::shared_ptr<const TextureSnapshot> capture(const uint8_t* image, size_t image_size,
                                                const uint8_t* palette, size_t palette_size) {
    // Most draws reuse their source. Vectorized memcmp avoids rehashing
    // every byte with a serial hash recurrence; changes still receive a new copy.
    auto previous = last_source.find(image);
    // There used to be a fast path here that returned the cached snapshot for a source already seen
    // this frame, comparing only the address and the two sizes. Melee rewrites texture and palette
    // memory in place, so any draw after such a write rendered the bytes from before it: a texture
    // or palette animated within a single frame showed its previous contents, which is one way a
    // draw ends up looking wrong for exactly one frame. Correctness needs the comparison, and the
    // comparison below is a vectorized memcmp of a few KB, so the cost is bounded.
    if (previous != last_source.end() && equal(*previous->second.snapshot, image, image_size, palette, palette_size)) {
      previous->second.used = generation;
      return previous->second.snapshot;
    }
    uint64_t hash = hash_bytes(image, image_size) ^ (hash_bytes(palette, palette_size) * 31);
    auto range = entries.equal_range(hash);
    for (auto i = range.first; i != range.second; ++i) {
      if (equal(*i->second.snapshot, image, image_size, palette, palette_size)) {
        i->second.used = generation;
        last_source[image] = i->second;
        return i->second.snapshot;
      }
    }
    auto s = std::make_shared<TextureSnapshot>();
    s->image.assign(image, image + image_size);
    if (palette_size) s->palette.assign(palette, palette + palette_size);
    s->hash = hash;
    entries.emplace(hash, Entry{s, generation});
    last_source[image] = Entry{s, generation};
    return s;
  }
};

inline uint32_t texture_mip_count(uint32_t width, uint32_t height, uint32_t requested) {
  uint32_t maximum = 1;
  for (uint32_t d = std::max(width, height); d > 1; d >>= 1) ++maximum;
  return std::max(1u, std::min(requested, maximum));
}
inline uint32_t texture_chain_bytes(uint32_t w, uint32_t h, uint32_t format, uint32_t levels) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < levels; ++i) {
    total += texture_level_bytes(w, h, format);
    w = std::max(1u, w / 2); h = std::max(1u, h / 2);
  }
  return total;
}
} // namespace gx
