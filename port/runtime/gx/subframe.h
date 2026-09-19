// Sub-frame pose solver: builds per-draw transform overrides for a display frame that falls between
// two 60 Hz simulation frames. Presentation only; guest state is never touched.
//
// For each draw of the current frame that also existed in the previous frame (same identity, same
// vertex count and primitive), every position matrix slot the draw uses gets the delta transform
//   D = M_cur * inverse(M_prev)
// decomposed into rotation / uniform-ish scale / translation. The fraction t of that delta is applied
// on top of M_cur (extrapolation, predicted motion) or M_prev (interpolation, one frame of latency).
// Non-rigid deltas and detected cuts retain the latest pose. Experimental matrix modes
// use heuristic pairing; authored mode samples validated tracks forward and holds all
// unsupported draws at the latest pose. Guest object generations guard address reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "gx_core.h"
#include "authored_pose.h"

namespace gx {

struct SubFrameStats { uint32_t draws = 0, paired = 0, rigid = 0, blended = 0, cuts = 0; uint32_t missing = 0, hud = 0, state = 0, geometry = 0, projection = 0, state_register = 256, authored = 0, carried = 0, vertex_blended = 0;
  uint32_t skinned = 0;   // skinned (character model) draws in the current frame; zero on menus and stage select
  // Draws whose pairing outcome differs from the previous simulation frame. A total says how many
  // objects are held; this says how many are CHANGING between held and re-posed, which is what an
  // object flashing actually is: a steady hold is invisible, alternating is not.
  uint32_t pair_flips = 0;
  // Draws whose re-posed or carried matrices landed implausibly far from where they were, and were
  // held instead. Any non-zero value is geometry that would have been drawn in the wrong place.
  uint32_t insane = 0;
  // Draws found to be standing still in the world and therefore advanced by the camera alone, as one
  // rigid piece. Large on a stage seen with a moving camera; zero when the camera is still.
  uint32_t stage_locked = 0;
};

// The endpoint invariant. In Interpolate a presented frame at phase 1 IS the current simulation
// frame, so every draw must leave the solver with exactly the matrices the game loaded. A draw that
// does not is a draw that jumps at the simulation boundary, once every tick, which is what an object
// flickering is. Checked only on the frames that land on phase 1, so it costs nothing the rest of
// the time, and accumulated across the run so the presentation loop can print the change per second.
// `worst` is the largest position error in world units and `worst_identity` names the draw.
struct EndpointStats { uint64_t checked = 0, off = 0; float worst = 0; uint64_t worst_identity = 0; };
EndpointStats& subframe_endpoint_stats();

// Development measurement, filled only when MELEE_AUDIT_STAGE_SPLIT is in the environment. Counts
// the draws standing still in the world that the solver's other routes would have placed away from
// the one camera transform the rest of the static stage uses, and by how far in world units. That
// gap is what makes coplanar layers of a stage fight over depth on the frames between ticks.
struct StageSplitAudit { uint64_t checked = 0, split = 0; float worst = 0; };
// Draws whose route changed between two presented frames of the SAME simulation frame, where the
// phase is the only difference. Each one is an object drawn on one timeline and then the other
// inside a single tick.
struct PhaseFlipStats { uint64_t compared = 0, flipped = 0, flipped_skinned = 0; };
PhaseFlipStats& subframe_phase_flips();
StageSplitAudit& subframe_stage_split_audit();

class SubFrameSolver {
 public:
  // Thresholds for treating a per-frame delta as a discontinuity (world units / radians).
  // Screw-motion extrapolation of a 3x4 matrix `t` frames past `cur` given `prev` (t in [0,1]);
  // holds `cur` at discontinuities. Used for camera motion by the authored path.
  static void extrapolate_matrix(const float prev[12], const float cur[12], double t, float out[12]);
  static void interpolate_matrix(const float prev[12], const float cur[12], double t, float out[12]);   // between prev and cur
  float max_translation = 40.0f;
  float max_rotation = 1.2f;

  // Pairs draws of `cur` with `prev` and caches the per-slot deltas. Both frames must stay alive
  // until the next call.
  void set_frames(const Frame* prev, const Frame* cur);
  // Fills `out` (resized to cur->draws.size()) for phase t in [0, 1]. Interpolate: pose between
  // prev (t = 0) and cur (t = 1). Extrapolate: pose t frames beyond cur.
  void build(double t, bool interpolate, std::vector<DrawMatrices>& out, bool authored = false) const;
  const SubFrameStats& stats() const { return stats_; }
  // Menus, character select, stage select and everything else that is not a running match. There
  // the solver sticks to the treatment that was in place before the stage and geometry work: no
  // blending of rebuilt vertex streams, no camera-locked static geometry, and a stream that cannot
  // be blended holds its own current matrices. The reason is consistency rather than accuracy. A
  // menu rebuilds its text and panels on some ticks and not others, so anything that treats a
  // rebuilt draw differently from a moved one makes that element alternate between two timelines
  // from tick to tick, and alternating is what the eye reads as flicker. Matrix-driven motion
  // (cursors, sliding panels, the rotating background) still gets full sub-frame motion.
  void set_menu_mode(bool on) { menu_mode_ = on; }

  // Exposed for tests: apply fraction t of the rigid/blended delta between prev and cur 3x4 matrices.
  static void fractional(const float prev[12], const float cur[12], double t, bool interpolate,
                         float max_translation, float max_rotation, float out_pos[12], float out_nrm[9],
                         const float cur_nrm[9], const float prev_nrm[9], SubFrameStats* stats);

 private:
  // bit i set: a 3x4 matrix starts at row i. Position and texture-coordinate matrices share the
  // array but are advanced differently, so they are tracked apart.
  struct Pair { int prev_draw; uint64_t used_slots; uint64_t pos_slots; uint64_t tex_slots; bool blend_vertices; size_t blend_offset; };
  const Frame* prev_ = nullptr;
  const Frame* cur_ = nullptr;
  std::vector<Pair> pairs_;
  mutable std::vector<Vertex> vertex_blend_;   // per presented frame: blended streams, one disjoint range per draw
  // Previous frame draws by identity, sorted; reused across simulation frames so pairing does not
  // allocate a hash node per draw every tick.
  std::vector<std::pair<uint64_t, int>> prev_index_;
  // Pairing outcome per draw identity, last simulation frame and this one, for pair_flips.
  std::unordered_map<uint64_t, bool> pair_history_, pair_seen_;
  // This frame's camera (from the first paired draw that carries a view): used to move draws that
  // have no pair onto the same timeline as the rest of the frame.
  const AuthoredPose* camera_previous_ = nullptr;
  const AuthoredPose* camera_current_ = nullptr;
  // Which draws were re-posed on the previous presented frame OF THE SAME simulation frame. The
  // phase is the only thing that changes between those frames, so a draw that is re-posed at one
  // phase and held at the next is being drawn on two different timelines within a single tick,
  // which is an object flashing several times per tick rather than once.
  mutable std::vector<uint8_t> last_posed_, routes_;
  std::vector<int> pair_reason_;   // development dump only
  mutable double last_build_phase_ = -1;
  bool menu_mode_ = false;
  mutable SubFrameStats stats_;
};

}  // namespace gx
