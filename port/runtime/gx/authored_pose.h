// Immutable authored joint channels captured on the simulation thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PackedAnimation.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
namespace gx {
struct AuthoredPose;
// One HSD_RObj entry on a joint: a constraint HSD_RObjUpdateAll applies after the joint's own SRT.
// Only the reference types the sampler can evaluate are captured (see capture_robj_list); anything
// else declines the chain, so a constraint present here is one the evaluator knows how to run.
struct AuthoredConstraint {
  uint32_t flags = 0;                          // the RObj flags word: reference type, subtype, active bit
  uint64_t target_generation = 0;              // identity of the referenced HSD_JObj
  std::shared_ptr<const AuthoredPose> target;  // root..target chain, so the target re-poses with everything else
};
struct AuthoredJoint {
  uint64_t generation = 0;
  uint32_t flags = 0;
  uint32_t anim_flags = 0;             // HSD_AObj flags (AOBJ_LOOP and friends)
  std::array<float,3> scale{}, rotation{}, translation{};
  // JOBJ_USE_QUATERNION: HSD_JObj::rotate is a Quaternion, not Euler angles (jobj.h, +0x1C), and
  // the local matrix comes from MTXQuat instead of the sin/cos chain. Stage models use these
  // heavily, so without an evaluator none of that geometry could be re-posed.
  bool quaternion = false;
  std::array<float,4> quat{};   // x, y, z, w
  std::array<float,12> world{};
  float frame = 0, rate = 0, end = 0, rewind = 0;
  std::vector<NativeMelee::PackedTrack> tracks;
  std::vector<AuthoredConstraint> constraints;   // HSD_JObj::robj, empty for an unconstrained joint
};
// One bone of a skinned matrix slot: its joint chain, blend weight and inverse-bind (envelope) matrix.
struct AuthoredBone { std::shared_ptr<const AuthoredPose> chain; float weight = 1; std::array<float,12> envelope{}; };
struct AuthoredSlot { std::vector<AuthoredBone> bones; };
struct AuthoredPose {
  std::vector<AuthoredJoint> joints;   // rigid draws and bone chains: root..joint
  // Envelope (skinned) draws: view matrix, skeleton root transform and the weighted bones per matrix slot.
  bool envelope = false;
  bool has_view = false;               // `view` holds the draw's camera (view) matrix
  bool quake = false;                  // a camera quake was running when the view was captured
  std::array<float,12> view{};
  std::shared_ptr<const AuthoredPose> chain;   // rigid draws: the joint chain root..joint (shared per joint per frame)
  int right_kind = 0;                  // 0 none, 1 inverse(x.env), 2 inverse(x.world)*m.world, 3 inverse(x.world*x.env)*m.world
  std::array<float,12> right_envelope{};
  std::shared_ptr<const AuthoredPose> right_chain_m, right_chain_x;
  std::vector<AuthoredSlot> slots;
};
// Diagnostics: why capture/sampling declined a draw (indexed by rejection site; see the sources).
// Which HSD joint features actually cost us poses. capture[4] counts "this chain had a feature the
// capture path has no evaluator for", which lumps billboards, instances, quaternion joints, IK
// effectors, independent matrices and constraints into one number and so cannot be used to decide
// which evaluator to write first. These count each feature separately, and a joint carrying several
// is counted in each, so the totals are per feature rather than per joint.
// The robj_* entries break the constraint lists down by HSD_RObj reference type and constraint
// subtype (robj.h), counted per RObj entry rather than per joint, for the same reason: "this joint
// has a constraint" does not say which evaluator would pay for itself. The last four are the
// reasons a constraint this path does understand still could not be captured.
enum CaptureFeature {
  FEAT_BILLBOARD, FEAT_PBILLBOARD, FEAT_INSTANCE, FEAT_QUATERNION, FEAT_JOINT1, FEAT_JOINT2,
  FEAT_USER_DEF_MTX, FEAT_MTX_INDEP_PARENT, FEAT_MTX_INDEP_SRT, FEAT_ROBJ,
  FEAT_ROBJ_POSITION, FEAT_ROBJ_DIR, FEAT_ROBJ_UP, FEAT_ROBJ_ORIENT, FEAT_ROBJ_JOBJ_OTHER,
  FEAT_ROBJ_LIMIT, FEAT_ROBJ_IKHINT, FEAT_ROBJ_EXP, FEAT_ROBJ_INACTIVE, FEAT_ROBJ_UNKNOWN,
  FEAT_ROBJ_ANIMATED, FEAT_ROBJ_DEEP, FEAT_ROBJ_TARGET, FEAT_ROBJ_CYCLE,
  FEAT_COUNT
};
extern const char* const kCaptureFeatureNames[FEAT_COUNT];
struct AuthoredStats { std::atomic<uint32_t> capture[24]{}; std::atomic<uint32_t> sample[32]{}; std::atomic<uint32_t> feature[FEAT_COUNT]{};
  std::atomic<uint32_t> captured{0}, sampled{0};
  // Constraint work, so the evaluator can be seen running rather than inferred from a falling
  // rejection count: chains captured carrying an HSD_RObj, and constraints evaluated while sampling.
  std::atomic<uint32_t> robj_chains{0}, robj_applied{0}; };
AuthoredStats& authored_stats();
// Interpolate (exact in-betweens of the previous and current game frames, one frame late) instead
// of predicting ahead of the current frame. Set by the solver before sampling.
void set_authored_interpolate(bool on);
// Per-presented-frame cache of sampled joint chains: draws of one object share the chain.
struct AuthoredChain { bool ok = false; std::array<float,12> world{}, inverse_current{}; };
// `allow_static` changes the outcome for a chain with no animation, so it belongs in the key:
// without it, whichever draw reached the chain first decided whether every other draw of that
// object moved with the camera or held.
struct AuthoredChainKey {
  const AuthoredPose* previous; const AuthoredPose* current; bool allow_static;
  bool operator==(const AuthoredChainKey& o) const { return previous == o.previous && current == o.current && allow_static == o.allow_static; }
};
struct AuthoredPairHash { size_t operator()(const AuthoredChainKey& k) const {
  return (std::hash<const void*>()(k.previous) * 31u) ^ std::hash<const void*>()(k.current) ^ (k.allow_static ? 0x9e3779b9u : 0u); } };
using AuthoredCache = std::unordered_map<AuthoredChainKey, AuthoredChain, AuthoredPairHash>;
// Phase is [0,1] frames forward from current. Returns false at unsupported state or
// animation boundaries; callers must retain the current pose. Never calls guest code.
bool sample_authored(const AuthoredPose& previous, const AuthoredPose& current,
                     double phase, const float matrix[12], float result[12], float normals[9],
                     const float current_normals[9], AuthoredCache* cache = nullptr);
// Skinned draws: rebuilds every envelope matrix slot (and its normal matrix) at the fractional
// frame from the bones' sampled chains. `current_pos/current_nrm` are the draw's matrices (used
// to validate the reconstruction); outputs are the full 64-row position and 32-row normal arrays.
bool sample_authored_envelope(const AuthoredPose& previous, const AuthoredPose& current, double phase,
                              const float current_pos[256], const float current_nrm[96], float out_pos[256], float out_nrm[96],
                              AuthoredCache* cache);
// Applies only the camera's motion to a draw that could not be re-posed, so it still moves with a
// panning camera instead of holding for a whole simulation frame. `pos_slots` marks the rows that
// hold position matrices (texture-coordinate matrices share the array and must not be touched).
// `base` is the pose whose view `in_pos`/`in_nrm` were taken from: the previous frame for a draw
// that paired and is holding its previous matrices, the current frame for one that did not pair.
bool carry_camera(const AuthoredPose& previous, const AuthoredPose& current, double phase,
                  const AuthoredPose& base,
                  const float in_pos[256], const float in_nrm[96], uint64_t pos_slots,
                  float out_pos[256], float out_nrm[96]);
// The same camera transform built once for a whole presented frame, for the many draws that share
// it, and its application to one draw's matrices. `carry` takes a CURRENT-frame model-view matrix
// onto the sub-frame view; false means the view is not moving and the matrices already stand.
bool camera_sub_frame_carry(const AuthoredPose& previous, const AuthoredPose& current, double phase,
                            float carry[12], float carry_inverse[12], bool& has_inverse);
bool apply_carry(const float carry[12], const float carry_inverse[12],
                 const float in_pos[256], const float in_nrm[96], uint64_t pos_slots,
                 float out_pos[256], float out_nrm[96]);
}
