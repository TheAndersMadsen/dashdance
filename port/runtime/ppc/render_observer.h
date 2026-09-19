// Read-only render identity hooks around selected original guest functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
namespace ppc { struct Context; }
namespace gx {
enum class Observe { AllocateJoint, LoadJoint, ReleaseJoint, DisplayJoint, RigidMatrix, OtherMatrix, EnvelopeMatrix };
struct AuthoredPose;
class RenderObserver {
  ppc::Context& cpu_;
  Observe kind_;
  uint64_t saved_generation_ = 0, saved_pass_ = 0;
  uint32_t saved_draw_ = 0, saved_joint_ = 0;
  uint8_t* saved_memory_ = nullptr;
  uint8_t saved_owner_ = 0xFF;
  uint32_t loaded_joint_ = 0;   // JObjLoad: the joint, taken at entry
  bool saved_rigid_ = false, saved_envelope_ = false;
  std::shared_ptr<const AuthoredPose> saved_pose_;
 public:
  RenderObserver(ppc::Context& cpu, Observe kind, uint8_t* memory = nullptr);
  ~RenderObserver();
  RenderObserver(const RenderObserver&) = delete;
  RenderObserver& operator=(const RenderObserver&) = delete;
};
uint64_t observed_draw_identity(uint64_t fallback, uint64_t& generation);
void finish_observed_frame();
void set_authored_capture(bool enabled);
std::shared_ptr<const AuthoredPose> capture_authored_pose();
// Player slot (0..5) whose fighter is being rendered right now, or 0xFF for anything else. Off
// unless set_owner_tracking(true): resolving it costs a few guest reads per rendered object, and
// only the display-only fighter tint needs it. Reads guest memory, never writes it.
void set_owner_tracking(bool enabled);
uint8_t observed_owner();
// Whether the draw being recorded is skinned (SetupEnvelopeModelMtx). A fighter's model is skinned;
// its shadow and its effects are not, which is what separates the model from everything else drawn
// under the same player.
bool observed_skinned();
}
