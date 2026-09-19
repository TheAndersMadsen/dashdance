#include "render_observer.h"
// GX command processor: FIFO -> register state -> captured frames.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_core.h"
#include "host.h"
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gx {

namespace {

BPMemory g_bp;
CPMemory g_cp;
XFMemory g_xf;
uint8_t g_tmem[1024 * 1024];
uint32_t g_bp_mask = 0xFFFFFF;
int32_t g_tev_colors[4][4], g_tev_kcolors[4][4];
Backend* g_backend = nullptr;
Frame g_frame;
TextureSnapshotCache g_texture_snapshots;
uint64_t g_frame_sequence = 0;
bool g_discontinuity = false;   // simulation thread only, like the rest of this file's state
std::vector<uint8_t> g_buf;
size_t g_buf_pos = 0;   // parsed prefix of g_buf
// Bytes the pending (incomplete) command at g_buf_pos needs before it can parse. The game writes the
// FIFO a few bytes at a time, so retrying a large vertex primitive on every write was quadratic.
size_t g_parse_need = 0;
inline size_t incomplete(size_t need) { g_parse_need = need; return 0; }
uint32_t g_cp_version = 1; // bumped on every CP register write: invalidates the vertex-descriptor cache
// Draw identity bookkeeping (reset per frame).
uint32_t g_dl_addr = 0, g_dl_draw_ordinal = 0, g_dl_call_ordinal = 0;
std::unordered_map<uint32_t, uint32_t> g_dl_calls;        // display list address -> calls this frame
std::unordered_map<uint64_t, uint32_t> g_immediate_draws;  // (tex0, count, prim) -> ordinal this frame
uint64_t g_commands = 0, g_draws = 0, g_vertices = 0;
uint32_t g_efb_copies = 0;

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

// ---------------- vertex attribute description ----------------
struct AttrDesc {
  uint32_t type;      // 0 none, 1 direct, 2 index8, 3 index16
  uint32_t format;    // component format
  uint32_t count;     // elements flag
  uint32_t frac;
  uint32_t array;     // CP array index for indexed
};

struct VertexDesc {
  bool posmtx;
  bool texmtx[8];
  AttrDesc pos, nrm, col[2], tex[8];
  uint32_t nrm_index3;
  uint32_t size;      // bytes per vertex in the stream
};

uint32_t comp_bytes(uint32_t format) { static const uint32_t b[] = {1, 1, 2, 2, 4, 4, 4, 4}; return b[format & 7]; }

VertexDesc build_desc_uncached(uint32_t fmt) {
  VertexDesc d{};
  uint32_t lo = g_cp.vcd_lo(), hi = g_cp.vcd_hi();
  uint32_t a = g_cp.vat_a(fmt), b = g_cp.vat_b(fmt), c = g_cp.vat_c(fmt);
  d.posmtx = lo & 1;
  for (int i = 0; i < 8; ++i) d.texmtx[i] = (lo >> (1 + i)) & 1;
  d.pos = {bits(lo, 9, 2), bits(a, 1, 3), bits(a, 0, 1), bits(a, 4, 5), 0};
  d.nrm = {bits(lo, 11, 2), bits(a, 10, 3), bits(a, 9, 1), 0, 1};
  d.nrm_index3 = bits(a, 31, 1);
  d.col[0] = {bits(lo, 13, 2), bits(a, 14, 3), bits(a, 13, 1), 0, 2};
  d.col[1] = {bits(lo, 15, 2), bits(a, 18, 3), bits(a, 17, 1), 0, 3};
  d.tex[0] = {bits(hi, 0, 2), bits(a, 22, 3), bits(a, 21, 1), bits(a, 25, 5), 4};
  d.tex[1] = {bits(hi, 2, 2), bits(b, 1, 3), bits(b, 0, 1), bits(b, 4, 5), 5};
  d.tex[2] = {bits(hi, 4, 2), bits(b, 10, 3), bits(b, 9, 1), bits(b, 13, 5), 6};
  d.tex[3] = {bits(hi, 6, 2), bits(b, 19, 3), bits(b, 18, 1), bits(b, 22, 5), 7};
  d.tex[4] = {bits(hi, 8, 2), bits(b, 28, 3), bits(b, 27, 1), bits(c, 0, 5), 8};
  d.tex[5] = {bits(hi, 10, 2), bits(c, 6, 3), bits(c, 5, 1), bits(c, 9, 5), 9};
  d.tex[6] = {bits(hi, 12, 2), bits(c, 15, 3), bits(c, 14, 1), bits(c, 18, 5), 10};
  d.tex[7] = {bits(hi, 14, 2), bits(c, 24, 3), bits(c, 23, 1), bits(c, 27, 5), 11};
  uint32_t size = 0;
  if (d.posmtx) size += 1;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) size += 1;
  auto attr = [&](const AttrDesc& x, uint32_t direct) {
    if (x.type == 1) size += direct; else if (x.type == 2) size += 1; else if (x.type == 3) size += 2;
  };
  attr(d.pos, comp_bytes(d.pos.format) * (d.pos.count ? 3 : 2));
  if (d.nrm.type == 1) size += comp_bytes(d.nrm.format) * (d.nrm.count ? 9 : 3);
  else if (d.nrm.type == 2) size += (d.nrm.count && d.nrm_index3) ? 3 : 1;
  else if (d.nrm.type == 3) size += (d.nrm.count && d.nrm_index3) ? 6 : 2;
  static const uint32_t csize[] = {2, 3, 4, 2, 3, 4, 4, 4};
  for (int i = 0; i < 2; ++i) attr(d.col[i], csize[d.col[i].format]);
  for (int i = 0; i < 8; ++i) attr(d.tex[i], comp_bytes(d.tex[i].format) * (d.tex[i].count ? 2 : 1));
  d.size = size;
  return d;
}
// The descriptor for a vertex format only changes when a CP register does, so it is built once per
// CP state instead of once per FIFO write (the game streams a draw's vertices 4 bytes at a time, and
// the parser has to know the draw's length for every one of those writes).
const VertexDesc& build_desc(uint32_t fmt) {
  static VertexDesc cache[8];
  static uint32_t cache_version[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  fmt &= 7;
  if (cache_version[fmt] != g_cp_version) { cache[fmt] = build_desc_uncached(fmt); cache_version[fmt] = g_cp_version; }
  return cache[fmt];
}

// 2^-frac for the 5-bit vertex fraction field. The values are exact powers of two, identical to
// std::ldexp(1.0f, -frac), which cost a C library call for every component of every vertex.
constexpr float kFracScale[32] = {
    1.0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f, 0x1p-11f,
    0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f, 0x1p-22f,
    0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, 0x1p-31f};
inline float read_component(const uint8_t* p, uint32_t format, uint32_t frac) {
  const float scale = kFracScale[frac & 31];
  switch (format) {
    case 0: return (float)p[0] * scale;
    case 1: return (float)(int8_t)p[0] * scale;
    case 2: return (float)(uint16_t)be16(p) * scale;
    case 3: return (float)(int16_t)be16(p) * scale;
    default: { uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return f; }
  }
}

void read_color(const uint8_t* p, uint32_t format, uint8_t out[4]) {
  switch (format) {
    case 0: { uint32_t v = be16(p); out[0] = (uint8_t)((v >> 11) * 255 / 31); out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); out[2] = (uint8_t)((v & 31) * 255 / 31); out[3] = 255; break; }
    case 1: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3: { uint32_t v = be16(p); out[0] = (uint8_t)(((v >> 12) & 15) * 17); out[1] = (uint8_t)(((v >> 8) & 15) * 17); out[2] = (uint8_t)(((v >> 4) & 15) * 17); out[3] = (uint8_t)((v & 15) * 17); break; }
    case 4: { uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
      out[0] = (uint8_t)(((v >> 18) & 63) * 255 / 63); out[1] = (uint8_t)(((v >> 12) & 63) * 255 / 63); out[2] = (uint8_t)(((v >> 6) & 63) * 255 / 63); out[3] = (uint8_t)((v & 63) * 255 / 63); break; }
    default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
  }
}

const uint8_t* array_ptr(uint32_t array, uint32_t index) {
  uint32_t base = g_cp.array_base(array), stride = g_cp.array_stride(array);
  return host::ptr(0x80000000u | ((base + stride * index) & 0x01FFFFFFu));
}

// Decodes `count` vertices of format `fmt` from `src` into the frame. Returns components mask.
// Per-draw decode plan: everything that is constant for a draw (formats, component sizes, fixed-point
// scales, indexed-array bases and strides, the matrix-index defaults) is resolved once here, so the
// per-vertex loop is straight-line loads. The arithmetic is exactly read_component's: an integer
// converted to float and multiplied by the same power-of-two scale, or the raw IEEE float.
namespace {
struct AttrPlan {
  uint32_t type = 0;       // 0 none, 1 direct, 2 index8, 3 index16
  uint32_t format = 0, cb = 0, n = 0, direct_size = 0;
  float scale = 1.0f;
  uint32_t array_base = 0, stride = 0;   // indexed arrays (guest addresses, resolved per index like array_ptr)
  inline const uint8_t* fetch(const uint8_t*& p) const {
    if (type == 1) { const uint8_t* q = p; p += direct_size; return q; }
    uint32_t index; if (type == 2) { index = *p; p += 1; } else { index = be16(p); p += 2; }
    return indexed(index);
  }
  inline const uint8_t* indexed(uint32_t index) const {
    return host::ptr(0x80000000u | ((array_base + stride * index) & 0x01FFFFFFu));
  }
  inline float component(const uint8_t* q, uint32_t k) const {
    const uint8_t* c = q + k * cb;
    switch (format) {
      case 0: return (float)c[0] * scale;
      case 1: return (float)(int8_t)c[0] * scale;
      case 2: return (float)(uint16_t)be16(c) * scale;
      case 3: return (float)(int16_t)be16(c) * scale;
      default: { uint32_t u = be32(c); float f; std::memcpy(&f, &u, 4); return f; }
    }
  }
};
void plan_set(AttrPlan& a, const AttrDesc& x, uint32_t n) {
  a.type = x.type; a.format = x.format; a.cb = comp_bytes(x.format); a.n = n; a.direct_size = a.cb * n;
  a.scale = std::ldexp(1.0f, -(int)x.frac);
  if (a.type == 2 || a.type == 3) { a.array_base = g_cp.array_base(x.array); a.stride = g_cp.array_stride(x.array); }
}
}  // namespace

uint32_t decode_vertices(const VertexDesc& d, const uint8_t* src, uint32_t count, uint32_t fmt) {
  uint32_t components = 0;
  const uint32_t mia = g_cp.matrix_index_a(), mib = g_cp.matrix_index_b();
  // ---- plan
  uint8_t texmtx_default[8];
  for (int i = 0; i < 8; ++i) texmtx_default[i] = (uint8_t)(i < 4 ? bits(mia, 6 + 6 * i, 6) : bits(mib, 6 * (i - 4), 6));
  const uint8_t posmtx_default = (uint8_t)(mia & 63);
  if (d.posmtx) components |= VB_HAS_POSMTXIDX;
  for (int i = 0; i < 8; ++i) if (d.texmtx[i]) components |= VB_HAS_TEXMTXIDX0 << i;
  AttrPlan pos; plan_set(pos, d.pos, d.pos.count ? 3 : 2);
  AttrPlan nrm; uint32_t nrm_index_bytes = 0, nrm_direct_bytes = 0;
  if (d.nrm.type) {
    const uint32_t frac = d.nrm.format == 1 ? 6 : d.nrm.format == 3 ? 14 : d.nrm.format == 0 ? 7 : d.nrm.format == 2 ? 15 : 0;
    AttrDesc nd = d.nrm; nd.frac = frac;
    plan_set(nrm, nd, 3);
    const uint32_t nvec = d.nrm.count ? 3 : 1;
    if (nvec > 1) components |= VB_UNCAPTURED_NBT;
    nrm_direct_bytes = nrm.cb * 3 * nvec;
    nrm_index_bytes = d.nrm.type == 2 ? ((d.nrm.count && d.nrm_index3) ? 3 : 1) : ((d.nrm.count && d.nrm_index3) ? 6 : 2);
    components |= VB_HAS_NRM0;
  }
  static const uint32_t csize[] = {2, 3, 4, 2, 3, 4, 4, 4};
  AttrPlan col[2];
  for (int i = 0; i < 2; ++i) {
    if (!d.col[i].type) continue;
    plan_set(col[i], d.col[i], 1); col[i].direct_size = csize[d.col[i].format];
    components |= i ? VB_HAS_COL1 : VB_HAS_COL0;
  }
  AttrPlan tex[8];
  for (int i = 0; i < 8; ++i) {
    if (!d.tex[i].type) continue;
    plan_set(tex[i], d.tex[i], d.tex[i].count ? 2 : 1);
    components |= VB_HAS_UV0 << i;
  }
  // ---- vertices, written in place (resize value-initialises, matching the old zeroed `Vertex out{}`)
  const size_t first = g_frame.vertices.size();
  g_frame.vertices.resize(first + count);
  Vertex* outv = g_frame.vertices.data() + first;
  for (uint32_t v = 0; v < count; ++v) {
    Vertex& out = outv[v];
    const uint8_t* p = src + (size_t)v * d.size;
    out.posmtx = d.posmtx ? *p++ : posmtx_default;
    for (int i = 0; i < 8; ++i) out.texmtx[i] = d.texmtx[i] ? *p++ : texmtx_default[i];
    if (const uint8_t* q = pos.fetch(p)) for (uint32_t k = 0; k < pos.n; ++k) out.pos[k] = pos.component(q, k);
    if (d.nrm.type) {
      const uint8_t* q;
      if (d.nrm.type == 1) { q = p; p += nrm_direct_bytes; }
      else if (d.nrm.type == 2) { q = nrm.indexed(*p); p += nrm_index_bytes; }
      else { q = nrm.indexed(be16(p)); p += nrm_index_bytes; }
      if (q) for (int k = 0; k < 3; ++k) out.nrm[k] = nrm.component(q, k);
    }
    for (int i = 0; i < 2; ++i) {
      if (!d.col[i].type) continue;
      const uint8_t* q = col[i].fetch(p);
      uint8_t* dst = i ? out.col1 : out.col0;
      if (q) read_color(q, d.col[i].format, dst);
      if (!d.col[i].count) dst[3] = 255;
    }
    for (int i = 0; i < 8; ++i) {
      if (!d.tex[i].type) continue;
      if (const uint8_t* q = tex[i].fetch(p)) for (uint32_t k = 0; k < tex[i].n; ++k) out.uv[i][k] = tex[i].component(q, k);
    }
  }
  (void)fmt;
  return components;
}

// ---------------- draw snapshot ----------------
void snapshot_textures(DrawCall& dc) {
  host::SimCostScope cost(host::SIM_SNAPSHOT);
  uint32_t stages = g_bp.numtevstages() + 1;
  for (uint32_t s = 0; s < stages; ++s) {
    if (!g_bp.order_enable(s)) continue;
    int map = g_bp.order_texmap(s);
    TextureRef& t = dc.textures[map];
    if (t.used) continue;
    t.used = true;
    uint32_t i0 = g_bp.teximage0(map);
    t.width = (i0 & 0x3FF) + 1;
    t.height = ((i0 >> 10) & 0x3FF) + 1;
    t.format = (i0 >> 20) & 0xF;
    t.addr = (g_bp.teximage3(map) & 0xFFFFFF) << 5;
    uint32_t tl = g_bp.textlut(map);
    t.tlut_addr = (tl & 0x3FF) << 9;
    t.tlut_format = (tl >> 10) & 3;
    t.mode0 = g_bp.texmode0(map);
    t.mode1 = g_bp.texmode1(map);
    uint32_t min_filter = (t.mode0 >> 5) & 7;
    // GX low two filter bits select mip filtering; high bit selects minification.
    bool use_mips = (min_filter & 3) != 0;
    uint32_t max_lod = (t.mode1 >> 8) & 0xFF;
    t.mip_levels = texture_mip_count(t.width, t.height, use_mips ? ((max_lod + 15) / 16) + 1 : 1);
  }
  // Indirect stages reference textures too.
  uint32_t ind = g_bp.numindstages();
  for (uint32_t i = 0; i < ind; ++i) {
    int map = bits(g_bp.iref(), 6 * i, 3);
    TextureRef& t = dc.textures[map];
    if (t.used) continue;
    t.used = true;
    uint32_t i0 = g_bp.teximage0(map);
    t.width = (i0 & 0x3FF) + 1; t.height = ((i0 >> 10) & 0x3FF) + 1; t.format = (i0 >> 20) & 0xF;
    t.addr = (g_bp.teximage3(map) & 0xFFFFFF) << 5;
    uint32_t tl = g_bp.textlut(map);
    t.tlut_addr = (tl & 0x3FF) << 9; t.tlut_format = (tl >> 10) & 3;
    t.mode0 = g_bp.texmode0(map); t.mode1 = g_bp.texmode1(map);
    t.mip_levels = 1;
  }
  for (TextureRef& t : dc.textures) {
    if (!t.used) continue;
    uint32_t total = texture_chain_bytes(t.width, t.height, t.format, t.mip_levels);
    uint32_t offset = t.addr & 0x3FFFFFFFu;
    uint32_t palette_bytes = t.format == 8 ? 32 : t.format == 9 ? 512 : t.format == 10 ? 32768 : 0;
    if (offset >= ppc::RAM_SIZE || total > ppc::RAM_SIZE - offset ||
        t.tlut_addr > sizeof g_tmem || palette_bytes > sizeof g_tmem - t.tlut_addr)
      host::die("GX texture range invalid: %08X+%X, palette %X+%X", t.addr, total, t.tlut_addr, palette_bytes);
    t.data = g_texture_snapshots.capture(host::ram + offset, total, g_tmem + t.tlut_addr, palette_bytes);
  }
}

void record_draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t components) {
  host::SimCostScope record_cost(host::SIM_RECORD);
  // Built on the stack (hot in cache), then moved in: filling the vector element directly measured
  // worse, because each field write lands in cold memory instead of one sequential copy.
  DrawCall dc{DrawCall::SkipInit{}};
  dc.primitive = primitive;
  dc.first_vertex = first;
  dc.vertex_count = count;
  dc.components = components;
  dc.bp = g_bp;
  std::memcpy(dc.posMatrices, g_xf.posMatrices, sizeof dc.posMatrices);
  std::memcpy(dc.normalMatrices, g_xf.normalMatrices, sizeof dc.normalMatrices);
  std::memcpy(dc.xf_regs, &g_xf.raw[0x1000], sizeof dc.xf_regs);
  // Post-transform matrices and lights are 1.5 KB of the draw and most draws use neither; the
  // renderer reads them under the same tests (fill_vs_constants), so skipped copies are never read.
  if (dc.xf_regs[0x12] & 1) std::memcpy(dc.postMatrices, g_xf.postMatrices, sizeof dc.postMatrices);
  bool lit = false;
  for (uint32_t j = 0; j < (dc.xf_regs[0x09] & 3); ++j) lit = lit || lit_enable(dc.xf_regs[0x0E + j]) || lit_enable(dc.xf_regs[0x10 + j]);
  if (lit) std::memcpy(dc.lights, g_xf.lights, sizeof dc.lights);
  dc.matrix_index_a = g_cp.matrix_index_a();
  dc.matrix_index_b = g_cp.matrix_index_b();
  std::memcpy(dc.tev_colors, g_tev_colors, sizeof g_tev_colors);
  std::memcpy(dc.tev_kcolors, g_tev_kcolors, sizeof g_tev_kcolors);
  snapshot_textures(dc);
  if (g_dl_addr) {
    dc.identity = hash_bytes(&g_dl_addr, 4) ^ ((uint64_t)g_dl_call_ordinal << 40) ^ ((uint64_t)g_dl_draw_ordinal << 20) ^ 1;
    ++g_dl_draw_ordinal;
  } else {
    uint64_t k = ((uint64_t)dc.textures[0].addr << 32) ^ ((uint64_t)count << 8) ^ primitive;
    uint32_t ordinal = g_immediate_draws[k]++;
    dc.identity = hash_bytes(&k, 8) ^ ((uint64_t)ordinal << 44) ^ 2;
  }
  { host::SimCostScope cost(host::SIM_OBSERVE);
    dc.identity = observed_draw_identity(dc.identity, dc.object_generation);
    dc.owner_player = observed_owner();   // SkipInit above means this has to be written every draw
    dc.skinned = observed_skinned();
    dc.authored_pose = capture_authored_pose(); }
  g_frame.draws.push_back(std::move(dc));
  g_frame.commands.push_back({FrameCommand::Draw, (uint32_t)g_frame.draws.size() - 1});
  ++g_draws;
  g_vertices += count;
}

// ---------------- BP side effects ----------------
void bp_write(uint32_t value) {
  uint8_t r = (uint8_t)(value >> 24);
  uint32_t v = value & 0xFFFFFF;
  if (r == BP_BP_MASK) { g_bp_mask = v; return; }
  uint32_t masked = (g_bp.reg[r] & ~g_bp_mask) | (v & g_bp_mask);
  g_bp_mask = 0xFFFFFF;
  g_bp.reg[r] = masked;
  if (r >= BP_TEV_COLOR_RA && r <= BP_TEV_COLOR_RA + 7) {
    // TEV color/konst registers share BP slots; the type bit selects the destination at write time.
    int idx = (r - BP_TEV_COLOR_RA) / 2;
    bool bg = r & 1;
    int32_t (*dst)[4] = (masked & (1 << 23)) ? g_tev_kcolors : g_tev_colors;
    if (bg) { dst[idx][2] = sbits(masked, 0, 11); dst[idx][1] = sbits(masked, 12, 11); }
    else { dst[idx][0] = sbits(masked, 0, 11); dst[idx][3] = sbits(masked, 12, 11); }
    return;
  }
  switch (r) {
    case BP_SETDRAWDONE:
      if (masked & 2) host::set_pe_finish_pending();
      break;
    case BP_PE_TOKEN_INT_ID:
      host::set_pe_token_pending((uint16_t)masked);
      break;
    case BP_LOADTLUT1: {
      uint32_t tmem_addr = (masked & 0x3FF) << 9;
      uint32_t count = (masked & 0x1FFC00) >> 5;
      uint32_t src = (g_bp.reg[BP_LOADTLUT0] << 5) & 0x01FFFFFF;
      if (tmem_addr + count <= sizeof g_tmem) std::memcpy(g_tmem + tmem_addr, host::ptr(0x80000000u | src, count), count);
      break;
    }
    case BP_TRIGGER_EFB_COPY: {
      EfbCopy c{};
      capture_copy_state(c, g_bp);
      c.dest_addr = g_bp.reg[BP_EFB_ADDR] << 5;
      c.dest_stride = g_bp.reg[BP_MIPMAP_STRIDE] << 5;
      uint32_t tl = g_bp.reg[BP_EFB_TL], br = g_bp.reg[BP_EFB_BR];
      c.src_x = tl & 0x3FF; c.src_y = (tl >> 10) & 0x3FF;
      c.src_w = (br & 0x3FF) + 1; c.src_h = ((br >> 10) & 0x3FF) + 1;
      uint32_t tpf = bits(masked, 3, 4);
      c.format = tpf / 2 + (tpf & 1) * 8;
      c.to_xfb = bits(masked, 14, 1);
      c.clear = bits(masked, 11, 1);
      c.intensity = bits(masked, 15, 1);
      c.half_scale = bits(masked, 9, 1);
      c.is_depth = (g_bp.zcontrol() & 7) == 3;
      c.clear_color = ((g_bp.reg[BP_CLEAR_AR] & 0xFF) << 24) | ((g_bp.reg[BP_CLEAR_AR] & 0xFF00) << 8) |
                      ((g_bp.reg[BP_CLEAR_GB] & 0xFF00) >> 8 << 8) | (g_bp.reg[BP_CLEAR_GB] & 0xFF);
      // clear_color layout: A(31..24) R(23..16) G(15..8) B(7..0)
      c.clear_color = ((g_bp.reg[BP_CLEAR_AR] >> 8) & 0xFF) << 24 | (g_bp.reg[BP_CLEAR_AR] & 0xFF) << 16 |
                      ((g_bp.reg[BP_CLEAR_GB] >> 8) & 0xFF) << 8 | (g_bp.reg[BP_CLEAR_GB] & 0xFF);
      c.clear_z = g_bp.reg[BP_CLEAR_Z] & 0xFFFFFF;
      uint32_t yscale = g_bp.reg[BP_COPYYSCALE];
      c.y_scale = bits(masked, 10, 1) ? 256.0f / (float)yscale : (float)yscale / 256.0f;
      if (!c.to_xfb && !c.is_depth && c.format == 6 && !c.half_scale) {
        const uint64_t size = uint64_t(c.dest_stride) * ((c.src_h + 3) / 4);
        const uint32_t offset = c.dest_addr & 0x3fffffffu;
        if (offset < ppc::RAM_SIZE && size <= ppc::RAM_SIZE - offset)
          c.destination_before_copy.assign(host::ram + offset, host::ram + offset + size);
      }
      g_frame.copies.push_back(c);
      g_frame.commands.push_back({FrameCommand::Copy, (uint32_t)g_frame.copies.size() - 1});
      ++g_efb_copies;
      if (c.to_xfb) {
        g_frame.sequence = ++g_frame_sequence;
        g_frame.scene_major = host::rd8(0x80479D30);
        g_frame.scene_minor = host::rd8(0x80479D33);
        {
          static uint16_t last_scene = 0xFFFF;
          const uint16_t scene = (uint16_t)(g_frame.scene_major << 8 | g_frame.scene_minor);
          if (scene != last_scene) { host::log("scene: major %02X minor %02X (frame %llu)", g_frame.scene_major, g_frame.scene_minor, (unsigned long long)g_frame.sequence); last_scene = scene; }
        }
        g_frame.time = host::now_seconds(); // completed snapshot availability anchors presentation
        g_frame.discontinuous = g_discontinuity;
        g_discontinuity = false;
        if (g_backend) g_backend->submit_and_recycle(g_frame);   // hands over the buffers, returns recycled ones
        else g_frame.clear();
        g_texture_snapshots.end_frame();
        g_dl_calls.clear(); g_immediate_draws.clear(); finish_observed_frame();
      }
      break;
    }
    default:
      break;
  }
}

// ---------------- XF ----------------
void xf_load(uint32_t address, uint32_t count, const uint8_t* data) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t a = address + i;
    if (a < 0x1058) g_xf.raw[a] = be32(data + i * 4);
  }
}

void xf_indexed_load(uint32_t op, uint32_t value) {
  uint32_t index = value >> 16, address = value & 0xFFF, size = ((value >> 12) & 0xF) + 1;
  uint32_t array = 12 + (op - 0x20) / 8;
  const uint8_t* src = array_ptr(array, index);
  xf_load(address, size, src);
}

// ---------------- command parsing ----------------
size_t parse_command(const uint8_t* d, size_t len);

void run_display_list(uint32_t addr, uint32_t size) {
  if ((addr & 0x3FFFFFFFu) + size > 0x01800000u) { host::log("gx: display list outside RAM %08X+%X", addr, size); return; }
  const uint8_t* p = host::ptr(addr, size);
  uint32_t saved_addr = g_dl_addr, saved_draw = g_dl_draw_ordinal, saved_call = g_dl_call_ordinal;
  g_dl_addr = addr; g_dl_draw_ordinal = 0; g_dl_call_ordinal = g_dl_calls[addr]++;
  size_t used = 0;
  while (used < size) {
    size_t n = parse_command(p + used, size - used);
    if (!n) break;
    used += n;
  }
  g_dl_addr = saved_addr; g_dl_draw_ordinal = saved_draw; g_dl_call_ordinal = saved_call;
}

// Parses one command at `d`; returns its length, or 0 when `len` is short (g_need then holds the
// total length the command will have, so the FIFO writer can skip parsing until it has arrived).
size_t parse_command(const uint8_t* d, size_t len) {
  uint8_t op = d[0];
  if (op == 0x00) return 1;
  if (op == 0x08) { if (len < 6) return incomplete(6); g_cp.reg[d[1]] = be32(d + 2); ++g_cp_version; return 6; }
  if (op == 0x10) {
    if (len < 5) return incomplete(5);
    uint32_t count = (be16(d + 1) & 0xF) + 1, address = be16(d + 3);
    size_t need = 5 + count * 4;
    if (len < need) return incomplete(need);
    xf_load(address, count, d + 5);
    return need;
  }
  if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
    if (len < 5) return incomplete(5);
    xf_indexed_load(op, be32(d + 1));
    return 5;
  }
  if (op == 0x40) {
    if (len < 9) return incomplete(9);
    run_display_list(be32(d + 1), be32(d + 5));
    return 9;
  }
  if (op == 0x48) return 1;
  if (op == 0x61) { if (len < 5) return incomplete(5); bp_write(be32(d + 1)); return 5; }
  if (op >= 0x80 && op < 0xC0) {
    if (len < 3) return incomplete(3);
    uint32_t fmt = op & 7, count = be16(d + 1);
    const VertexDesc& desc = build_desc(fmt);
    size_t need = 3 + (size_t)count * desc.size;
    if (len < need) return incomplete(need);
    if (count) {
      uint32_t first = (uint32_t)g_frame.vertices.size();
      uint32_t components = decode_vertices(desc, d + 3, count, fmt);
      record_draw(op & 0xF8, first, count, components);
    }
    return need;
  }
  static int reported = 0;
  if (reported++ < 10) host::log("gx: unknown opcode %02X", op);
  return 1;
}

}  // namespace

void mark_discontinuity() { g_discontinuity = true; }

void init(Backend* backend) {
  g_backend = backend;
  std::memset(&g_bp, 0, sizeof g_bp);
  std::memset(&g_cp, 0, sizeof g_cp);
  std::memset(&g_xf, 0, sizeof g_xf);
  std::memset(g_tmem, 0, sizeof g_tmem);
  g_frame.clear();
  g_buf.clear(); g_buf_pos = 0; g_parse_need = 0; ++g_cp_version;
}

void write_fifo(uint32_t value, int bytes) {
  const size_t at = g_buf.size();
  g_buf.resize(at + (size_t)bytes);   // one size update per write instead of a push_back per byte
  for (int i = 0; i < bytes; ++i) g_buf[at + i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
  if (g_buf.size() - g_buf_pos < g_parse_need) return;   // the pending command is still incomplete
  g_parse_need = 0;
  while (g_buf_pos < g_buf.size()) {
    size_t n = parse_command(g_buf.data() + g_buf_pos, g_buf.size() - g_buf_pos);
    if (!n) break;               // parse_command recorded how many bytes it needs
    g_buf_pos += n;
    g_parse_need = 0;            // a display list inside the command may have left a stale need
    ++g_commands;
  }
  if (g_buf_pos == g_buf.size()) { g_buf.clear(); g_buf_pos = 0; }
  else if (g_buf_pos >= 1 << 16) { g_buf.erase(g_buf.begin(), g_buf.begin() + g_buf_pos); g_buf_pos = 0; }
}

void stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies) {
  if (commands) *commands = g_commands;
  if (draws) *draws = g_draws;
  if (vertices) *vertices = g_vertices;
  if (efb_copies) *efb_copies = g_efb_copies;
}

const uint8_t* tmem() { return g_tmem; }

}  // namespace gx
