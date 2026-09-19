// SPDX-License-Identifier: GPL-2.0-or-later
#include "subframe.h"
#include "authored_pose.h"
#include "gx_shader.h"
#include <cstdlib>
#include <cstdio>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <cmath>
#include <cstring>
#include <thread>
#include <algorithm>
#include <mutex>
#include <functional>
#include <condition_variable>

namespace gx {
EndpointStats& subframe_endpoint_stats() { static EndpointStats e; return e; }
StageSplitAudit& subframe_stage_split_audit() { static StageSplitAudit a; return a; }
PhaseFlipStats& subframe_phase_flips() { static PhaseFlipStats p; return p; }
// Development measurement, off unless MELEE_AUDIT_STAGE_SPLIT is in the environment: reports how
// far apart the solver's routes place pieces of the stage that are standing still in the world.
// Development bisect, off unless MELEE_SUBFRAME_DIAG is set: bit 1 no stage object sampling,
// 2 no vertex blending, 4 no camera carry, 8 no UV matrices, 16 no skinned sampling, 32 no stage lock.
static uint32_t subframe_diag() {
  static const uint32_t bits = std::getenv("MELEE_SUBFRAME_DIAG") ? (uint32_t)std::strtoul(std::getenv("MELEE_SUBFRAME_DIAG"), nullptr, 0) : 0u;
  return bits;
}
// Development: MELEE_DUMP_SEQ=<simulation frame> writes every draw's pairing and route for that
// frame to subframe_dump.txt in the working directory.
static uint64_t dump_sequence() {
  static const uint64_t seq = std::getenv("MELEE_DUMP_SEQ") ? std::strtoull(std::getenv("MELEE_DUMP_SEQ"), nullptr, 0) : 0;
  return seq;
}
static bool audit_stage_split() {
  static const bool on = std::getenv("MELEE_AUDIT_STAGE_SPLIT") != nullptr;
  return on;
}
// How far a single presented frame may move a draw before the result is treated as wrong. One
// simulation frame of the fastest legitimate motion in Melee is far below this; a
// reconstruction error is far above it.
static constexpr float kMaxCarryTranslation = 600.0f;
namespace {

// Whether two draws render with the same material, looking only at state that reaches the output.
// Comparing every BP register rejected most of a frame whenever an earlier draw (a hit spark, a
// stage effect, menu text) left different values in TEV stages, texture coordinates or indirect
// stages that these draws do not use: whole scenes then lost their in-between frames for one tick
// and snapped (Yoshi's Story, Fountain of Dreams, menus, the stage select cursor). The shader uid
// already filters out unused stages; add the pipeline state it leaves out and the texture
// coordinate scales of the generators the draw actually uses. Constants (TEV colours, fog, alpha
// references) are not compared: the presented draw always takes them from the current frame.
bool same_draw_state(const DrawCall& a, const DrawCall& b, uint32_t& changed, bool compare_shader) {
  // The shader identity is only needed to tell two draws apart when nothing else can. It is the
  // wrong test for a draw the observer tracked, because a material change on the same object then
  // reads as a different object: Melee's intangibility flash adds a TEV stage to the fighter
  // (GENMODE numtevstages 2 <-> 3) twice per six-frame flash cycle, which unpaired an airdodging
  // fighter for two whole ticks per cycle. He held still while the scene advanced, then snapped, a
  // 20 Hz stall in time with the flash. Blend, alpha, cull and texcoord scale are still compared
  // below, so a genuinely different material still breaks the pair.
  if (compare_shader) {
    const PSUid ua = make_ps_uid(a), ub = make_ps_uid(b);
    if (!(ua == ub)) {
      changed = 0;
      for (unsigned i = 0; i < 256; ++i) if (ua.bp.reg[i] != ub.bp.reg[i]) { changed = i; break; }
      return false;
    }
  }
  if (a.bp.blendmode() != b.bp.blendmode()) { changed = BP_BLENDMODE; return false; }
  if (a.bp.dstalpha() != b.bp.dstalpha()) { changed = BP_CONSTANTALPHA; return false; }
  if (a.bp.cullmode() != b.bp.cullmode()) { changed = 0; return false; }
  const unsigned generators = std::min(8u, b.xf_regs[0x3F] & 15);
  for (unsigned i = 0; i < generators; ++i) {
    if (a.bp.texcoord_s((int)i) != b.bp.texcoord_s((int)i)) { changed = BP_SU_SSIZE + 2 * i; return false; }
    if (a.bp.texcoord_t((int)i) != b.bp.texcoord_t((int)i)) { changed = BP_SU_SSIZE + 2 * i + 1; return false; }
  }
  return true;
}

bool same_textures(const DrawCall& a, const DrawCall& b) {
  for (unsigned i = 0; i < 8; ++i) {
    const auto& x = a.textures[i]; const auto& y = b.textures[i];
    if (x.used != y.used) return false;
    if (!x.used) continue;
    // The address is deliberately not compared: the same material can be regenerated into a
    // different buffer each frame. Shape, format and sampler state must still match.
    if (x.width != y.width || x.height != y.height ||
        x.format != y.format || x.mip_levels != y.mip_levels || x.tlut_format != y.tlut_format ||
        x.mode0 != y.mode0 || x.mode1 != y.mode1) return false;
    if (x.data == y.data) continue;
    if (!x.data || !y.data || x.data->hash != y.data->hash ||
        x.data->image != y.data->image || x.data->palette != y.data->palette) return false;
  }
  return true;
}

uint32_t texture_matrix_index(const DrawCall& d, unsigned generator) {
  return generator < 4 ? bits(d.matrix_index_a, 6 + 6 * generator, 6)
                       : bits(d.matrix_index_b, 6 * (generator - 4), 6);
}

bool same_matrix_bindings(const DrawCall& a, const DrawCall& b) {
  if (!(b.components & VB_HAS_POSMTXIDX) && (a.matrix_index_a & 63) != (b.matrix_index_a & 63)) return false;
  if (a.xf_regs[0x3F] != b.xf_regs[0x3F] || (a.xf_regs[0x12] & 1) != (b.xf_regs[0x12] & 1)) return false;
  for (unsigned k = 0; k < std::min(8u, b.xf_regs[0x3F] & 15); ++k) {
    if (a.xf_regs[0x40 + k] != b.xf_regs[0x40 + k]) return false;
    if ((b.xf_regs[0x12] & 1) && a.xf_regs[0x50 + k] != b.xf_regs[0x50 + k]) return false;
    if (!(b.components & (VB_HAS_TEXMTXIDX0 << k)) &&
        texture_matrix_index(a, k) != texture_matrix_index(b, k)) return false;
  }
  return true;
}

// Exact geometry is the reuse gate. A stable draw address/ordinal does not prove
// that regenerated vertices describe the same surface. Ignore only padding and
// attributes absent from the captured layout; compare every active input.
bool same_geometry(const Vertex* a, const Vertex* b, uint32_t count, uint32_t components) {
  if (components & VB_UNCAPTURED_NBT) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (std::memcmp(a[i].pos, b[i].pos, sizeof a[i].pos)) return false;
    if ((components & VB_HAS_POSMTXIDX) && a[i].posmtx != b[i].posmtx) return false;
    if ((components & VB_HAS_NRM0) && std::memcmp(a[i].nrm, b[i].nrm, sizeof a[i].nrm)) return false;
    if ((components & VB_HAS_COL0) && std::memcmp(a[i].col0, b[i].col0, sizeof a[i].col0)) return false;
    if ((components & VB_HAS_COL1) && std::memcmp(a[i].col1, b[i].col1, sizeof a[i].col1)) return false;
    for (unsigned k = 0; k < 8; ++k) {
      if ((components & (VB_HAS_UV0 << k)) && std::memcmp(a[i].uv[k], b[i].uv[k], sizeof a[i].uv[k])) return false;
      if ((components & (VB_HAS_TEXMTXIDX0 << k)) && a[i].texmtx[k] != b[i].texmtx[k]) return false;
    }
  }
  return true;
}

// 3x4 row-major affine (XF layout): rows r0..r2, translation in column 3.
struct Affine { float m[12]; };

Affine mul(const Affine& a, const Affine& b) {   // a * b
  Affine r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] + a.m[i * 4 + 2] * b.m[2 * 4 + j];
      if (j == 3) s += a.m[i * 4 + 3];
      r.m[i * 4 + j] = s;
    }
  return r;
}

bool invert(const Affine& a, Affine& out) {
  const float* m = a.m;
  double det = (double)m[0] * (m[5] * m[10] - m[6] * m[9]) - (double)m[1] * (m[4] * m[10] - m[6] * m[8]) + (double)m[2] * (m[4] * m[9] - m[5] * m[8]);
  if (std::fabs(det) < 1e-12) return false;
  double id = 1.0 / det;
  float inv[9] = {
      (float)((m[5] * m[10] - m[6] * m[9]) * id), (float)((m[2] * m[9] - m[1] * m[10]) * id), (float)((m[1] * m[6] - m[2] * m[5]) * id),
      (float)((m[6] * m[8] - m[4] * m[10]) * id), (float)((m[0] * m[10] - m[2] * m[8]) * id), (float)((m[2] * m[4] - m[0] * m[6]) * id),
      (float)((m[4] * m[9] - m[5] * m[8]) * id),  (float)((m[1] * m[8] - m[0] * m[9]) * id),  (float)((m[0] * m[5] - m[1] * m[4]) * id)};
  for (int i = 0; i < 3; ++i) {
    out.m[i * 4 + 0] = inv[i * 3 + 0]; out.m[i * 4 + 1] = inv[i * 3 + 1]; out.m[i * 4 + 2] = inv[i * 3 + 2];
    out.m[i * 4 + 3] = -(inv[i * 3 + 0] * m[3] + inv[i * 3 + 1] * m[7] + inv[i * 3 + 2] * m[11]);
  }
  return true;
}

struct Quat { double w, x, y, z; };

Quat quat_from_rotation(const double r[9]) {   // row-major 3x3, assumed orthonormal
  Quat q;
  double tr = r[0] + r[4] + r[8];
  if (tr > 0) {
    double s = std::sqrt(tr + 1.0) * 2;
    q.w = 0.25 * s; q.x = (r[7] - r[5]) / s; q.y = (r[2] - r[6]) / s; q.z = (r[3] - r[1]) / s;
  } else if (r[0] > r[4] && r[0] > r[8]) {
    double s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
    q.w = (r[7] - r[5]) / s; q.x = 0.25 * s; q.y = (r[1] + r[3]) / s; q.z = (r[2] + r[6]) / s;
  } else if (r[4] > r[8]) {
    double s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
    q.w = (r[2] - r[6]) / s; q.x = (r[1] + r[3]) / s; q.y = 0.25 * s; q.z = (r[5] + r[7]) / s;
  } else {
    double s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
    q.w = (r[3] - r[1]) / s; q.x = (r[2] + r[6]) / s; q.y = (r[5] + r[7]) / s; q.z = 0.25 * s;
  }
  double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n > 0) { q.w /= n; q.x /= n; q.y /= n; q.z /= n; }
  return q;
}

void rotation_from_quat(const Quat& q, double r[9]) {
  double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  r[0] = 1 - 2 * (yy + zz); r[1] = 2 * (xy - wz);     r[2] = 2 * (xz + wy);
  r[3] = 2 * (xy + wz);     r[4] = 1 - 2 * (xx + zz); r[5] = 2 * (yz - wx);
  r[6] = 2 * (xz - wy);     r[7] = 2 * (yz + wx);     r[8] = 1 - 2 * (xx + yy);
}

}  // namespace

void SubFrameSolver::interpolate_matrix(const float prev[12], const float cur[12], double t, float out[12]) {
  fractional(prev, cur, t, true, 40.0f, 1.2f, out, nullptr, nullptr, nullptr, nullptr);
}

void SubFrameSolver::extrapolate_matrix(const float prev[12], const float cur[12], double t, float out[12]) {
  fractional(prev, cur, t, false, 40.0f, 1.2f, out, nullptr, nullptr, nullptr, nullptr);
}

void SubFrameSolver::fractional(const float prev[12], const float cur[12], double t, bool interpolate,
                                float max_translation, float max_rotation, float out_pos[12], float out_nrm[9],
                                const float cur_nrm[9], const float prev_nrm[9], SubFrameStats* stats) {
  const float* base = interpolate ? prev : cur;
  const float* base_nrm = interpolate ? prev_nrm : cur_nrm;
  auto exact = [&](bool current = true) {
    std::memcpy(out_pos, current ? cur : base, 12 * sizeof(float));
    const float* normals = current ? cur_nrm : base_nrm;
    if (out_nrm && normals) std::memcpy(out_nrm, normals, 9 * sizeof(float));
  };
  if (!std::isfinite(t)) { exact(); if (stats) ++stats->cuts; return; }
  for (int i = 0; i < 12; ++i) if (!std::isfinite(prev[i]) || !std::isfinite(cur[i])) {
    exact(); if (stats) ++stats->cuts; return;
  }
  if (interpolate && t >= 1.0) { exact(); return; }
  if (t <= 0.0) { exact(false); return; }
  t = std::min(t, 1.0);
  if (std::memcmp(prev, cur, 12 * sizeof(float)) == 0) { exact(); return; }
  Affine P, C, Pi;
  std::memcpy(P.m, prev, sizeof P.m); std::memcpy(C.m, cur, sizeof C.m);
  if (!invert(P, Pi)) { exact(); if (stats) ++stats->cuts; return; }
  Affine D = mul(C, Pi);
  // Decompose the 3x3 of D into scale per column and a rotation; test near-rigidity.
  double col_len[3];
  for (int j = 0; j < 3; ++j) col_len[j] = std::sqrt((double)D.m[j] * D.m[j] + (double)D.m[4 + j] * D.m[4 + j] + (double)D.m[8 + j] * D.m[8 + j]);
  bool rigid = true;
  double r[9];
  for (int j = 0; j < 3; ++j) {
    if (col_len[j] < 0.5 || col_len[j] > 2.0) rigid = false;
    for (int i = 0; i < 3; ++i) r[i * 3 + j] = col_len[j] > 0 ? D.m[i * 4 + j] / col_len[j] : 0.0;
  }
  if (rigid) {
    for (int a = 0; a < 3 && rigid; ++a)
      for (int b = a + 1; b < 3; ++b) {
        double dot = r[a] * r[b] + r[3 + a] * r[3 + b] + r[6 + a] * r[6 + b];
        if (std::fabs(dot) > 0.05) { rigid = false; break; }
      }
    double det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (det < 0.5) rigid = false;   // reflection or degenerate
  }
  double tx = D.m[3], ty = D.m[7], tz = D.m[11];
  double tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
  if (tlen > max_translation) { exact(); if (stats) ++stats->cuts; return; }
  Affine F;   // fractional delta
  if (rigid) {
    Quat q = quat_from_rotation(r);
    if (q.w < 0) { q.w = -q.w; q.x = -q.x; q.y = -q.y; q.z = -q.z; }
    double angle = 2.0 * std::acos(std::min(1.0, q.w));
    if (angle > max_rotation) { exact(); if (stats) ++stats->cuts; return; }
    // Screw motion: rotate by t*angle about the same axis through the same point, slide t of the
    // way along the axis. F(0) = I, F(1) = D, and F(t) stays on the true rigid path between them.
    double half = angle * t * 0.5, sh = std::sin(half), axis_n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    double n[3] = {0, 0, 1};
    if (axis_n > 1e-9) { n[0] = q.x / axis_n; n[1] = q.y / axis_n; n[2] = q.z / axis_n; }
    Quat qt{std::cos(half), n[0] * sh, n[1] * sh, n[2] * sh};
    double rt[9]; rotation_from_quat(qt, rt);
    double st[3];
    for (int j = 0; j < 3; ++j) st[j] = std::pow(col_len[j], t);
    double T[3] = {tx, ty, tz}, ft[3];
    if (angle < 1e-6) {
      for (int i = 0; i < 3; ++i) ft[i] = T[i] * t;
    } else {
      double along = T[0] * n[0] + T[1] * n[1] + T[2] * n[2];
      double perp[3] = {T[0] - along * n[0], T[1] - along * n[1], T[2] - along * n[2]};
      double cross[3] = {n[1] * perp[2] - n[2] * perp[1], n[2] * perp[0] - n[0] * perp[2], n[0] * perp[1] - n[1] * perp[0]};
      double cot = std::cos(angle * 0.5) / std::sin(angle * 0.5);
      double p[3];   // a point on the screw axis: (I - R) p = perp
      for (int i = 0; i < 3; ++i) p[i] = 0.5 * (perp[i] + cot * cross[i]);
      for (int i = 0; i < 3; ++i) {
        double rp = rt[i * 3 + 0] * p[0] + rt[i * 3 + 1] * p[1] + rt[i * 3 + 2] * p[2];
        ft[i] = p[i] - rp + along * t * n[i];
      }
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) F.m[i * 4 + j] = (float)(rt[i * 3 + j] * st[j]);
      F.m[i * 4 + 3] = (float)ft[i];
    }
    if (stats) ++stats->rigid;
  } else {
    // Shear/reflection/large scale deltas are not a reliable animation path.
    exact(); if (stats) ++stats->cuts; return;
  }
  Affine B; std::memcpy(B.m, base, sizeof B.m);
  Affine R = mul(F, B);
  std::memcpy(out_pos, R.m, sizeof R.m);
  if (out_nrm && base_nrm) {
    // Normals transform by inverse transpose, including non-uniform scale.
    Affine inverse;
    if (!invert(F, inverse)) { exact(); if (stats) ++stats->cuts; return; }
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        out_nrm[i * 3 + j] = inverse.m[i] * base_nrm[j] +
            inverse.m[4 + i] * base_nrm[3 + j] + inverse.m[8 + i] * base_nrm[6 + j];
  }
}

void SubFrameSolver::set_frames(const Frame* prev, const Frame* cur) {
  if (prev && cur && cur->sequence != prev->sequence + 1) prev = nullptr;
  prev_ = prev; cur_ = cur;
  pairs_.clear(); prev_index_.clear();
  last_posed_.clear(); last_build_phase_ = -1;
  // Roll the pairing record forward one simulation frame, so pair_flips compares like with like.
  pair_history_.swap(pair_seen_);
  pair_seen_.clear();
  camera_previous_ = camera_current_ = nullptr;
  stats_ = SubFrameStats{};
  if (!cur) return;
  stats_.draws = (uint32_t)cur->draws.size();
  if (prev) {
    prev_index_.reserve(prev->draws.size());
    for (size_t i = 0; i < prev->draws.size(); ++i) prev_index_.push_back({prev->draws[i].identity, (int)i});
    // Stable order keeps the first draw for a repeated identity, as the hash map did.
    std::stable_sort(prev_index_.begin(), prev_index_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  pairs_.resize(cur->draws.size());
  if (dump_sequence() && cur->sequence == dump_sequence()) pair_reason_.assign(cur->draws.size(), 0);
  for (size_t i = 0; i < cur->draws.size(); ++i) {
    const DrawCall& d = cur->draws[i];
    if (d.authored_pose && d.authored_pose->envelope) ++stats_.skinned;
    Pair p{-1, 0, 0, 0, false, 0};
    auto it = std::lower_bound(prev_index_.begin(), prev_index_.end(), d.identity, [](const std::pair<uint64_t, int>& e, uint64_t id) { return e.first < id; });
    if (it != prev_index_.end() && it->first != d.identity) it = prev_index_.end();
    if (it != prev_index_.end()) {
      const DrawCall& pd = prev->draws[it->second];
      bool vertex_ranges_valid = pd.first_vertex <= prev->vertices.size() && pd.vertex_count <= prev->vertices.size() - pd.first_vertex &&
          d.first_vertex <= cur->vertices.size() && d.vertex_count <= cur->vertices.size() - d.first_vertex;
      // Same stream, byte for byte: the draw moves by its matrices alone. Otherwise the geometry
      // itself is animated, and the two streams are blended per presented frame (below) as long as
      // they still describe the same primitive with the same layout.
      bool same_vertices = vertex_ranges_valid && d.vertex_count && pd.vertex_count == d.vertex_count &&
          same_geometry(prev->vertices.data() + pd.first_vertex, cur->vertices.data() + d.first_vertex,
                        d.vertex_count, d.components);
      bool valid = false; int reason = 0;
      if (d.object_generation != pd.object_generation) { ++stats_.missing; reason = 1; }
      else if (d.xf_regs[0x26] != 0) { ++stats_.hud; reason = 2; }
      // Immediate-mode text and other unobserved primitives use texture/count/
      // submission-ordinal identities. Inserting or removing a glyph can reuse
      // that key for a different primitive. A changing stream needs an observed
      // object lifetime before it is safe to invent geometry between the draws.
      else if (!d.object_generation && !same_vertices) { ++stats_.geometry; reason = 3; }
      else if (!vertex_ranges_valid || !d.vertex_count || pd.vertex_count != d.vertex_count ||
          pd.primitive != d.primitive || pd.components != d.components) { ++stats_.geometry; reason = 4; }
      // The shader identity is only compared for draws the observer did not see; an observed draw is
      // already identified by generation, pass and draw ordinal (see same_draw_state).
      else if (!same_draw_state(pd, d, stats_.state_register, !d.object_generation) ||
               !same_textures(pd, d) || !same_matrix_bindings(pd, d)) { ++stats_.state; reason = 5; }
      else if (std::memcmp(&pd.xf_regs[0x20], &d.xf_regs[0x20], 7 * sizeof(uint32_t))) { ++stats_.projection; reason = 6; }
      else { valid = true; p.blend_vertices = !same_vertices; }
      if (dump_sequence() && cur->sequence == dump_sequence()) pair_reason_[i] = reason;
      // Did this identity pair last frame? Flipping between held and re-posed is what the eye
      // reads as an object flashing; a steady hold is invisible.
      {
        auto was = pair_history_.find(d.identity);
        if (was != pair_history_.end() && was->second != valid) ++stats_.pair_flips;
        pair_seen_[d.identity] = valid;
      }
      if (valid) {
        p.prev_draw = it->second;
        // Collect used position matrix slots: per-vertex indices or the CP default, plus texgen matrices.
        if (d.components & VB_HAS_POSMTXIDX) {
          for (uint32_t v = 0; v < d.vertex_count; ++v) {
            uint32_t idx = cur->vertices[d.first_vertex + v].posmtx;
            if (idx < 64) p.pos_slots |= 1ull << idx;
          }
        } else {
          p.pos_slots |= 1ull << (d.matrix_index_a & 63);
        }
        uint32_t texgens = d.xf_regs[0x3F] & 15;
        for (uint32_t k = 0; k < texgens && k < 8; ++k) {
          const uint32_t type = tmi_texgentype(d.xf_regs[0x40 + k]);
          if (type >= 1 && type <= 3) continue; // emboss/colour generators do not read these matrices
          if (d.components & (VB_HAS_TEXMTXIDX0 << k)) {
            for (uint32_t v = 0; v < d.vertex_count; ++v) {
              const uint32_t idx = cur->vertices[d.first_vertex + v].texmtx[k];
              if (idx < 64) p.tex_slots |= 1ull << idx;
            }
          } else {
            p.tex_slots |= 1ull << texture_matrix_index(d, k);
          }
        }
        p.used_slots = p.pos_slots | p.tex_slots;
        // One camera per frame: keep the first paired pair of poses that carries a view, so draws
        // that cannot be paired at all can still be moved onto this frame's timeline (below).
        if (!camera_previous_ && d.authored_pose && pd.authored_pose && d.authored_pose->has_view && pd.authored_pose->has_view) {
          camera_previous_ = pd.authored_pose.get(); camera_current_ = d.authored_pose.get();
        }
        ++stats_.paired;
      }
    }
    if (it == prev_index_.end()) { ++stats_.missing; if (dump_sequence() && cur->sequence == dump_sequence()) pair_reason_[i] = 7; }
    pairs_[i] = p;
  }
  // Reserve a disjoint range of the blend buffer for every draw whose geometry is animated, so the
  // parallel solver chunks can fill them without sharing anything.
  size_t blended_vertices = 0;
  for (size_t i = 0; i < pairs_.size(); ++i) {
    if (!pairs_[i].blend_vertices) continue;
    pairs_[i].blend_offset = blended_vertices;
    blended_vertices += cur->draws[i].vertex_count;
  }
  vertex_blend_.resize(blended_vertices);
}

// Blends one draw's vertex stream between the two simulation frames. Sparks, shields, hit flashes
// and similar effects rewrite their vertices every frame rather than moving a matrix, so this is
// the only way they can move between simulation frames. Returns false when a vertex jumps far
// enough that the buffer is showing different geometry rather than the same geometry in motion.
// Where a matrix slot moved between the two frames. The game loads an object's matrices into
// whichever XF slots are free that frame, so the same geometry can arrive with every vertex pointing
// at a different slot than it did a tick ago while nothing about the object changed. `pos_map[s]` is
// the previous frame's slot for current slot s (-1 when unused, s when unchanged); `tex_map` the same
// for texture-coordinate matrices. A slot that maps to two different previous slots is not a move
// but a real rebinding, and that still rejects the pair.
struct SlotRemap { int8_t pos_map[64]; int8_t tex_map[64]; bool moved = false; };

static bool blend_vertex_stream(const Vertex* previous, const Vertex* current, uint32_t count,
                                double t, bool interpolate, float max_translation, Vertex* out, SlotRemap& remap,
                                uint32_t components, uint32_t texgens, bool legacy_selectors) {
  const float phase = (float)t;
  std::memset(remap.pos_map, -1, sizeof remap.pos_map);
  std::memset(remap.tex_map, -1, sizeof remap.tex_map);
  remap.moved = false;
  // A move is a one-to-one renaming of slots. One previous slot feeding two current ones means a
  // vertex was rebound to a different matrix than its neighbours, which is a real change of binding.
  int8_t pos_back[64], tex_back[64];
  std::memset(pos_back, -1, sizeof pos_back);
  std::memset(tex_back, -1, sizeof tex_back);
  for (uint32_t v = 0; v < count; ++v) {
    // Matrix selectors are discrete bindings, not animation channels. Blending positions across a
    // changed binding deforms a newly assigned primitive. But a binding that moved for EVERY vertex
    // of the draw the same way is the object's matrix living in a different slot this frame, which
    // is how Yoshi's Story's tree arrived every single tick: identical geometry, new slot numbers,
    // rejected, and so drawn at the current frame while the rest of the scene was in between. Map
    // the slots instead, and reject only when one current slot claims two previous ones.
    // The menus keep the comparison exactly as it was before the stage work: every selector, used
    // or not. It rejects a draw whenever leftover state from the previous draw differs, which is
    // most ticks, and that is the point. A menu is rebuilt rather than moved, and the one thing it
    // cannot tolerate is being rebuilt draw by draw into two different treatments.
    if (legacy_selectors) {
      if (previous[v].posmtx != current[v].posmtx ||
          std::memcmp(previous[v].texmtx, current[v].texmtx, sizeof current[v].texmtx)) return false;
    } else if (components & VB_HAS_POSMTXIDX) {
      const int cs = current[v].posmtx & 63, ps = previous[v].posmtx & 63;
      if (remap.pos_map[cs] < 0) remap.pos_map[cs] = (int8_t)ps;
      else if (remap.pos_map[cs] != ps) return false;
      if (pos_back[ps] < 0) pos_back[ps] = (int8_t)cs;
      else if (pos_back[ps] != cs) return false;
      if (cs != ps) remap.moved = true;
    }
    // Only the generators this draw uses, and only where the index comes from the vertex. The rest
    // of texmtx[] is whatever state the previous draw left behind, and on Yoshi's Story that
    // leftover differed between frames on every draw of the tree (generator 1, unused, slot 0 one
    // tick and the identity slot the next), which rejected the whole tree every tick.
    for (uint32_t k = 0; k < texgens && k < 8 && !legacy_selectors; ++k) {
      if (!(components & (VB_HAS_TEXMTXIDX0 << k))) continue;
      const int cs = current[v].texmtx[k] & 63, ps = previous[v].texmtx[k] & 63;
      if (remap.tex_map[cs] < 0) remap.tex_map[cs] = (int8_t)ps;
      else if (remap.tex_map[cs] != ps) return false;
      if (tex_back[ps] < 0) tex_back[ps] = (int8_t)cs;
      else if (tex_back[ps] != cs) return false;
      if (cs != ps) remap.moved = true;
    }
    for (int k = 0; k < 3; ++k) {
      const float delta = current[v].pos[k] - previous[v].pos[k];
      if (!std::isfinite(delta) || std::abs(delta) > max_translation) return false;
    }
  }
  for (uint32_t v = 0; v < count; ++v) {
    const Vertex& a = previous[v];
    const Vertex& b = current[v];
    Vertex& o = out[v];
    o = b;   // indices, matrix selects and anything not blended come from the current frame
    const float* base_pos = interpolate ? a.pos : b.pos;
    for (int k = 0; k < 3; ++k) o.pos[k] = base_pos[k] + phase * (b.pos[k] - a.pos[k]);
    const float* base_nrm = interpolate ? a.nrm : b.nrm;
    for (int k = 0; k < 3; ++k) o.nrm[k] = base_nrm[k] + phase * (b.nrm[k] - a.nrm[k]);
    // Texture coordinates are not blended. Motion of a texture across a surface is done with a
    // texture matrix, which is advanced separately; a per-vertex coordinate that changed between
    // frames is a different sprite on the same quad: the next digit of the timer, the next frame
    // of a flipbook. Blending those rolled the timer's hundredths digit up and down between two
    // glyphs on every presented frame, which is the shaking timer. `o = b` above already carries
    // the current frame's coordinates.
    for (int k = 0; k < 4; ++k) {
      const int base0 = interpolate ? a.col0[k] : b.col0[k], base1 = interpolate ? a.col1[k] : b.col1[k];
      o.col0[k] = (uint8_t)std::min(255.0f, std::max(0.0f, base0 + phase * ((int)b.col0[k] - (int)a.col0[k])));
      o.col1[k] = (uint8_t)std::min(255.0f, std::max(0.0f, base1 + phase * ((int)b.col1[k] - (int)a.col1[k])));
    }
  }
  return true;
}

namespace {
// Persistent workers for the authored solver: every presented frame re-poses ~1000 draws, which
// are independent apart from the per-chain sample cache (kept per chunk; draws of one object are
// contiguous so chains rarely repeat across chunks).
class SolverPool {
 public:
  explicit SolverPool(int workers) {
    for (int i = 0; i < workers; ++i) threads_.emplace_back([this, i] { loop(i + 1); });
  }
  ~SolverPool() {
    { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  int chunks() const { return (int)threads_.size() + 1; }
  // Runs fn(chunk) for chunk in [0, chunks()); the caller executes chunk 0.
  void run(const std::function<void(int)>& fn) {
    { std::lock_guard<std::mutex> lk(m_); job_ = &fn; pending_ = (int)threads_.size(); ++generation_; }
    cv_.notify_all();
    fn(0);
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return pending_ == 0; });
    job_ = nullptr;
  }
 private:
  void loop(int chunk) {
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(int)>* job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
        if (quit_) return;
        seen = generation_;
        job = job_;
      }
      (*job)(chunk);
      std::lock_guard<std::mutex> lk(m_);
      if (--pending_ == 0) done_.notify_one();
    }
  }
  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable cv_, done_;
  const std::function<void(int)>* job_ = nullptr;
  int pending_ = 0;
  uint64_t generation_ = 0;
  bool quit_ = false;
};
SolverPool& solver_pool() { static SolverPool pool(3); return pool; }
}  // namespace

void SubFrameSolver::build(double t, bool interpolate, std::vector<DrawMatrices>& out, bool authored) const {
  out.resize(cur_ ? cur_->draws.size() : 0);
  if (!cur_) return;
  SubFrameStats* stats = &stats_;
  stats->rigid = stats->blended = stats->cuts = stats->authored = stats->carried = stats->vertex_blended = 0;
  if (authored) {
    // Sample forward from the latest state. Unsupported/discontinuous draws hold their current
    // matrices instead of inventing motion or adding a frame of delay. Chunks run in parallel.
    // Stage geometry (ground, platforms, bushes, backdrops) does not move in the world: only the
    // camera does. Re-posing each of those objects on its own gave neighbouring layers of the same
    // surface slightly different positions between ticks, so coplanar faces fought over depth: the
    // Yoshi's Story ground flashed black and looked see-through, and bushes flickered. One identical
    // camera transform for every unskinned draw keeps the whole stage rigid together. Skinned
    // characters keep their own animation sampling, which is where sub-frame motion matters.
    constexpr bool kSampleRigidObjects = true;   // stage objects keep their own sub-frame motion
    const uint32_t diag = subframe_diag();
    const bool sample_rigid = kSampleRigidObjects && !(diag & 1);
    const size_t n = cur_->draws.size();
    set_authored_interpolate(interpolate);
    SolverPool& pool = solver_pool();
    const int chunks = n >= 128 ? pool.chunks() : 1;
    std::vector<uint32_t> counts((size_t)chunks, 0), carries((size_t)chunks, 0), blends((size_t)chunks, 0), stages((size_t)chunks, 0);
    // A presented frame at phase 1 in Interpolate is the current simulation frame itself, so every
    // draw must leave the solver holding exactly the matrices the game loaded. Whatever does not is
    // what jumps once per tick. Checked only on those frames.
    const bool endpoint_check = interpolate && t >= 1.0;
    // The camera's own step this tick, cur_view * inverse(prev_view). A draw that did not move in
    // the world has a model-view matrix that changed by exactly this and nothing else, which is how
    // the stage is told apart from the things standing on it without knowing anything about either.
    // Audit run only (MELEE_AUDIT_STAGE_SPLIT in the environment): measures how far the old routes
    // put a static draw from the single camera transform, per presented frame.
    const bool audit = audit_stage_split();
    const bool menus = menu_mode_;
    routes_.assign(n, 0);
    std::vector<int> audit_row;
    std::vector<float> audit_expect;
    std::vector<uint32_t> audit_counts((size_t)chunks, 0), audit_splits((size_t)chunks, 0);
    std::vector<float> audit_worsts((size_t)chunks, 0.0f);
    if (audit) { audit_row.assign(n, -1); audit_expect.assign(n * 3, 0.0f); }
    // The sub-frame camera transform, built once and shared by every static draw in this frame.
    float stage_carry[12], stage_carry_inv[12];
    bool stage_carry_has_inverse = false, have_stage_carry = false;
    if (camera_previous_ && camera_current_)
      have_stage_carry = camera_sub_frame_carry(*camera_previous_, *camera_current_, t, stage_carry, stage_carry_inv, stage_carry_has_inverse);
    Affine camera_step{};
    bool have_camera_step = false;
    // The camera moved this tick but the sub-frame view is being held at the current frame: a quake
    // (the view jolts back and forth, and advancing along a jolt overshoots it) or a cut. Every
    // route that takes its view from the camera then draws the current frame's view, so the one
    // route that blends model-view rows, which carry the camera inside them, has to do the same or
    // it alone slides toward the previous frame while everything around it holds. On Yoshi's Story
    // that was the tree: its branches, blended, half a shake away from its foliage, held, on the
    // frame a fighter was hit. Seen in pinned captures at the exact frames of the hits.
    bool camera_holds = false;
    if (camera_previous_ && camera_current_ && camera_previous_->has_view && camera_current_->has_view) {
      Affine prev_view{}, cur_view{}, inv_prev{};
      std::memcpy(prev_view.m, camera_previous_->view.data(), sizeof prev_view.m);
      std::memcpy(cur_view.m, camera_current_->view.data(), sizeof cur_view.m);
      if (invert(prev_view, inv_prev)) { camera_step = mul(cur_view, inv_prev); have_camera_step = true; }
      camera_holds = !menus && !have_stage_carry && std::memcmp(prev_view.m, cur_view.m, sizeof prev_view.m) != 0;
    }
    std::vector<uint32_t> checked((size_t)chunks, 0), offs((size_t)chunks, 0);
    std::vector<float> worsts((size_t)chunks, 0.0f);
    std::vector<uint64_t> worst_ids((size_t)chunks, 0);
    auto work = [&](int chunk) {
      AuthoredCache chain_cache;   // one sampled chain per object per chunk per presented frame
      size_t begin = n * (size_t)chunk / (size_t)chunks, end = n * (size_t)(chunk + 1) / (size_t)chunks;
      uint32_t count = 0, carried = 0, blended = 0, staged = 0;
      uint32_t lagged = 0, worst_kind = 0, worst_verts = 0; float worst_err = 0;
      for (size_t i = begin; i < end; ++i) {
        const DrawCall& d = cur_->draws[i];
        DrawMatrices& o = out[i];
        const Pair& p = pairs_[i];
        const DrawCall* pd = p.prev_draw >= 0 ? &prev_->draws[p.prev_draw] : nullptr;
        // Draws that cannot be sampled hold: at the current pose when predicting, at the previous
        // pose when interpolating, so every object stays on the same timeline.
        const DrawCall& hold = (interpolate && pd) ? *pd : d;
        std::memcpy(o.pos, hold.posMatrices, sizeof o.pos);
        std::memcpy(o.nrm, hold.normalMatrices, sizeof o.nrm);
        o.vertices = nullptr;
        if (!pd) {
          // No pair at all (stage bushes and similar rewrite their geometry every frame). Leaving
          // these at their exact current pose while every paired draw is shown between frames puts
          // them on a different timeline, which is visible as flicker against a moving camera.
          // The camera carry of this frame moves them with everything else.
          // Never the HUD. An orthographic draw (xf_regs[0x26]) is in screen space: the timer, the
          // percentages, the stock icons and the magnifier do not sit in the world and must not be
          // moved by the camera. They are unpaired by design (see the hud counter above), so without
          // this they fell into the carry below and were shifted a fraction of the camera's motion
          // on every presented frame, which reads as the HUD juddering while the game is smooth.
          // The carry does not need this draw's own pose: it uses the frame's camera, which is
          // already passed in below. Requiring an authored pose meant every draw the observer never
          // captured got no carry at all, and Yoshi's Story logs 51,115 of those per interval. Each
          // one kept its exact matrices while the rest of the scene advanced with the camera, so it
          // drifted by the full camera delta, and the drift swings with the sub-frame phase: on
          // screen that is the stage sliding against itself, worst while the camera moves, in both
          // sub-frame modes, and absent at a locked frame rate because there are no sub-frames.
          // A draw that knows which camera it was rendered with is only carried by that camera.
          // Melee draws its HUD through a camera of its own, perspective rather than orthographic,
          // so the test above does not catch it. The timer's hundredths digits change texture every
          // tick, never pair, and so landed here to be carried by the WORLD camera's motion: each
          // digit was drawn up to a unit away from where the HUD camera, which never moves, has it,
          // and swung with the phase. That is the shaking timer. A draw with no pose at all still
          // gets the carry, since nearly all of those are stage scenery on the main camera.
          const bool own_camera_matches = menus || !d.authored_pose || !d.authored_pose->has_view ||
                                          (camera_current_ && d.authored_pose->view == camera_current_->view);
          if (camera_previous_ && camera_current_ && d.xf_regs[0x26] == 0 && own_camera_matches) {
            uint64_t slots = 0;
            if (d.components & VB_HAS_POSMTXIDX) {
              for (uint32_t v = 0; v < d.vertex_count; ++v) {
                const uint32_t idx = cur_->vertices[d.first_vertex + v].posmtx;
                if (idx < 64) slots |= 1ull << idx;
              }
            } else {
              slots |= 1ull << (d.matrix_index_a & 63);
            }
            // `hold` is this frame's draw (there is no pair), so its matrices embed the current
            // frame's view: that, not the mode, is what the carry has to cancel.
            if (slots && carry_camera(*camera_previous_, *camera_current_, t, *camera_current_, hold.posMatrices, hold.normalMatrices, slots, o.pos, o.nrm)) ++carried;
          }
          routes_[i] = 5;
          continue;
        }
        // The previous frame's matrices as this draw sees them. Normally the previous draw's own
        // arrays; when its slots moved (see SlotRemap) the rows are gathered from the old slots into
        // the new ones, so every path below that reads "the previous row" reads the right object.
        const float* held_pos = hold.posMatrices;
        const float* held_nrm = hold.normalMatrices;
        const float* prev_pos = pd->posMatrices;
        const float* prev_nrm = pd->normalMatrices;
        float remapped_prev_pos[256], remapped_prev_nrm[96], remapped_held_pos[256], remapped_held_nrm[96];
        if (p.blend_vertices) {
          SlotRemap remap;
          if ((diag & 2) || p.blend_offset + d.vertex_count > vertex_blend_.size() ||
              !blend_vertex_stream(&prev_->vertices[pd->first_vertex], &cur_->vertices[d.first_vertex], d.vertex_count,
                                   t, interpolate, max_translation, &vertex_blend_[p.blend_offset], remap,
                                   d.components, d.xf_regs[0x3F] & 15, menus)) {
            // The stream cannot be blended, so the current one is drawn as it is, but the
            // matrices still advance below with everything else. Holding the whole draw at its
            // current frame put it on a timeline of its own, and a menu element that is rebuilt on
            // some ticks and not others then alternated between the current frame and the frame in
            // between. Alternating between two treatments is what the eye reads as flicker, so
            // every draw gets the same one.
            routes_[i] = 6;
            remap.moved = false;   // a rejected stream may have filled the map only part way
            // In a match the current stream is drawn as it is while the matrices advance below with
            // everything else. In the menus the whole draw holds its own current frame, which is
            // what every rebuilt draw did before the stage work: menu text and panels are rebuilt
            // rather than moved, and what matters there is that they are all treated the same way
            // on every tick. One of them blending while the next holds is what flickers.
            if (menus) {
              std::memcpy(o.pos, d.posMatrices, sizeof o.pos);
              std::memcpy(o.nrm, d.normalMatrices, sizeof o.nrm);
              continue;
            }
          } else {
            o.vertices = &vertex_blend_[p.blend_offset];
            ++blended;
          }
          if (remap.moved) {
            std::memcpy(remapped_prev_pos, pd->posMatrices, sizeof remapped_prev_pos);
            std::memcpy(remapped_prev_nrm, pd->normalMatrices, sizeof remapped_prev_nrm);
            for (int row = 0; row < 64; ++row) {
              const int from = remap.pos_map[row] >= 0 ? remap.pos_map[row] : remap.tex_map[row];
              if (from < 0 || from == row || row + 3 > 64 || from + 3 > 64) continue;
              std::memcpy(&remapped_prev_pos[row * 4], &pd->posMatrices[from * 4], 12 * sizeof(float));
              if (row + 3 <= 32 && from + 3 <= 32) std::memcpy(&remapped_prev_nrm[row * 3], &pd->normalMatrices[from * 3], 9 * sizeof(float));
            }
            prev_pos = remapped_prev_pos; prev_nrm = remapped_prev_nrm;
            if (interpolate) {
              // The held pose is the previous frame, so it moves with the remap too.
              std::memcpy(remapped_held_pos, remapped_prev_pos, sizeof remapped_held_pos);
              std::memcpy(remapped_held_nrm, remapped_prev_nrm, sizeof remapped_held_nrm);
              held_pos = remapped_held_pos; held_nrm = remapped_held_nrm;
              std::memcpy(o.pos, held_pos, sizeof o.pos);
              std::memcpy(o.nrm, held_nrm, sizeof o.nrm);
            }
          }
        }
        // Texture-coordinate matrices animate independently of geometry: scrolling skies, water and
        // backdrops are driven entirely by them, and they used to advance only once per simulation
        // frame, which left most of the moving image stepping at 60 Hz. They are not rigid
        // transforms, so they advance by a straight per-element blend; a large jump holds.
        auto sample_textures = [&] {
          if (diag & 8) return;
          // A different starting row can still overlap a position matrix. Never
          // overwrite any of its rows with an independently sampled UV transform.
          const uint64_t position_rows = p.pos_slots | (p.pos_slots << 1) | (p.pos_slots << 2);
          for (int row = 0; row + 3 <= 64; ++row) {
            if (!(p.tex_slots & (1ull << row)) || (position_rows & (7ull << row))) continue;
            const float* previous_row = &prev_pos[row * 4];
            const float* current_row = &d.posMatrices[row * 4];
            const float* base_row = &held_pos[row * 4];
            bool continuous = true;
            for (int k = 0; k < 12; ++k) {
              const float delta = current_row[k] - previous_row[k];
              if (!std::isfinite(delta) || std::abs(delta) > 16.0f) { continuous = false; break; }
            }
            if (!continuous) { std::memcpy(&o.pos[row * 4], current_row, 12 * sizeof(float)); continue; }
            for (int k = 0; k < 12; ++k) o.pos[row * 4 + k] = base_row[k] + (float)t * (current_row[k] - previous_row[k]);
          }
        };
        // The stage, and everything else standing still in the world, moves on screen for one
        // reason only: the camera. Such a draw's model-view matrix changed by exactly the camera's
        // step this tick, which is what is tested here, row by row.
        //
        // Every one of them must then be advanced by ONE transform, the same one, or coplanar
        // surfaces separate. Yoshi's Story's ground, its bushes and its background are built from
        // layers of geometry that sit on each other, and the solver was free to reach each layer by
        // a different route: one draw sampled its own joint chain, the next failed on a joint and
        // blended its matrix rows, a third fell back to the camera carry. All three land in the same
        // place at phase 0 and phase 1, which is why the endpoint check finds nothing, but in
        // between they disagree by a fraction of a unit, and a fraction of a unit is all it takes
        // for two coplanar faces to swap which one is in front. That is the flicker: not geometry
        // in the wrong place, but neighbouring layers of the same surface fighting over depth on
        // the frames between ticks. It needs a moving camera to show, it needs sub-frame animation
        // to exist at all, and it is worst on the stages built in layers (Yoshi's Story, Fountain
        // of Dreams, Dream Land) while Battlefield and Final Destination stay clean.
        //
        // Routing all of them through the camera carry gives the whole static world a single rigid
        // transform per presented frame. Anything that is actually moving is untouched by this and
        // keeps its own sampled motion.
        if (have_camera_step && p.pos_slots && !menus && !(diag & 32)) {
          bool static_in_world = true;
          uint64_t slots = p.pos_slots;
          while (slots && static_in_world) {
            const unsigned long bit = (unsigned long)__builtin_ctzll(slots);
            slots &= slots - 1;
            const int row = (int)bit;
            if (row + 3 > 64) { static_in_world = false; break; }
            // Row by row and element by element, stopping at the first difference: almost every
            // draw that is not part of the static stage fails on the first number tested.
            const float* was = &prev_pos[row * 4];
            const float* now = &d.posMatrices[row * 4];
            for (int r = 0; r < 3 && static_in_world; ++r)
              for (int c = 0; c < 4; ++c) {
                float a = camera_step.m[r * 4 + 0] * was[c] + camera_step.m[r * 4 + 1] * was[4 + c] + camera_step.m[r * 4 + 2] * was[8 + c];
                if (c == 3) a += camera_step.m[r * 4 + 3];
                const float b = now[r * 4 + c];
                // Tight on purpose. This only has to absorb the rounding of one matrix multiply in
                // single precision, about a millionth of the magnitude. Anything looser starts
                // calling slowly moving scenery static, and these are exactly the stages that have
                // it: Fountain of Dreams' platforms rise a fraction of a unit per tick, Randall
                // crawls across Yoshi's Story, Whispy's leaves sway. Locking those to the camera
                // would stop their sub-frame motion and step them once per tick instead.
                if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > 1e-5f * (1.0f + std::abs(b))) { static_in_world = false; break; }
              }
          }
          if (static_in_world && audit) {
            // Audit: work out where the single camera transform puts this draw, then let the old
            // routes run anyway and compare at the end. The gap between them is how far apart two
            // pieces of the same surface are drawn mid-interval, which is the quantity that decides
            // whether coplanar faces fight over depth.
            float scratch_pos[256]; float scratch_nrm[96];
            std::memcpy(scratch_pos, d.posMatrices, sizeof scratch_pos);
            std::memcpy(scratch_nrm, d.normalMatrices, sizeof scratch_nrm);
            if (carry_camera(*camera_previous_, *camera_current_, t, *camera_current_, d.posMatrices, d.normalMatrices,
                             p.pos_slots, scratch_pos, scratch_nrm)) {
              const unsigned long bit = (unsigned long)__builtin_ctzll(p.pos_slots);
              const int row = (int)bit;
              if (row + 3 <= 64) {
                audit_row[i] = row;
                for (int r = 0; r < 3; ++r) audit_expect[i * 3 + r] = scratch_pos[row * 4 + r * 4 + 3];
              }
            }
          } else if (static_in_world) {
            ++staged;
            if (have_stage_carry &&
                apply_carry(stage_carry, stage_carry_has_inverse ? stage_carry_inv : nullptr, d.posMatrices, d.normalMatrices,
                            p.pos_slots, o.pos, o.nrm)) {
              ++carried;
            } else {
              // No sub-frame camera motion to apply (a quake or a cut holds the view): then the
              // matrices the game loaded are the answer, for this draw and for its neighbours.
              std::memcpy(o.pos, d.posMatrices, sizeof o.pos);
              std::memcpy(o.nrm, d.normalMatrices, sizeof o.nrm);
            }
            routes_[i] = 4;
            sample_textures();
            continue;
          }
        }
        // A position matrix advanced between the two frames without an authored pose to sample. It
        // used to be blended element by element, which is fine for a translation and wrong for a
        // rotation: halfway between two orientations, the average of the two matrices is a squashed
        // matrix, not a turned one, and for a large turn it collapses toward zero. Yoshi's Story's
        // tree sways on joints the observer does not capture, so its branches went through exactly
        // that once a cycle, drawn flattened and thrown out through the foliage for one presented
        // frame (seen in captures, on the frame between two correct ones). The rigid path used by
        // the matrix modes takes the turn as a turn, and holds when the change is not a rigid one.
        // Returns whether the row was moved; a discontinuity leaves the held pose in place.
        // A position matrix advanced between the two frames without an authored pose to sample:
        // a straight per-element blend of the rows, exact at both ends. The rigid (screw) blend the
        // matrix modes use was tried here and broke the menus, whose panels animate with
        // non-uniform scale and skew that a rigid decomposition cannot represent. Returns whether the
        // row was moved; a discontinuity leaves the held pose in place.
        auto blend_row = [&](int row) -> bool {
          if (!interpolate) return false;
          const float* previous_row = &prev_pos[row * 4];
          const float* current_row = &d.posMatrices[row * 4];
          const bool has_normals = row < 32;
          if (camera_holds) {
            std::memcpy(&o.pos[row * 4], current_row, 12 * sizeof(float));
            if (has_normals) std::memcpy(&o.nrm[row * 3], &d.normalMatrices[row * 3], 9 * sizeof(float));
            return true;
          }
          for (int k = 0; k < 12; ++k) {
            const float delta = current_row[k] - previous_row[k];
            if (!std::isfinite(delta) || std::abs(delta) > max_translation) return false;
          }
          for (int k = 0; k < 12; ++k) o.pos[row * 4 + k] = previous_row[k] + (float)t * (current_row[k] - previous_row[k]);
          if (has_normals) {
            const float* pn = &prev_nrm[row * 3];
            const float* cn = &d.normalMatrices[row * 3];
            bool n_ok = true;
            for (int k = 0; k < 9; ++k) if (!std::isfinite(cn[k] - pn[k])) { n_ok = false; break; }
            if (n_ok) for (int k = 0; k < 9; ++k) o.nrm[row * 3 + k] = pn[k] + (float)t * (cn[k] - pn[k]);
          }
          return true;
        };
        if (!d.authored_pose || !pd->authored_pose) {
          // No authored pose, but the draw is paired, so its position matrices can still be advanced
          // between the two frames the same way the texture matrices are. Without this they were left
          // at the held pose while everything around them moved, so any stage geometry the observer
          // does not capture but the game does move sat a whole frame behind and snapped forward once
          // per simulation frame: Yoshi's Story's trees, Fountain of Dreams' rising platforms, the
          // stage itself. At a high presented frame rate that is a 60 Hz stutter on exactly those
          // objects while the rest of the scene is smooth, which is what the blinking is.
          // Bounded like the texture path: a jump larger than a plausible frame of motion holds,
          // so a matrix reused for something else cannot drag geometry across the screen.
          // Interpolate only. There the base pose is the previous frame, so advancing by the delta
          // lands exactly on the current pose at phase 1: no overshoot is possible. In Predict the
          // base is the current frame, so the same arithmetic would extrapolate past it, and for a
          // matrix with no authored meaning that can throw geometry anywhere. Predict keeps holding
          // the latest pose, which is what the unit test pins.
          uint64_t slots = interpolate ? p.pos_slots : 0;
          while (slots) {
            const unsigned long bit = (unsigned long)__builtin_ctzll(slots);
            slots &= slots - 1;
            const int row = (int)bit;
            if (row + 3 > 64) continue;
            blend_row(row);
          }
          ++count;
          routes_[i] = 2;
          sample_textures();
          continue;
        }
        bool posed = false;
        if (d.authored_pose->envelope) {
          posed = !(diag & 16) && sample_authored_envelope(*pd->authored_pose, *d.authored_pose, t, d.posMatrices, d.normalMatrices, o.pos, o.nrm, &chain_cache);
        } else if (sample_rigid && !(d.components & VB_HAS_POSMTXIDX)) {
          // One chain, so one position matrix: the draw's own row, not necessarily row 0. Stage and
          // effect geometry commonly sits at a higher index and used to be skipped outright.
          const uint32_t row = d.matrix_index_a & 63;
          if (row + 3 <= 64) {
            const bool has_normals = row + 3 <= 32;
            static const float identity_normals[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            float sampled_pos[12], sampled_nrm[9];
            if (sample_authored(*pd->authored_pose, *d.authored_pose, t, &d.posMatrices[row * 4], sampled_pos, sampled_nrm,
                                has_normals ? &d.normalMatrices[row * 3] : identity_normals, &chain_cache)) {
              std::memcpy(&o.pos[row * 4], sampled_pos, sizeof sampled_pos);
              if (has_normals) std::memcpy(&o.nrm[row * 3], sampled_nrm, sizeof sampled_nrm);
              posed = true;
            }
          }
        }
        routes_[i] = posed ? 1 : 0;
        if (posed) ++count;
        else {
          // The re-pose failed. Carrying only the camera leaves the object at the PREVIOUS frame's
          // own pose, so at phase 1, where the presented frame is the current simulation frame, it is
          // a whole frame behind: measured at up to 301 draws per presented frame and 96 world units
          // out. As the phase sweeps 0 to 1 each simulation frame that object swings against
          // everything around it, sixty times a second, which is the stage flicker.
          //
          // posMatrices are model-view, so blending them between the two frames carries the object's
          // own motion and the camera's together, and lands exactly on the current pose at phase 1.
          // Interpolate only: in Predict the base is the current frame and the same arithmetic would
          // extrapolate past it. Bounded, so a matrix reused for something else holds instead.
          bool advanced = false;
          if (interpolate && !(diag & 4)) {
            uint64_t slots = p.pos_slots;
            while (slots) {
              const unsigned long bit = (unsigned long)__builtin_ctzll(slots);
              slots &= slots - 1;
              const int row = (int)bit;
              if (row + 3 > 64) continue;
              if (blend_row(row)) advanced = true;
            }
          }
          if (advanced) { ++carried; routes_[i] = 2; }
          else if (carry_camera(*pd->authored_pose, *d.authored_pose, t, *hold.authored_pose, held_pos, held_nrm, p.pos_slots, o.pos, o.nrm)) { ++carried; routes_[i] = 3; }
        }
        // Sanity: a re-posed or carried draw must land near where it was. Nothing downstream checks
        // this, so a single bad matrix (a chain that reconstructed wrong, a camera carry taken
        // across a cut or a sharp zoom) puts an object somewhere else entirely for one presented
        // frame. On screen that is stage geometry vanishing and coming back: the blink. If it
        // happens, hold this draw instead, which is always a safe answer because it is what the
        // simulation itself last produced.
        {
          uint64_t slots = p.pos_slots;
          bool sane = true;
          while (slots && sane) {
            const unsigned long bit = (unsigned long)__builtin_ctzll(slots);
            const int row = (int)bit;
            slots &= slots - 1;
            if (row + 3 > 64) continue;
            for (int r = 0; r < 3 && sane; ++r) {
              const float have = o.pos[row * 4 + r * 4 + 3], was = held_pos[row * 4 + r * 4 + 3];
              if (!std::isfinite(have) || std::abs(have - was) > kMaxCarryTranslation) sane = false;
            }
          }
          if (!sane) {
            ++stats_.insane;
            routes_[i] = 7;
            std::memcpy(o.pos, held_pos, sizeof o.pos);
            std::memcpy(o.nrm, held_nrm, sizeof o.nrm);
          }
        }
        // Envelope sampling publishes a complete matrix array. Apply disjoint UV
        // animation afterwards so that publication cannot erase the sampled UVs.
        sample_textures();
      }
      // Every draw of this chunk, whichever route it took above, including the ones that took an
      // early exit: at phase 1 the answer is known, so anything that differs from it jumps once per
      // simulation frame.
      if (endpoint_check)
        for (size_t i = begin; i < end; ++i) {
          const DrawCall& d = cur_->draws[i];
          const DrawMatrices& o = out[i];
          uint64_t slots = pairs_[i].pos_slots ? pairs_[i].pos_slots : (1ull << (d.matrix_index_a & 63));
          float worst = 0;
          while (slots) {
            const unsigned long bit = (unsigned long)__builtin_ctzll(slots);
            slots &= slots - 1;
            const int row = (int)bit;
            if (row + 3 > 64) continue;
            for (int r = 0; r < 3; ++r) {
              const float err = std::abs(o.pos[row * 4 + r * 4 + 3] - d.posMatrices[row * 4 + r * 4 + 3]);
              if (std::isfinite(err) && err > worst) worst = err;
            }
          }
          ++checked[(size_t)chunk];
          if (worst > 0.01f) {
            ++offs[(size_t)chunk];
            if (worst > worsts[(size_t)chunk]) { worsts[(size_t)chunk] = worst; worst_ids[(size_t)chunk] = d.identity; }
          }
        }
      if (audit)
        for (size_t i = begin; i < end; ++i) {
          if (audit_row[i] < 0) continue;
          const int row = audit_row[i];
          float worst = 0;
          for (int r = 0; r < 3; ++r) {
            const float err = std::abs(out[i].pos[row * 4 + r * 4 + 3] - audit_expect[i * 3 + r]);
            if (std::isfinite(err) && err > worst) worst = err;
          }
          ++audit_counts[(size_t)chunk];
          if (worst > 0.0005f) ++audit_splits[(size_t)chunk];
          if (worst > audit_worsts[(size_t)chunk]) audit_worsts[(size_t)chunk] = worst;
        }
      counts[(size_t)chunk] = count;
      carries[(size_t)chunk] = carried;
      blends[(size_t)chunk] = blended;
      stages[(size_t)chunk] = staged;
    };
    if (chunks == 1) work(0); else pool.run(work);
    for (uint32_t c : counts) stats->authored += c;
    for (uint32_t c : carries) stats->carried += c;
    for (uint32_t c : blends) stats->vertex_blended += c;
    for (uint32_t c : stages) stats->stage_locked += c;
    // Two presented frames of the same simulation frame differ only in phase. A draw that takes one
    // route at one phase and another at the next is being drawn on two timelines inside a single
    // tick, which on screen is that object flashing several times per tick.
    if (last_build_phase_ >= 0 && last_posed_.size() == routes_.size()) {
      PhaseFlipStats& f = subframe_phase_flips();
      for (size_t i = 0; i < routes_.size(); ++i) {
        ++f.compared;
        if (routes_[i] == last_posed_[i]) continue;
        ++f.flipped;
        if (cur_->draws[i].authored_pose && cur_->draws[i].authored_pose->envelope) ++f.flipped_skinned;
      }
    }
    last_posed_ = routes_;
    last_build_phase_ = t;
    if (dump_sequence() && cur_->sequence == dump_sequence() && pair_reason_.size() == n) {
      if (FILE* f = std::fopen("subframe_dump.txt", "a")) {
        std::fprintf(f, "# sequence %llu phase %.3f interpolate %d camera_holds %d stage_carry %d: i identity gen verts posmtxidx reason route dist(out,prev) dist(out,cur) dist(prev,cur) tex0\n", (unsigned long long)cur_->sequence, t, (int)interpolate, (int)camera_holds, (int)have_stage_carry);
        for (size_t i = 0; i < n; ++i) {
          const DrawCall& d = cur_->draws[i];
          const Pair& p = pairs_[i];
          const DrawCall* pd = p.prev_draw >= 0 ? &prev_->draws[p.prev_draw] : nullptr;
          const int row = (int)(d.matrix_index_a & 63);
          auto dist = [&](const float* a, const float* b) { double s2 = 0; for (int r = 0; r < 3; ++r) { const double v = (double)a[row * 4 + r * 4 + 3] - b[row * 4 + r * 4 + 3]; s2 += v * v; } return std::sqrt(s2); };
          std::fprintf(f, "%zu %016llX %llu %u %d %d %d %.3f %.3f %.3f %08X\n", i, (unsigned long long)d.identity, (unsigned long long)d.object_generation, d.vertex_count,
                       (d.components & VB_HAS_POSMTXIDX) ? 1 : 0, pair_reason_[i], routes_[i],
                       pd ? dist(out[i].pos, pd->posMatrices) : -1.0, dist(out[i].pos, d.posMatrices), pd ? dist(pd->posMatrices, d.posMatrices) : -1.0,
                       d.textures[0].used && d.textures[0].data ? (unsigned)d.textures[0].data->hash : 0u);
          {
            const bool has_pose = d.authored_pose != nullptr;
            const bool has_view = has_pose && d.authored_pose->has_view;
            const bool same_view = has_view && camera_current_ && d.authored_pose->view == camera_current_->view;
            std::fprintf(f, "   pose %d view %d main_camera %d ortho %d envelope %d out_y %.2f prev_y %.2f cur_y %.2f blend_vertices %d\n",
                         (int)has_pose, (int)has_view, (int)same_view, (int)(d.xf_regs[0x26] != 0), (int)(has_pose && d.authored_pose->envelope),
                         out[i].pos[row * 4 + 7], pd ? pd->posMatrices[row * 4 + 7] : 0.0f, d.posMatrices[row * 4 + 7], (int)p.blend_vertices);
          }
          if (routes_[i] == 6 && pd) {
            // Why the vertex blend was rejected: selector changes and the largest position step.
            uint32_t selector = 0; float largest = 0; const uint32_t count = std::min(d.vertex_count, pd->vertex_count);
            for (uint32_t v = 0; v < count; ++v) {
              const Vertex& pv = prev_->vertices[pd->first_vertex + v]; const Vertex& cv = cur_->vertices[d.first_vertex + v];
              if (pv.posmtx != cv.posmtx || std::memcmp(pv.texmtx, cv.texmtx, sizeof cv.texmtx)) ++selector;
              for (int k = 0; k < 3; ++k) largest = std::max(largest, std::abs(cv.pos[k] - pv.pos[k]));
              if (v < 3) std::fprintf(f, "   v%u posmtx %u->%u tex %u,%u,%u,%u,%u,%u,%u,%u -> %u,%u,%u,%u,%u,%u,%u,%u\n", v, pv.posmtx, cv.posmtx,
                                      pv.texmtx[0], pv.texmtx[1], pv.texmtx[2], pv.texmtx[3], pv.texmtx[4], pv.texmtx[5], pv.texmtx[6], pv.texmtx[7],
                                      cv.texmtx[0], cv.texmtx[1], cv.texmtx[2], cv.texmtx[3], cv.texmtx[4], cv.texmtx[5], cv.texmtx[6], cv.texmtx[7]);
            }
            std::fprintf(f, "   blend rejected: prev verts %u cur verts %u selector changes %u largest step %.2f (limit %.1f) blend_offset ok %d\n",
                         pd->vertex_count, d.vertex_count, selector, largest, max_translation, (int)(p.blend_offset + d.vertex_count <= vertex_blend_.size()));
          }
        }
        std::fclose(f);
      }
    }
    if (audit) {
      StageSplitAudit& a = subframe_stage_split_audit();
      for (uint32_t c : audit_counts) a.checked += c;
      for (uint32_t c : audit_splits) a.split += c;
      for (float c : audit_worsts) a.worst = std::max(a.worst, c);
    }
    {
      EndpointStats& e = subframe_endpoint_stats();
      for (uint32_t c : checked) e.checked += c;
      for (uint32_t c : offs) e.off += c;
      for (size_t c = 0; c < worsts.size(); ++c)
        if (worsts[c] > e.worst) { e.worst = worsts[c]; e.worst_identity = worst_ids[c]; }
    }
    return;
  }
  for (size_t i = 0; i < cur_->draws.size(); ++i) {
    const DrawCall& d = cur_->draws[i];
    DrawMatrices& o = out[i];
    const Pair& p = pairs_[i];
    const DrawCall* pd = p.prev_draw >= 0 ? &prev_->draws[p.prev_draw] : nullptr;
    const DrawCall& base = (interpolate && pd) ? *pd : d;
    o.vertices = nullptr;
    std::memcpy(o.pos, base.posMatrices, sizeof o.pos);
    std::memcpy(o.nrm, base.normalMatrices, sizeof o.nrm);
    if (!pd) continue;
    for (int row = 0; row < 64; ++row) {
      if (!(p.used_slots & (1ull << row))) continue;
      if (row + 3 > 64) break;
      const float* prev_m = &pd->posMatrices[row * 4];
      const float* cur_m = &d.posMatrices[row * 4];
      // Normal matrix for pos index `row` lives at normalMatrices[row*3 ..] when row < 32.
      bool has_nrm = row < 32 && row + 3 <= 32;
      float nrm_out[9];
      fractional(prev_m, cur_m, t, interpolate, max_translation, max_rotation, &o.pos[row * 4],
                 has_nrm ? nrm_out : nullptr, has_nrm ? &d.normalMatrices[row * 3] : nullptr,
                 has_nrm ? &pd->normalMatrices[row * 3] : nullptr, stats);
      if (has_nrm) std::memcpy(&o.nrm[row * 3], nrm_out, sizeof nrm_out);
    }
  }
}

}  // namespace gx
