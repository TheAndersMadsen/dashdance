// SPDX-License-Identifier: GPL-2.0-or-later
#include "authored_pose.h"
#include "Geometry.h"
#include "subframe.h"
#include <atomic>
#include <cmath>
#include <cstring>
namespace gx {
// Furthest the view may travel in one simulation frame before it is treated as a cut rather than
// motion. Melee's camera tracks fighters; a cut jumps far beyond anything tracking produces.
static constexpr float kMaxViewStep = 120.0f;
AuthoredStats& authored_stats() { static AuthoredStats st; return st; }
static std::atomic<bool> g_interpolate{false};
void set_authored_interpolate(bool on) { g_interpolate.store(on, std::memory_order_relaxed); }
namespace {
using NativeMelee::Matrix;
bool near(float a,float b) { return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=0.002f*(1+std::abs(b)); }
bool inverse(const Matrix& m,Matrix& o) {
  double det=m[0]*(double(m[5])*m[10]-double(m[6])*m[9])-m[1]*(double(m[4])*m[10]-double(m[6])*m[8])+m[2]*(double(m[4])*m[9]-double(m[5])*m[8]);
  if (!std::isfinite(det)||std::abs(det)<1e-12) return false;
  const int rows[3][2]={{1,2},{2,0},{0,1}};
  for(int i=0;i<3;++i)for(int j=0;j<3;++j) {
    int a=rows[j][0],b=rows[j][1],c=rows[i][0],d=rows[i][1];
    o[i*4+j]=float((double(m[a*4+c])*m[b*4+d]-double(m[a*4+d])*m[b*4+c])/det);
  }
  for(int i=0;i<3;++i)o[i*4+3]=-(o[i*4]*m[3]+o[i*4+1]*m[7]+o[i*4+2]*m[11]);
  return true;
}
bool same_track(const NativeMelee::PackedTrack& a,const NativeMelee::PackedTrack& b) {
  return a.start_frame==b.start_frame&&a.channel==b.channel&&a.value_format==b.value_format&&a.slope_format==b.slope_format&&a.bytes==b.bytes;
}
// Inverse-transpose of the 3x3 part (the GX normal matrix), row-major 3x3 output.
bool normal_matrix(const Matrix& m, float out[9]) {
  Matrix inv; if(!inverse(m,inv))return false;
  for(int r=0;r<3;++r)for(int c=0;c<3;++c)out[r*3+c]=inv[c*4+r];
  return true;
}
Matrix from12(const float* p) { Matrix m; std::memcpy(m.data(),p,48); return m; }
// HSD_AObjInterpretAnim: a looping animation that reaches end_frame folds back into
// [rewind_frame, end_frame). Mirroring it here turns the loop boundary from a discontinuity
// (which used to decline the whole chain) into an ordinary continuation.
constexpr uint32_t AOBJ_LOOP = 1u << 29;
float wrap_frame(const AuthoredJoint& j,float frame) {
  if((j.anim_flags&AOBJ_LOOP)&&j.rewind<j.end&&frame>=j.end) return std::fmod(frame-j.rewind,j.end-j.rewind)+j.rewind;
  return frame;
}
bool continuous_clock(const AuthoredJoint& previous, const AuthoredJoint& current) {
  // Spatial reconstruction tolerates error proportional to world coordinates.
  // Applying that tolerance to an animation clock accepts an entire missing tick
  // after frame 500, advancing paused tracks and snapping back every source frame.
  constexpr uint32_t clock_flags = (1u << 27) | (1u << 28) | AOBJ_LOOP | (1u << 30);
  if (!std::isfinite(current.rate) || current.rate == 0 || previous.rate != current.rate ||
      previous.frame == current.frame || previous.end != current.end || previous.rewind != current.rewind ||
      ((previous.anim_flags ^ current.anim_flags) & clock_flags) ||
      (current.anim_flags & ((1u << 27) | (1u << 28) | (1u << 30)))) return false;
  const float expected = wrap_frame(current, previous.frame + current.rate);
  return std::isfinite(expected) && std::isfinite(current.frame) &&
      std::abs(expected - current.frame) <= 0.0001f;
}

// The local matrix of a JOBJ_USE_QUATERNION joint. MTXQuat (mtx.c) builds the rotation from the
// quaternion; HSD_MtxSRT then applies scale before it and the translation after, and the inherited
// scale is divided out per row exactly as the Euler path in Geometry.h does. Quaternions are blended
// with a normalised lerp along the shorter arc: over one simulation frame the two are a few degrees
// apart, where nlerp and slerp agree to far better than the 0.002 the reconstruction is checked to.
Matrix quat_srt(const NativeMelee::Vec& scale, const std::array<float,4>& q,
                const NativeMelee::Vec& position, const NativeMelee::Vec& parent_scale) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  Matrix m = {{1 - 2 * (yy + zz), 2 * (xy - wz),     2 * (xz + wy),     position[0],
               2 * (xy + wz),     1 - 2 * (xx + zz), 2 * (yz - wx),     position[1],
               2 * (xz - wy),     2 * (yz + wx),     1 - 2 * (xx + yy), position[2]}};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      if (std::abs(parent_scale[r]) < 1e-10f) throw std::runtime_error("Zero inherited scale");
      m[r * 4 + c] *= scale[c] * parent_scale[c] / parent_scale[r];
    }
  return m;
}

std::array<float,4> quat_blend(const std::array<float,4>& a, const std::array<float,4>& b, float t) {
  float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
  const float sign = dot < 0 ? -1.0f : 1.0f;   // shorter arc: q and -q are the same rotation
  std::array<float,4> out{};
  for (int k = 0; k < 4; ++k) out[k] = a[k] + t * (sign * b[k] - a[k]);
  float n = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
  if (!(n > 1e-8f)) return b;
  for (int k = 0; k < 4; ++k) out[k] /= n;
  return out;
}

const AuthoredPose& chain_of(const AuthoredPose& p) { return p.chain ? *p.chain : p; }
// Camera: the view matrix advanced `phase` frames by screw extrapolation of its last change. Returns
// the sampled view and the transform that carries a current view-space matrix to it.
bool camera_motion(const AuthoredPose& previous,const AuthoredPose& current,double phase,Matrix& view_new,Matrix& carry,bool& moved) {
  moved=false;
  if(!current.has_view){ view_new=NativeMelee::Identity(); carry=view_new; return false; }
  Matrix cur=from12(current.view.data());
  view_new=cur; carry=NativeMelee::Identity();
  if(!previous.has_view||previous.view==current.view) return true;
  // A quake jolts the view back and forth every tick. Advancing along that jolt overshoots it
  // (an alternating offset of a is drawn as up to 3a), so the shaking camera holds its exact
  // 60 Hz view as the console shows it.
  // This used to apply to Predict only, so Interpolate, the mode most people run, blended every
  // quake and rendered the whole scene through camera positions the game never showed.
  if(previous.quake||current.quake) return true;
  Matrix prev=from12(previous.view.data());
  // A cut (respawn, a zoom snap, a new stock) moves the view further in one tick than tracking ever
  // does. There is nothing between the two views to show, so blending across it slides the whole
  // stage through positions that never existed: the level rendering wrong for a split second.
  {
    float moved=0;
    for(int r=0;r<3;++r){ const float d=cur[r*4+3]-prev[r*4+3]; moved+=d*d; }
    if(!(moved<kMaxViewStep*kMaxViewStep)) return true;   // also catches NaN
  }
  if(g_interpolate.load(std::memory_order_relaxed)) SubFrameSolver::interpolate_matrix(prev.data(),cur.data(),phase,view_new.data());
  else SubFrameSolver::extrapolate_matrix(prev.data(),cur.data(),phase,view_new.data());
  Matrix inv_cur; if(!inverse(cur,inv_cur)){ view_new=cur; return true; }
  carry=NativeMelee::Multiply(view_new,inv_cur);
  // "moved" asks whether the sampled view differs from the one the held matrices already embed, and
  // that base is the current frame in Predict but the previous frame in Interpolate (carry_camera
  // makes the same distinction). Comparing against the current frame in both modes made Interpolate
  // report "not moved" at phase 1, where the sampled view is exactly the current one. Static chains
  // were then declined and the camera carry skipped, so every unanimated piece of the stage held the
  // previous frame while the fighters advanced: the whole of Yoshi's Story jumped back a frame and
  // forward again on the 4% of presented frames that land on phase 1, several times a second, worst
  // on the foliage because its woven texture shows a one pixel shift most.
  moved=g_interpolate.load(std::memory_order_relaxed)?view_new!=prev:view_new!=cur;
  return true;
}
}
static bool cached_chain(const AuthoredPose& previous,const AuthoredPose& current,double phase,Matrix& world,Matrix& inv,AuthoredCache* cache,bool allow_static);

// HSD_RObjUpdateAll's first step, and the only constraint kind captured: a joint carrying one or
// more active REFTYPE_JOBJ subtype 1 entries keeps its own orientation and scale and takes the
// average world position of their target joints. HSD_RObjGetGlobalPosition sums the targets'
// mtx[i][3] over the entries it matched and multiplies by 1/n; JObjUpdateFunc type 0x35 writes that
// into the joint's matrix. The type 0x38 that follows recomputes jobj->translate from the
// parent-relative matrix, which cannot change this matrix and is read back only by the limit and
// expression paths, neither of which occurs, so it is not reproduced.
// `exact` is the reconstruction of the current frame and is what the endpoint proof below compares
// against the matrix the game actually produced; `world` is the same arithmetic on sampled targets.
static bool constrain(const AuthoredJoint& previous,const AuthoredJoint& current,double phase,
                      AuthoredCache* cache,Matrix& exact,Matrix& world,bool& animated) {
  if(previous.constraints.size()!=current.constraints.size()){ ++authored_stats().sample[9]; return false; }
  float sum_exact[3]={0,0,0}, sum_new[3]={0,0,0}; int n=0;
  for(size_t k=0;k<current.constraints.size();++k) {
    const auto& c=current.constraints[k]; const auto& pc=previous.constraints[k];
    // The same constraint on the same joint in both frames, or there is nothing to interpolate: a
    // joint freed and reallocated at the same address gets a new generation and declines here.
    if(c.flags!=pc.flags||!c.target_generation||c.target_generation!=pc.target_generation||
       !c.target||!pc.target||c.target->joints.empty()||pc.target->joints.empty()){ ++authored_stats().sample[9]; return false; }
    Matrix target_new,target_inv;
    // allow_static: a target that is not moving this frame still has a position to be pinned to.
    if(!cached_chain(*pc.target,*c.target,phase,target_new,target_inv,cache,true)){ ++authored_stats().sample[9]; return false; }
    const auto& target_now=c.target->joints.back().world;
    const auto& target_was=pc.target->joints.back().world;
    for(int a=0;a<3;++a){ sum_exact[a]+=target_now[a*4+3]; sum_new[a]+=target_new[a*4+3]; }
    // A joint whose own tracks are still can be moved entirely by its target, and then the chain is
    // not static: it must sample rather than hold, exactly like a game-driven translation delta.
    if(target_was[3]!=target_now[3]||target_was[7]!=target_now[7]||target_was[11]!=target_now[11]) animated=true;
    ++n;
  }
  if(!n) return true;
  const float average=1.0f/(float)n;
  for(int a=0;a<3;++a){ exact[a*4+3]=average*sum_exact[a]; world[a*4+3]=average*sum_new[a]; }
  authored_stats().robj_applied+=(uint32_t)n;
  return true;
}

// The expensive part: re-sample every joint's authored tracks at the fractional frame and
// rebuild the chain's world matrix. Shared by all draws of the same object in a presented frame.
// `allow_static` accepts chains with no animated track (their world matrix simply holds), which
// skinned models need for bones that are not moving this frame.
static bool sample_chain(const AuthoredPose& previous,const AuthoredPose& current,double phase,Matrix& world,Matrix& inv,bool allow_static,AuthoredCache* cache) {
  if(!std::isfinite(phase)||phase<0||phase>1||current.joints.empty()||previous.joints.size()!=current.joints.size()){ ++authored_stats().sample[1]; return false; }
  world=NativeMelee::Identity(); Matrix exact=world;
  // The captured pose and the sampled pose each accumulate their own inherited scale; sharing one
  // made the validation below compare a captured world matrix against a sampled scale chain, which
  // is why animated scale used to decline (s8).
  NativeMelee::Vec inherited{{1,1,1}}, inherited_exact{{1,1,1}};
  bool animated=false,partial=false;
  try {
    for(size_t i=0;i<current.joints.size();++i) {
      const auto& j=current.joints[i]; const auto& p=previous.joints[i];
      if(!j.generation||j.generation!=p.generation||j.flags!=p.flags||j.tracks.size()!=p.tracks.size()){ ++authored_stats().sample[2]; return false; }
      const bool interp=g_interpolate.load(std::memory_order_relaxed);
      // Predict: sample `phase` frames past the current frame. Interpolate: sample `phase` frames
      // past the previous frame (exact at both ends, shown one frame late). Either way the frame is
      // folded back into the loop the way the game folds it, so a loop boundary is a continuation.
      const float sample_frame=wrap_frame(j,float((interp?p.frame:j.frame)+phase*j.rate));
      auto scale=j.scale,rot=j.rotation,pos=j.translation;
      bool driven[3]={false,false,false},rotation_driven=false;
      for(const auto& t:j.tracks){
        if(t.channel>=5&&t.channel<=7)driven[t.channel-5]=true;
        if(t.channel>=1&&t.channel<=3)rotation_driven=true;
      }
      // A joint whose animation was restarted, paused or retimed this frame, or that something
      // other than its tracks is driving, holds its captured pose. The rest of the skeleton still
      // animates: one such joint used to freeze a whole fighter, which is what made a run cycle
      // hitch once per stride.
      // Looping animations wrap back into range above; a non-looping one that has run past its last
      // keyframe holds rather than extrapolating a track beyond what it authored.
      // s3 used to count every hold here, which buried the real defect: an animation that has
      // simply finished, or that the game paused, holds its last pose because that IS the pose,
      // and counting those as failures made the counter read 38% when almost none of it was wrong.
      // Split by cause so only a genuine clock discontinuity (restart, retime, swap) reads as s3.
      bool sampled=false;
      if(!j.tracks.empty()) {
        const bool clock=continuous_clock(p,j);
        sampled=clock&&sample_frame>=0&&sample_frame<=j.end;
        if(!sampled) {
          if(p.frame==j.frame) ++authored_stats().sample[24];        // paused: holding is correct
          else if(!clock) ++authored_stats().sample[3];              // restarted/retimed: the real defect
          else if(sample_frame>j.end) ++authored_stats().sample[25]; // finished, non-looping: hold is correct
          else ++authored_stats().sample[26];                        // before the first keyframe
          partial=true;
        }
      }
      if(sampled) {
        auto ts=scale,tr=rot,tp=pos;   // commit only if every track of this joint samples
        for(size_t k=0;k<j.tracks.size();++k) {
          const auto& t=j.tracks[k]; if(!same_track(t,p.tracks[k])){ ++authored_stats().sample[4]; sampled=false; break; }
          float* component=nullptr;
          if(t.channel>=1&&t.channel<=3)component=&tr[t.channel-1];
          else if(t.channel>=5&&t.channel<=7)component=&tp[t.channel-5];
          else if(t.channel>=8&&t.channel<=10)component=&ts[t.channel-8];
          else { ++authored_stats().sample[5]; sampled=false; break; }
          float at_current, value;
          if(!NativeMelee::SamplePacked(t,j.frame,at_current)||!near(at_current,*component)){ ++authored_stats().sample[6]; sampled=false; break; }
          if(!NativeMelee::SamplePacked(t,sample_frame,value)){ ++authored_stats().sample[7]; sampled=false; break; }
          *component=value;
        }
        if(sampled){ scale=ts; rot=tr; pos=tp; animated=true; } else partial=true;
      }
      // A joint that could not be sampled used to keep its current-frame values. In Interpolate every
      // other joint is shown at previous + phase, so this one sat a whole frame ahead of the rest of
      // its own model, and as sampling came and went between presented frames the scenery snapped
      // back and forth (Yoshi's Story: trees and flowers alternating at 240 Hz). Blend it between its
      // two frames by the same phase instead, so a model is never at two times at once.
      if(!sampled&&interp&&!j.tracks.empty()) {
        const float t=float(phase);
        for(const auto& tr_:j.tracks){
          if(tr_.channel>=1&&tr_.channel<=3){
            const int k=tr_.channel-1; float d=j.rotation[k]-p.rotation[k];
            if(!std::isfinite(d))continue;
            d=std::remainder(d,6.2831853f);   // the short way round
            rot[k]=p.rotation[k]+t*d;
          } else if(tr_.channel>=5&&tr_.channel<=7){
            const int k=tr_.channel-5; pos[k]=p.translation[k]+t*(j.translation[k]-p.translation[k]);
          } else if(tr_.channel>=8&&tr_.channel<=10){
            const int k=tr_.channel-8; scale[k]=p.scale[k]+t*(j.scale[k]-p.scale[k]);
          }
        }
        if(t>0&&t<1)animated=true;
      }
      // Game-driven motion (fighter positions, items, knockback) has no track: predict it forward by
      // the last simulated per-frame delta, bounded so teleports and respawns hold instead.
      for(int k=0;k<3;++k){
        if(driven[k])continue;
        float delta=j.translation[k]-p.translation[k];
        if(delta!=0.0f&&std::isfinite(delta)&&std::abs(delta)<=30.0f){ pos[k]=interp?p.translation[k]+float(phase)*delta:j.translation[k]+float(phase)*delta; animated=true; }
      }
      // Game-driven rotation (a fighter turning, a tumbling item) gets the same treatment; the
      // half-radian bound keeps a snap to a new facing, or an angle wrapping past pi, from spinning.
      if(!rotation_driven)for(int k=0;k<3;++k){
        float delta=j.rotation[k]-p.rotation[k];
        if(delta!=0.0f&&std::isfinite(delta)&&std::abs(delta)<=0.5f){ rot[k]=interp?p.rotation[k]+float(phase)*delta:j.rotation[k]+float(phase)*delta; animated=true; }
      }
      exact=NativeMelee::Multiply(exact,NativeMelee::SRT(j.scale,j.rotation,j.translation,inherited_exact));
      if(j.quaternion&&p.quaternion) {
        // Blend between the two frames' quaternions, the same way the Euler path blends angles.
        const auto q=quat_blend(interp?p.quat:j.quat, j.quat, interp?float(phase):0.0f);
        world=NativeMelee::Multiply(world,quat_srt(scale,q,pos,inherited));
        if(interp&&phase>0&&p.quat!=j.quat) animated=true;
      } else {
        world=NativeMelee::Multiply(world,NativeMelee::SRT(scale,rot,pos,inherited));
      }
      // HSD_JObjSetupMatrixSub applies the joint's constraints after make_mtx, so they land here,
      // on both the reconstruction and the sampled pose, and before the reconstruction is checked.
      if(!j.constraints.empty()&&!constrain(p,j,phase,cache,exact,world,animated)) return false;
      for(int k=0;k<12;++k)if(!near(exact[k],j.world[k])){ ++authored_stats().sample[8]; return false; }
      if(!(j.flags&8)){ for(int k=0;k<3;++k){ inherited[k]*=scale[k]; inherited_exact[k]*=j.scale[k]; } }
    }
  } catch(const std::exception&) { { ++authored_stats().sample[10]; return false; } }
  if(partial) ++authored_stats().sample[23];   // sampled, with at least one joint held
  if(!animated&&!allow_static){ ++authored_stats().sample[11]; return false; }
  if(!inverse(current.joints.back().world,inv)){ ++authored_stats().sample[12]; return false; }
  return true;
}
static bool cached_chain(const AuthoredPose& previous,const AuthoredPose& current,double phase,Matrix& world,Matrix& inv,AuthoredCache* cache,bool allow_static) {
  if(!cache)return sample_chain(previous,current,phase,world,inv,allow_static,cache);
  AuthoredChainKey key{&previous,&current,allow_static};
  // A constrained chain samples its targets through this same cache, so the insert has to follow the
  // nested sampling: the iterator from the lookup above is only compared, never held across it.
  if(cache->find(key)==cache->end()) {
    AuthoredChain chain; chain.ok=sample_chain(previous,current,phase,chain.world,chain.inverse_current,allow_static,cache);
    cache->emplace(key,chain);
  }
  auto it=cache->find(key);
  if(!it->second.ok)return false;
  world=it->second.world; inv=it->second.inverse_current;
  return true;
}
bool sample_authored(const AuthoredPose& previous,const AuthoredPose& current,double phase,
                     const float matrix[12],float result[12],float normals[9],const float current_normals[9],AuthoredCache* cache) {
  if(current.envelope||previous.envelope)return false;
  Matrix view_new,carry; bool camera_moved=false;
  camera_motion(previous,current,phase,view_new,carry,camera_moved);
  Matrix world,inv;
  if(!cached_chain(chain_of(previous),chain_of(current),phase,world,inv,cache,camera_moved))return false;
  Matrix base; std::memcpy(base.data(),matrix,48);
  auto output=NativeMelee::Multiply(carry,NativeMelee::Multiply(NativeMelee::Multiply(base,inv),world));
  Matrix base_inv;
  if(!inverse(base,base_inv)){ ++authored_stats().sample[13]; return false; }
  auto delta=NativeMelee::Multiply(output,base_inv);
  // `delta` is only ever used to carry the normal matrix forward. Declining the whole draw when it
  // cannot be inverted threw away a correct re-pose because of the lighting: flat geometry (foliage
  // cards, billboards, anything with a zero scale axis) has a singular delta by construction, and on
  // Yoshi's Story that was 17,870 draws per interval, every frame, which is what made the bushes and
  // trees flash. The positions are already computed and finite, so keep them and hold the normals
  // for this frame instead. A frame-old normal matrix is a lighting difference no one can see; the
  // object vanishing onto a different timeline is not.
  Matrix delta_inv;
  const bool have_normals = inverse(delta, delta_inv);
  if(!have_normals) ++authored_stats().sample[14];
  for(float v:output)if(!std::isfinite(v)){ ++authored_stats().sample[15]; return false; }
  std::memcpy(result,output.data(),48);
  if(have_normals) {
    for(int r=0;r<3;++r)for(int c=0;c<3;++c)
      normals[r*3+c]=delta_inv[r]*current_normals[c]+delta_inv[4+r]*current_normals[3+c]+delta_inv[8+r]*current_normals[6+c];
  } else {
    std::memcpy(normals,current_normals,9*sizeof(float));
  }
  ++authored_stats().sampled;
  return true;
}

// A draw that could not be re-posed still has to move with the camera: otherwise it freezes for a
// whole simulation frame while everything around it advances, and jumps a full frame at the
// boundary. This applies only the camera's motion, leaving the object's own pose held.
bool carry_camera(const AuthoredPose& previous,const AuthoredPose& current,double phase,
                  const AuthoredPose& base,
                  const float in_pos[256],const float in_nrm[96],uint64_t pos_slots,
                  float out_pos[256],float out_nrm[96]) {
  if(!previous.has_view||!current.has_view||!base.has_view||!pos_slots)return false;
  Matrix view_new,unused_carry; bool moved=false;
  camera_motion(previous,current,phase,view_new,unused_carry,moved);
  // The held matrices embed the view of the frame they were taken from, and which frame that is
  // depends on the caller, not on the mode: a paired draw holds the previous frame in Interpolate,
  // but a draw with no pair at all has only the current frame to hold. Inferring it from the mode
  // carried every unpaired draw off the previous frame's view while its matrices carried the
  // current one, which left the whole inverse camera step in the result: on Yoshi's Story that is
  // 51,115 draws per interval (the bushes, trees and backdrop) drawn a full camera frame away from
  // the rest of the stage whenever the camera moves, snapping back to the right place whenever it
  // stops. `base` is now the pose the caller actually held, so the carry cancels exactly the view
  // that is in the matrices and nothing else.
  const Matrix base_view=from12(base.view.data());
  if(view_new==base_view)return false;   // the matrices already embed the sampled view
  Matrix inv_base; if(!inverse(base_view,inv_base))return false;
  const Matrix carry=NativeMelee::Multiply(view_new,inv_base);
  Matrix carry_inv; const bool have_normals=inverse(carry,carry_inv);
  return apply_carry(carry.data(),have_normals?carry_inv.data():nullptr,in_pos,in_nrm,pos_slots,out_pos,out_nrm);
}

// The camera part of the work above, done once for a presented frame instead of once per draw. The
// whole of a stage that is standing still shares this transform, and building it per draw meant a
// matrix inverse and a screw interpolation (acos, sin, pow) for every piece of scenery on screen.
bool camera_sub_frame_carry(const AuthoredPose& previous,const AuthoredPose& current,double phase,
                            float carry[12],float carry_inverse[12],bool& has_inverse) {
  has_inverse=false;
  if(!previous.has_view||!current.has_view)return false;
  Matrix view_new,unused_carry; bool moved=false;
  camera_motion(previous,current,phase,view_new,unused_carry,moved);
  const Matrix cur_view=from12(current.view.data());
  if(view_new==cur_view)return false;
  Matrix inv_cur; if(!inverse(cur_view,inv_cur))return false;
  const Matrix c=NativeMelee::Multiply(view_new,inv_cur);
  std::memcpy(carry,c.data(),48);
  Matrix c_inv;
  if(inverse(c,c_inv)){ std::memcpy(carry_inverse,c_inv.data(),48); has_inverse=true; }
  return true;
}

bool apply_carry(const float carry[12],const float carry_inverse[12],
                 const float in_pos[256],const float in_nrm[96],uint64_t pos_slots,
                 float out_pos[256],float out_nrm[96]) {
  if(!pos_slots)return false;
  const Matrix c=from12(carry);
  bool any=false;
  for(int row=0;row+3<=64;++row) {
    if(!(pos_slots&(1ull<<row)))continue;
    const Matrix out=NativeMelee::Multiply(c,from12(in_pos+row*4));
    bool finite=true; for(float v:out)if(!std::isfinite(v))finite=false;
    if(!finite)continue;
    std::memcpy(out_pos+row*4,out.data(),48);
    any=true;
    if(carry_inverse&&row+3<=32) {
      const float* n=in_nrm+row*3; float updated[9];
      for(int r=0;r<3;++r)for(int c2=0;c2<3;++c2)
        updated[r*3+c2]=carry_inverse[r]*n[c2]+carry_inverse[4+r]*n[3+c2]+carry_inverse[8+r]*n[6+c2];
      std::memcpy(out_nrm+row*3,updated,sizeof updated);
    }
  }
  return any;
}

// Skinned draws (SetupEnvelopeModelMtx): slot = view * (sum_k weight_k * world_k * envelope_k) [* right].
// Computed twice per slot: with the current worlds (must match the draw's matrices, which proves the
// reconstruction) and with the sampled worlds at the fractional frame (the output).
bool sample_authored_envelope(const AuthoredPose& previous,const AuthoredPose& current,double phase,
                              const float current_pos[256],const float current_nrm[96],float out_pos[256],float out_nrm[96],AuthoredCache* cache) {
  if(!current.envelope||!previous.envelope||current.slots.empty()||current.slots.size()!=previous.slots.size()||current.slots.size()>10){ ++authored_stats().sample[16]; return false; }
  if(current.right_kind!=previous.right_kind){ ++authored_stats().sample[16]; return false; }
  Matrix view_new,carry; bool camera_moved=false;
  camera_motion(previous,current,phase,view_new,carry,camera_moved);
  const Matrix view=from12(current.view.data());
  // The skeleton-root transform, current and sampled.
  Matrix right_cur=NativeMelee::Identity(), right_new=NativeMelee::Identity();
  bool has_right=current.right_kind!=0;
  if(has_right) {
    if(!current.right_chain_m||!current.right_chain_x||!previous.right_chain_m||!previous.right_chain_x){ ++authored_stats().sample[17]; return false; }
    Matrix wm_new,wm_inv,wx_new,wx_inv;
    if(!cached_chain(*previous.right_chain_m,*current.right_chain_m,phase,wm_new,wm_inv,cache,true)||
       !cached_chain(*previous.right_chain_x,*current.right_chain_x,phase,wx_new,wx_inv,cache,true)){ ++authored_stats().sample[17]; return false; }
    const Matrix wm_cur=current.right_chain_m->joints.back().world, wx_cur=current.right_chain_x->joints.back().world;
    const Matrix xenv=from12(current.right_envelope.data());
    auto make=[&](const Matrix& wm,const Matrix& wx,Matrix& out)->bool{
      if(current.right_kind==1){ return inverse(xenv,out); }
      if(current.right_kind==2){ Matrix inv; if(!inverse(wx,inv))return false; out=NativeMelee::Multiply(inv,wm); return true; }
      Matrix n=NativeMelee::Multiply(wx,xenv), inv; if(!inverse(n,inv))return false; out=NativeMelee::Multiply(inv,wm); return true;
    };
    if(!make(wm_cur,wx_cur,right_cur)||!make(wm_new,wx_new,right_new)){ ++authored_stats().sample[17]; return false; }
  }
  // Staged, not published in place: a slot that fails after earlier slots were written would leave
  // the caller holding a pose that is fractional for some bones and current for the rest, which is
  // not a pose the game ever had. With a still camera nothing downstream overwrites it, so the
  // mixture would be drawn. Nothing reaches the caller unless every slot validates.
  float staged_pos[256]; float staged_nrm[96];
  std::memcpy(staged_pos,current_pos,sizeof staged_pos);
  std::memcpy(staged_nrm,current_nrm,sizeof staged_nrm);
  for(size_t s=0;s<current.slots.size();++s) {
    const auto& slot=current.slots[s]; const auto& pslot=previous.slots[s];
    if(slot.bones.size()!=pslot.bones.size()||slot.bones.empty()){ ++authored_stats().sample[18]; return false; }
    Matrix blend_cur{}, blend_new{};
    for(size_t b=0;b<slot.bones.size();++b) {
      const auto& bone=slot.bones[b]; const auto& pbone=pslot.bones[b];
      if(!bone.chain||!pbone.chain||!near(bone.weight,pbone.weight)||bone.envelope!=pbone.envelope){ ++authored_stats().sample[18]; return false; }
      Matrix w_new,w_inv;
      if(!cached_chain(*pbone.chain,*bone.chain,phase,w_new,w_inv,cache,true)){ ++authored_stats().sample[19]; return false; }
      const Matrix w_cur=bone.chain->joints.back().world;
      const Matrix env=from12(bone.envelope.data());
      // SetupEnvelopeModelMtx: a single full-weight bone without a skeleton-root transform uses the
      // joint matrix alone; every other case multiplies by the inverse-bind (envelope) matrix.
      bool bare = slot.bones.size()==1 && bone.weight>=1.0f-1.1920929e-7f && !has_right;
      Matrix c=bare?w_cur:NativeMelee::Multiply(w_cur,env), n=bare?w_new:NativeMelee::Multiply(w_new,env);
      for(int k=0;k<12;++k){ blend_cur[k]+=bone.weight*c[k]; blend_new[k]+=bone.weight*n[k]; }
    }
    if(has_right){ blend_cur=NativeMelee::Multiply(blend_cur,right_cur); blend_new=NativeMelee::Multiply(blend_new,right_new); }
    Matrix m_cur=NativeMelee::Multiply(view,blend_cur), m_new=NativeMelee::Multiply(view_new,blend_new);
    // Proof: the reconstruction of the current pose must reproduce the matrix the game loaded.
    const float* actual=current_pos+12*s;
    for(int k=0;k<12;++k)if(!near(m_cur[k],actual[k])){ ++authored_stats().sample[20]; return false; }
    for(float v:m_new)if(!std::isfinite(v)){ ++authored_stats().sample[21]; return false; }
    float nrm[9];
    if(!normal_matrix(m_new,nrm)){ ++authored_stats().sample[22]; return false; }
    std::memcpy(staged_pos+12*s,m_new.data(),48);
    if(9*s+9<=96)std::memcpy(staged_nrm+9*s,nrm,sizeof nrm);
  }
  std::memcpy(out_pos,staged_pos,sizeof staged_pos);
  std::memcpy(out_nrm,staged_nrm,sizeof staged_nrm);
  ++authored_stats().sampled;
  return true;
}
}
namespace gx {
const char* const kCaptureFeatureNames[FEAT_COUNT] = {
  "billboard", "pbillboard", "instance", "quaternion", "joint1", "joint2",
  "user_def_mtx", "mtx_indep_parent", "mtx_indep_srt", "robj",
  "robj_position", "robj_dir", "robj_up", "robj_orient", "robj_jobj_other",
  "robj_limit", "robj_ikhint", "robj_exp", "robj_inactive", "robj_unknown",
  "robj_animated", "robj_deep", "robj_target", "robj_cycle"
};
}
