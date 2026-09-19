// Guest execution remains authoritative. These hooks own host metadata only.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "render_observer.h"
#include "ppc.h"
#include "gx_texture.h"
#include "authored_pose.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
namespace gx {
namespace {
std::unordered_map<uint32_t, uint64_t> joints;
std::unordered_map<uint64_t, uint32_t> passes;
// Joint chains captured this simulation frame (root..joint), shared by every draw that skins with them.
std::unordered_map<uint32_t, std::shared_ptr<const AuthoredPose>> chains_this_frame;
uint64_t next_generation = 1, current_generation = 0, current_pass = 0;
uint32_t current_draw = 0, current_joint = 0;
uint8_t* current_memory = nullptr;
bool rigid = false, envelope = false, authored_enabled = false;
uint32_t envelope_pobj = 0, envelope_vmtx = 0, rigid_vmtx = 0;
std::shared_ptr<const AuthoredPose> current_pose;
struct Reader {
  uint8_t* memory; bool valid = true;
  bool span(uint32_t a,uint32_t n) { if(a<ppc::RAM_BASE||a-ppc::RAM_BASE>ppc::RAM_SIZE||n>ppc::RAM_SIZE-(a-ppc::RAM_BASE))valid=false;return valid; }
  uint32_t word(uint32_t a) { if(!span(a,4))return 0;const auto* p=memory+a-ppc::RAM_BASE;return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
  float real(uint32_t a) { uint32_t v=word(a);float f;std::memcpy(&f,&v,4);return f; }
  bool matrix(uint32_t a, std::array<float,12>& m) { if(!span(a,48))return false; for(int k=0;k<12;++k)m[k]=real(a+4*k); return true; }
};
// Which player's fighter is being rendered right now, for the display-only fighter tint.
//
// HSD_GObj_804D7814 (0x804D7814) is the GObj whose render_cb is running: baselib/gobj.c's
// render_gobj saves it, sets it to the object it is about to render, and restores it. So inside
// HSD_JObjDisp it names the object that owns these draws. A fighter GObj's user_data is its
// Fighter, the Fighter points back at its own GObj, and player_slots[slot].player_entity[0] is
// that GObj, which gives the slot. Every one of those is a read; nothing here writes guest memory,
// which is the whole point of doing the tint here instead of in the fighter the way the Gecko code
// does. All three checks have to agree, so anything that is not a player's fighter resolves to
// 0xFF and is never tinted.
std::atomic<bool> owner_tracking{false};
uint32_t owner_cache_gobj = 0;
uint8_t owner_cache_player = 0xFF, current_owner = 0xFF;

uint8_t resolve_owner(uint8_t* memory) {
  Reader r{memory};   // its own reader: a miss here must not mark the pose capture's reader invalid
  const uint32_t gobj = r.word(0x804D7814);
  if (!gobj) return 0xFF;
  if (gobj == owner_cache_gobj) return owner_cache_player;
  uint8_t player = 0xFF;
  const uint32_t fighter = r.word(gobj + 0x2C);          // HSD_GObj::user_data
  if (fighter && r.word(fighter) == gobj) {              // Fighter::gobj points back at it
    for (uint32_t slot = 0; slot < 6; ++slot)            // StaticPlayer player_slots[6], stride 0xE90
      if (r.word(0x80453080 + slot * 0xE90 + 0xB0) == gobj) { player = (uint8_t)slot; break; }
  }
  owner_cache_gobj = gobj;
  owner_cache_player = player;
  return player;
}

// game_camera (0x80452C68): quake_frames_left[5] at +0x8C, quake_gobj at +0xA0. Camera_ApplyQuake
// clears quake_offset once it is applied, so the counters are what still show a shake at draw time.
bool camera_quaking(Reader& r) {
  const uint32_t camera = 0x80452C68;
  for(int k=0;k<5;++k) if(r.word(camera+0x8C+4*k)) return true;
  return r.word(camera+0xA0)!=0;
}

// HSD_RObj (robj.h): next +0x00, flags +0x04, union u +0x08 (jobj / limit f32 / ik_hint
// {bone_length f32, rotate_x f32} / exp {expr, rvalue, nb_args, is_bytecode}), aobj +0x18.
// The reference type is flags & 0x70000000 and the constraint subtype flags & 0x0FFFFFFF; bit
// 0x80000000 is the "active" bit HSD_RObjGetByType and HSD_RObjGetGlobalPosition both require.
constexpr uint32_t ROBJ_TYPE_MASK = 0x70000000u, ROBJ_SUBTYPE_MASK = 0x0FFFFFFFu, ROBJ_ACTIVE = 0x80000000u;
constexpr uint32_t REFTYPE_EXP = 0x00000000u, REFTYPE_JOBJ = 0x10000000u, REFTYPE_LIMIT = 0x20000000u,
                   REFTYPE_IKHINT = 0x40000000u;
// Constraint subtypes HSD_RObjUpdateAll asks for: 1 position, 2 direction, 3 up, 4 orientation.
constexpr uint32_t CNS_POSITION = 1, CNS_DIR = 2, CNS_UP = 3, CNS_ORIENT = 4;
// Only a position constraint is evaluated. In a 2600 frame Yoshi's Story match every one of the
// 44314 constraints that declined a chain was REFTYPE_JOBJ subtype 1 and active; direction, up,
// orientation, limit, IK hint and expression entries never appeared once. The rest keep declining
// with their own counter rather than being guessed at: an evaluator nothing exercises cannot be
// shown to reproduce the guest matrix, and would be dead code.
constexpr int kConstraintDepthLimit = 4;   // constraint targets whose own chains carry constraints
int constraint_depth = 0;
// Chains whose capture has not finished. A constraint that reaches back into one of them would be a
// cycle: HSD_RObjGetGlobalPosition resolves its target by calling the guest's HSD_JObjSetupMatrix,
// which this path must never do, so a cycle cannot be evaluated and the chain holds instead.
std::vector<uint32_t> chains_in_progress;

// Counts one RObj list by reference type and subtype. Diagnostics only: it decides which evaluators
// are worth writing, and keeps reporting the ones still declined once they are written.
void count_robj_list(Reader& r, uint32_t robj) {
  for(int guard=0; robj && guard<16; ++guard, robj=r.word(robj)) {
    if(!r.span(robj,0x1C)) { ++authored_stats().feature[FEAT_ROBJ_UNKNOWN]; return; }
    const uint32_t flags=r.word(robj+4), type=flags&ROBJ_TYPE_MASK, sub=flags&ROBJ_SUBTYPE_MASK;
    if(r.word(robj+0x18)) ++authored_stats().feature[FEAT_ROBJ_ANIMATED];   // an AObj drives the active bit
    if(type==REFTYPE_JOBJ) {
      // An inactive constraint contributes nothing at all: HSD_RObjGetGlobalPosition and
      // HSD_RObjGetByType both test the active bit before looking at the reference.
      if(!(flags&ROBJ_ACTIVE)) { ++authored_stats().feature[FEAT_ROBJ_INACTIVE]; continue; }
      if(sub==CNS_POSITION) ++authored_stats().feature[FEAT_ROBJ_POSITION];
      else if(sub==CNS_DIR) ++authored_stats().feature[FEAT_ROBJ_DIR];
      else if(sub==CNS_UP) ++authored_stats().feature[FEAT_ROBJ_UP];
      else if(sub==CNS_ORIENT) ++authored_stats().feature[FEAT_ROBJ_ORIENT];
      else ++authored_stats().feature[FEAT_ROBJ_JOBJ_OTHER];
    }
    else if(type==REFTYPE_LIMIT) ++authored_stats().feature[FEAT_ROBJ_LIMIT];
    else if(type==REFTYPE_IKHINT) ++authored_stats().feature[FEAT_ROBJ_IKHINT];
    // A bytecode expression has its type mask cleared at load (HSD_RObjLoadDesc), so both kinds of
    // expression arrive as REFTYPE_EXP and only the active ones are ever evaluated.
    else if(type==REFTYPE_EXP) { if(flags&ROBJ_ACTIVE) ++authored_stats().feature[FEAT_ROBJ_EXP]; else ++authored_stats().feature[FEAT_ROBJ_INACTIVE]; }
    else ++authored_stats().feature[FEAT_ROBJ_UNKNOWN];
  }
}

std::shared_ptr<const AuthoredPose> capture_chain(Reader& r, uint32_t address);

// Captures a joint's HSD_RObj list into `out`, with the chain of every referenced joint so the
// sampler can re-pose the target as well. Returns false, having counted the offending entry, when
// the list holds anything this path cannot evaluate; the caller then declines the whole chain.
// Read-only: it walks guest memory through the reader and never calls guest code.
bool capture_robj_list(Reader& r, uint32_t robj, std::vector<AuthoredConstraint>& out) {
  if(constraint_depth>=kConstraintDepthLimit){ ++authored_stats().feature[FEAT_ROBJ_DEEP]; return false; }
  for(int guard=0; robj && guard<16; ++guard, robj=r.word(robj)) {
    if(!r.span(robj,0x1C)){ ++authored_stats().feature[FEAT_ROBJ_UNKNOWN]; return false; }
    const uint32_t flags=r.word(robj+4), type=flags&ROBJ_TYPE_MASK, sub=flags&ROBJ_SUBTYPE_MASK;
    // An AObj on the RObj drives the active bit through RObjUpdateFunc, so the constraint can switch
    // on or off partway through the pair of frames being interpolated. None was ever observed.
    if(r.word(robj+0x18)){ ++authored_stats().feature[FEAT_ROBJ_ANIMATED]; return false; }
    if(type!=REFTYPE_JOBJ||!(flags&ROBJ_ACTIVE)||sub!=CNS_POSITION) {
      if(type==REFTYPE_JOBJ&&!(flags&ROBJ_ACTIVE)) ++authored_stats().feature[FEAT_ROBJ_INACTIVE];
      else if(type==REFTYPE_JOBJ&&sub==CNS_DIR) ++authored_stats().feature[FEAT_ROBJ_DIR];
      else if(type==REFTYPE_JOBJ&&sub==CNS_UP) ++authored_stats().feature[FEAT_ROBJ_UP];
      else if(type==REFTYPE_JOBJ&&sub==CNS_ORIENT) ++authored_stats().feature[FEAT_ROBJ_ORIENT];
      else if(type==REFTYPE_JOBJ) ++authored_stats().feature[FEAT_ROBJ_JOBJ_OTHER];
      else if(type==REFTYPE_LIMIT) ++authored_stats().feature[FEAT_ROBJ_LIMIT];
      else if(type==REFTYPE_IKHINT) ++authored_stats().feature[FEAT_ROBJ_IKHINT];
      else if(type==REFTYPE_EXP) ++authored_stats().feature[flags&ROBJ_ACTIVE?FEAT_ROBJ_EXP:FEAT_ROBJ_INACTIVE];
      else ++authored_stats().feature[FEAT_ROBJ_UNKNOWN];
      return false;
    }
    AuthoredConstraint c;
    c.flags=flags;
    const uint32_t target=r.word(robj+8);
    auto g=target?joints.find(target):joints.end();
    if(!target||g==joints.end()){ ++authored_stats().feature[FEAT_ROBJ_TARGET]; return false; }
    c.target_generation=g->second;
    ++constraint_depth;
    c.target=capture_chain(r,target);
    --constraint_depth;
    if(!c.target){ ++authored_stats().feature[FEAT_ROBJ_TARGET]; return false; }
    out.push_back(std::move(c));
  }
  if(robj){ ++authored_stats().feature[FEAT_ROBJ_UNKNOWN]; return false; }   // list longer than the guard
  return true;
}

// Captures the joint chain root..address (local SRT, world matrix, authored tracks, constraints).
// Returns null (and counts the reason) when any joint needs an evaluator this path does not have.
std::shared_ptr<const AuthoredPose> capture_chain(Reader& r, uint32_t address) {
  auto cached = chains_this_frame.find(address);
  if (cached != chains_this_frame.end()) return cached->second;
  for(uint32_t open : chains_in_progress) if(open==address){ ++authored_stats().feature[FEAT_ROBJ_CYCLE]; return {}; }
  auto pose=std::make_shared<AuthoredPose>();
  size_t byte_count=0;
  uint32_t start = address;
  // Unwound on every return, so a declined chain cannot leave the guard poisoned for the frame.
  struct OpenChain {
    explicit OpenChain(uint32_t a) { chains_in_progress.push_back(a); }
    ~OpenChain() { chains_in_progress.pop_back(); }
  } open_guard(address);
  bool constrained=false;
  while(address && pose->joints.size()<128) {
    if(!r.span(address,0x88)){ ++authored_stats().capture[2]; return {}; }
    AuthoredJoint j;
    auto g=joints.find(address); if(g==joints.end()){ ++authored_stats().capture[3]; return {}; } j.generation=g->second;
    j.flags=r.word(address+0x14)&~0x40u;
    // Billboards, instances, quaternion/IK and independent matrices require their own authored
    // evaluators, so retain exact captured draws. Constraints (HSD_RObj) no longer decline the chain
    // on sight: the list is captured below and the sampler declines only what it cannot evaluate.
    const uint32_t robj=r.word(address+0x80);
    if(j.flags & (0x2E00u|0x1000u|0x600000u|0x3800000u)){   // 0x20000 (quaternion) is evaluated below
      // Record which feature it was, not just that there was one, so the next evaluator to write is
      // chosen by what real matches actually use. A joint can carry several; count each.
      auto note=[&](bool hit,CaptureFeature f){ if(hit) ++authored_stats().feature[f]; };
      note(j.flags&0x0E00u,FEAT_BILLBOARD);        // JOBJ billboard field
      note(j.flags&0x2000u,FEAT_PBILLBOARD);       // JOBJ_PBILLBOARD
      note(j.flags&0x1000u,FEAT_INSTANCE);         // JOBJ_INSTANCE
      note(j.flags&0x20000u,FEAT_QUATERNION);      // JOBJ_USE_QUATERNION
      note(j.flags&0x200000u,FEAT_JOINT1);         // JOBJ_JOINT1
      note(j.flags&0x400000u,FEAT_JOINT2);         // JOBJ_JOINT2 (with JOINT1 the EFFECTOR mask)
      note(j.flags&0x800000u,FEAT_USER_DEF_MTX);   // JOBJ_USER_DEF_MTX
      note(j.flags&0x1000000u,FEAT_MTX_INDEP_PARENT);
      note(j.flags&0x2000000u,FEAT_MTX_INDEP_SRT);
      note(robj!=0,FEAT_ROBJ);                     // HSD_RObj: constraints, IK hints, expressions
      count_robj_list(r,robj);
      ++authored_stats().capture[4]; return {};
    }
    for(int k=0;k<3;++k) {j.rotation[k]=r.real(address+0x1C+k*4);j.scale[k]=r.real(address+0x2C+k*4);j.translation[k]=r.real(address+0x38+k*4);}
    // The same four words are a Quaternion when JOBJ_USE_QUATERNION is set: w follows x, y, z.
    j.quaternion = (j.flags & 0x20000u) != 0;
    if(j.quaternion) for(int k=0;k<4;++k) j.quat[k]=r.real(address+0x1C+k*4);
    for(int k=0;k<12;++k)j.world[k]=r.real(address+0x44+k*4);
    // HSD_JObjSetupMatrixSub runs HSD_RObjUpdateAll after make_mtx, so the captured world matrix
    // above already has the constraint in it. Capturing the list (and its targets) is what lets the
    // sampler reproduce that same world matrix at a fractional frame, and prove it did.
    if(robj) {
      if(!capture_robj_list(r,robj,j.constraints)) { ++authored_stats().feature[FEAT_ROBJ]; ++authored_stats().capture[4]; return {}; }
      constrained=true;
    }
    uint32_t aobj=r.word(address+0x7C);
    if(aobj) {
      if(!r.span(aobj,28)||r.word(aobj+24)){ ++authored_stats().capture[5]; return {}; }
      uint32_t flags=r.word(aobj);
      j.anim_flags=flags;
      j.frame=r.real(aobj+4); j.rewind=r.real(aobj+8); j.end=r.real(aobj+12);
      if(!(flags&0x50000000u)) {
        j.rate=r.real(aobj+16); uint32_t fobj=r.word(aobj+20);
        while(fobj && j.tracks.size()<32) {
          if(!r.span(fobj,48)){ ++authored_stats().capture[6]; return {}; }
          NativeMelee::PackedTrack t;
          uint32_t data=r.word(fobj+8), length=r.word(fobj+12), packed=r.word(fobj+16), formats=r.word(fobj+20);
          if(length>65535||byte_count+length>1024*1024||!r.span(data,length)){ ++authored_stats().capture[7]; return {}; }
          t.start_frame=(int16_t)(r.word(fobj+24)>>16); t.channel=packed&255;
          if(!((t.channel>=1&&t.channel<=3)||(t.channel>=5&&t.channel<=10))){ ++authored_stats().capture[8]; return {}; }
          t.value_format=formats>>24; t.slope_format=(formats>>16)&255;
          t.bytes.assign(current_memory+data-ppc::RAM_BASE,current_memory+data-ppc::RAM_BASE+length);
          byte_count+=length; j.tracks.push_back(std::move(t)); fobj=r.word(fobj);
        }
        if(fobj){ ++authored_stats().capture[9]; return {}; }
      }
    }
    pose->joints.push_back(std::move(j)); address=r.word(address+12);
  }
  if(address||!r.valid){ ++authored_stats().capture[10]; return {}; }
  std::reverse(pose->joints.begin(),pose->joints.end());
  if(constrained) ++authored_stats().robj_chains;
  chains_this_frame[start] = pose;
  return pose;
}

// Envelope (skinned) draw: the view matrix, the skeleton "right" transform and, per matrix slot,
// the weighted bones with their inverse-bind matrices. Mirrors SetupEnvelopeModelMtx and
// _HSD_mkEnvelopeModelNodeMtx so the render thread can rebuild every slot at a fractional frame.
std::shared_ptr<const AuthoredPose> capture_envelope(Reader& r) {
  auto pose=std::make_shared<AuthoredPose>();
  pose->envelope = true;
  if(!r.matrix(envelope_vmtx, pose->view)){ ++authored_stats().capture[11]; return {}; }
  pose->has_view = true;
  pose->quake = camera_quaking(r);
  // right (HSD_JObjFindSkeleton walk)
  uint32_t m = current_joint;
  if(!r.span(m,0x88)){ ++authored_stats().capture[11]; return {}; }
  uint32_t mflags = r.word(m+0x14);
  if(!(mflags & 2u)) {   // not JOBJ_SKELETON_ROOT
    uint32_t x = m;
    for(int guard=0; x && guard<128; ++guard) { if(!r.span(x,0x88)){ ++authored_stats().capture[11]; return {}; } if(r.word(x+0x14)&3u)break; x=r.word(x+0xC); }
    if(!x){ ++authored_stats().capture[12]; return {}; }
    pose->right_chain_m = capture_chain(r, m);
    pose->right_chain_x = capture_chain(r, x);
    if(!pose->right_chain_m||!pose->right_chain_x){ ++authored_stats().capture[12]; return {}; }
    uint32_t xflags = r.word(x+0x14);
    uint32_t xenv = r.word(x+0x78);
    if(x==m) { pose->right_kind = 1; if(!xenv||!r.matrix(xenv,pose->right_envelope)){ ++authored_stats().capture[12]; return {}; } }
    else if(xflags & 2u) pose->right_kind = 2;
    else { pose->right_kind = 3; if(!xenv||!r.matrix(xenv,pose->right_envelope)){ ++authored_stats().capture[12]; return {}; } }
  }
  // HSD_PObj: class (4), next, verts, flags/n_display, display, u.envelope_list at +0x14.
  if(!r.span(envelope_pobj,0x18)){ ++authored_stats().capture[13]; return {}; }
  uint32_t list = r.word(envelope_pobj+0x14);
  for(int slot=0; slot<10 && list; ++slot, list=r.word(list)) {
    if(!r.span(list,8)){ ++authored_stats().capture[13]; return {}; }
    AuthoredSlot s;
    uint32_t env = r.word(list+4);
    if(!env){ ++authored_stats().capture[13]; return {}; }
    if(!r.span(env,12)){ ++authored_stats().capture[13]; return {}; }
    float first_weight = r.real(env+8);
    bool single = first_weight >= (1.0f - 1.1920929e-7f);
    for(int guard=0; env && guard<64; ++guard, env=single?0:r.word(env)) {
      if(!r.span(env,12)){ ++authored_stats().capture[13]; return {}; }
      AuthoredBone b;
      uint32_t jobj = r.word(env+4);
      b.weight = single ? 1.0f : r.real(env+8);
      uint32_t envmtx = jobj ? r.word(jobj+0x78) : 0;
      if(!jobj||!envmtx||!r.matrix(envmtx,b.envelope)){ ++authored_stats().capture[14]; return {}; }
      b.chain = capture_chain(r, jobj);
      if(!b.chain){ ++authored_stats().capture[15]; return {}; }
      s.bones.push_back(std::move(b));
    }
    if(s.bones.empty()){ ++authored_stats().capture[13]; return {}; }
    pose->slots.push_back(std::move(s));
  }
  if(pose->slots.empty()||!r.valid){ ++authored_stats().capture[13]; return {}; }
  return pose;
}
}
RenderObserver::RenderObserver(ppc::Context& cpu, Observe kind, uint8_t* memory) : cpu_(cpu), kind_(kind) {
  if (kind == Observe::LoadJoint) loaded_joint_ = cpu.r[3];
  if (kind == Observe::ReleaseJoint) joints.erase(cpu.r[3]);
  if (kind == Observe::RigidMatrix) { rigid = true; envelope = false; rigid_vmtx = cpu.r[4]; }
  if (kind == Observe::OtherMatrix) { rigid = false; envelope = false; }
  if (kind == Observe::EnvelopeMatrix) { rigid = false; envelope = true; envelope_pobj = cpu.r[3]; envelope_vmtx = cpu.r[4]; }
  if (kind != Observe::DisplayJoint) return;
  saved_joint_ = current_joint; current_joint = cpu.r[3];
  saved_memory_ = current_memory; current_memory = memory;
  saved_owner_ = current_owner;
  current_owner = owner_tracking.load(std::memory_order_relaxed) ? resolve_owner(memory) : (uint8_t)0xFF;
  saved_rigid_ = rigid; rigid = false; saved_envelope_ = envelope; envelope = false;
  saved_pose_ = std::move(current_pose); current_pose.reset();
  saved_generation_ = current_generation; saved_pass_ = current_pass; saved_draw_ = current_draw;
  auto it = joints.find(cpu.r[3]);
  current_generation = it == joints.end() ? 0 : it->second;
  const uint64_t key[] = {current_generation, cpu.r[5], cpu.r[6]};
  uint64_t pass_key = hash_bytes(key, sizeof key);
  const uint64_t pass[] = {pass_key, passes[pass_key]++};
  current_pass = hash_bytes(pass, sizeof pass); current_draw = 0;
}
RenderObserver::~RenderObserver() {
  if (kind_ == Observe::AllocateJoint && cpu_.r[3]) joints[cpu_.r[3]] = next_generation++;
  // JObjLoad takes the joint in r3 and returns a status, so the pointer has to come from entry.
  if (kind_ == Observe::LoadJoint && loaded_joint_ && !joints.count(loaded_joint_)) joints[loaded_joint_] = next_generation++;
  if (kind_ == Observe::DisplayJoint) {
    current_joint = saved_joint_; current_memory = saved_memory_; rigid = saved_rigid_; envelope = saved_envelope_; current_pose = std::move(saved_pose_);
    current_generation = saved_generation_; current_pass = saved_pass_; current_draw = saved_draw_; current_owner = saved_owner_;
  }
}
uint64_t observed_draw_identity(uint64_t fallback, uint64_t& generation) {
  generation = current_generation;
  if (!generation) return fallback;
  const uint64_t key[] = {generation, current_pass, current_draw++};
  return hash_bytes(key, sizeof key);
}
void set_authored_capture(bool enabled) { authored_enabled = enabled; }
void set_owner_tracking(bool enabled) { owner_tracking.store(enabled, std::memory_order_relaxed); }
uint8_t observed_owner() { return current_owner; }
bool observed_skinned() { return envelope; }
std::shared_ptr<const AuthoredPose> capture_authored_pose() {
  if(!authored_enabled||!current_generation||!current_memory||!(rigid||envelope)){ ++authored_stats().capture[1]; return {}; }
  Reader r{current_memory};
  if(envelope) {
    // Every PObj of a skinned model has its own envelope list, so this is per draw (chains are shared).
    auto pose = capture_envelope(r);
    if(pose) ++authored_stats().captured;
    return pose;
  }
  if(current_pose)return current_pose;
  auto chain = capture_chain(r, current_joint);
  if(!chain) return {};
  auto pose = std::make_shared<AuthoredPose>();
  pose->chain = chain;
  pose->has_view = rigid_vmtx && r.matrix(rigid_vmtx, pose->view);
  pose->quake = pose->has_view && camera_quaking(r);
  ++authored_stats().captured; current_pose=pose; return pose;
}
void finish_observed_frame() { passes.clear(); chains_this_frame.clear(); chains_in_progress.clear(); constraint_depth = 0; }
}
