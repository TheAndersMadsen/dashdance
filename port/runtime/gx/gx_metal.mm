// SPDX-License-Identifier: GPL-2.0-or-later
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#if defined(__APPLE__) && __has_include(<MetalFX/MetalFX.h>)
#define MELEE_HAS_METALFX 1
#import <MetalFX/MetalFX.h>
#endif
#include "gx_metal.h"
#include "gx_msl.h"
#include "gx_regs.h"
#include "gx_shader.h"
#include "gx_texture.h"
#include "frame_queue.h"
#include "host.h"
#include "window.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <CoreText/CoreText.h>
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gx {
namespace {

constexpr int FRAME_SLOTS = 3;
constexpr size_t VERTEX_RING = 24u << 20, INDEX_RING = 12u << 20, CONSTANT_RING = 32u << 20;

struct Ring {
  id<MTLBuffer> buffer = nil;
  size_t size = 0, used = 0;
  void init(id<MTLDevice> device, size_t bytes) {
    // Upload-only rings: shared for unified memory, write-combined so the CPU's sequential writes bypass the cache.
    buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    size = bytes; used = 0;
  }
  bool alloc(size_t bytes, size_t align, uint8_t** cpu, size_t* offset) {
    size_t off = (used + align - 1) & ~(align - 1);
    if (off + bytes > size) return false;
    *cpu = (uint8_t*)buffer.contents + off; *offset = off; used = off + bytes;
    return true;
  }
};

struct PsoKey {
  uint64_t vs, ps;
  uint32_t blend, zmode, pixel_format, topology;
  bool operator==(const PsoKey& o) const {
    return vs == o.vs && ps == o.ps && blend == o.blend && zmode == o.zmode && pixel_format == o.pixel_format && topology == o.topology;
  }
};
struct PsoKeyHash { size_t operator()(const PsoKey& k) const {
  const uint64_t f[] = {k.vs, k.ps, k.blend, k.zmode, k.pixel_format, k.topology};
  return (size_t)hash_bytes(f, sizeof f);
} };

struct TextureEntry { id<MTLTexture> texture = nil; uint32_t width = 0, height = 0, levels = 1; uint64_t last_used = 0; };
struct Pipeline { id<MTLRenderPipelineState> state = nil; };

struct BlitConstants { float rect[4]; float sharp[4]; float box[4]; };
struct ClearConstants { float color[4]; float depth; float pad[3]; };

const char* kBlitSource = R"(
#include <metal_stdlib>
using namespace metal;
struct C { float4 rect; float4 sharp; float4 box; };
struct O { float4 pos [[position]]; float2 uv; };
vertex O blit_vs(uint id [[vertex_id]], constant C& c [[buffer(0)]]) {
  O o; float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p * c.rect.xy + c.rect.zw; return o;
}
float4 downsample(texture2d<float> src, sampler samp, constant C& c, float2 uv) {
  int nx = int(c.box.x), ny = int(c.box.y);
  if (nx <= 1 && ny <= 1) return src.sample(samp, uv);
  float2 foot = c.sharp.xy * float2(nx, ny);
  float4 acc = float4(0);
  for (int y = 0; y < ny; ++y)
    for (int x = 0; x < nx; ++x)
      acc += src.sample(samp, uv + (float2(x, y) + 0.5) / float2(nx, ny) * foot - 0.5 * foot);
  return acc / float(nx * ny);
}
fragment float4 blit_ps(O i [[stage_in]], constant C& c [[buffer(0)]], texture2d<float> src [[texture(0)]], sampler samp [[sampler(0)]]) {
  float4 col = downsample(src, samp, c, i.uv);
  if (c.sharp.z <= 0.0) return col;
  float2 step = c.sharp.xy * max(c.box.xy, float2(1.0));
  float3 n = src.sample(samp, i.uv + float2(0, -step.y)).rgb, s = src.sample(samp, i.uv + float2(0, step.y)).rgb;
  float3 w = src.sample(samp, i.uv + float2(-step.x, 0)).rgb, e = src.sample(samp, i.uv + float2(step.x, 0)).rgb;
  float3 mn = min(min(min(n, s), min(w, e)), col.rgb), mx = max(max(max(n, s), max(w, e)), col.rgb);
  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, float3(1e-4))));
  float peak = -1.0 / mix(8.0, 5.0, saturate(c.sharp.z));
  float3 wgt = amp * peak;
  float3 r = (col.rgb + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt);
  return float4(saturate(r), col.a);
}
struct ClearC { float4 color; float depth; float pad0, pad1, pad2; };
struct ClearO { float4 pos [[position]]; };
vertex ClearO clear_vs(uint id [[vertex_id]], constant ClearC& c [[buffer(0)]]) {
  ClearO o; float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), c.depth, 1); return o;
}
fragment float4 clear_ps(ClearO i [[stage_in]], constant ClearC& c [[buffer(0)]]) { return c.color; }

// Controller overlay: one rounded-rect / circle / ring per instance, signed-distance
// anti-aliased, labels sampled from a CoreText atlas (system font).
struct OvShape { float4 rect; float4 color; float4 params; uint label; float label_w; float label_h; float pad; };
struct OvC { float2 size; float alpha; float pad; float4 labels[16]; };
struct OvO { float4 pos [[position]]; float2 px; uint id [[flat]]; };
// Text: one glyph quad per instance, sampled from the ASCII atlas (premultiplied output).
struct TxGlyph { float4 rect; float4 uv; float4 color; };
struct TxC { float2 size; float2 atlas; };
struct TxO { float4 pos [[position]]; float2 uv; uint id [[flat]]; };
vertex TxO text_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant TxGlyph* g [[buffer(0)]], constant TxC& c [[buffer(1)]]) {
  float4 r = g[iid].rect, u = g[iid].uv;
  bool right = (vid == 1 || vid == 2 || vid == 4), bottom = (vid == 2 || vid == 4 || vid == 5);
  float2 p = float2(right ? r.z : r.x, bottom ? r.w : r.y);
  TxO o; o.id = iid;
  o.pos = float4(p / c.size * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = float2(right ? u.x + u.z : u.x, bottom ? u.y + u.w : u.y) / c.atlas;
  return o;
}
fragment float4 text_ps(TxO i [[stage_in]], constant TxGlyph* g [[buffer(0)]], constant TxC& c [[buffer(1)]], texture2d<float> atlas [[texture(0)]], sampler samp [[sampler(0)]]) {
  float a = atlas.sample(samp, i.uv).r * g[i.id].color.a;
  return float4(g[i.id].color.rgb * a, a);
}
vertex OvO overlay_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant OvShape* shapes [[buffer(0)]], constant OvC& c [[buffer(1)]]) {
  float4 r = shapes[iid].rect + float4(-2, -2, 2, 2);
  float2 p = float2((vid == 1 || vid == 2 || vid == 4) ? r.z : r.x, (vid == 2 || vid == 4 || vid == 5) ? r.w : r.y);
  OvO o; o.px = p; o.id = iid;
  o.pos = float4(p / c.size * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float label_coverage(OvShape s, constant OvC& c, float2 p, texture2d<float> atlas, sampler samp) {
  if (s.label == 0u || s.label >= 16u) return 0.0;
  float4 l = c.labels[s.label];   // atlas rect in texels: x, y, w, h
  if (l.w <= 0.0) return 0.0;
  float aspect = l.z / l.w;
  float lw = min(s.label_w, s.label_h * aspect), lh = lw / aspect;
  float2 centre = (s.rect.xy + s.rect.zw) * 0.5;
  float2 q = (p - centre) / float2(lw, lh) + 0.5;
  if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) return 0.0;
  float2 texel = l.xy + q * l.zw;
  return atlas.sample(samp, texel / float2(atlas.get_width(), atlas.get_height())).r;
}
fragment float4 overlay_ps(OvO i [[stage_in]], constant OvShape* shapes [[buffer(0)]], constant OvC& c [[buffer(1)]],
                           texture2d<float> atlas [[texture(0)]], sampler samp [[sampler(0)]]) {
  OvShape s = shapes[i.id];
  float2 half_size = (s.rect.zw - s.rect.xy) * 0.5, centre = (s.rect.xy + s.rect.zw) * 0.5;
  float corner = min(s.params.x, min(half_size.x, half_size.y));
  float2 q = abs(i.px - centre) - (half_size - corner);
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - corner;
  float fill = 1.0 - smoothstep(-0.75, 0.75, d);
  float label = label_coverage(s, c, i.px, atlas, samp) * fill;
  float3 col = mix(s.color.rgb, float3(1.0), label);
  float a = max(fill * s.color.a, label * 0.95);
  return float4(col * a * c.alpha, a * c.alpha);
}
)";

std::atomic<int> g_device_scale_cap{0}, g_thermal_scale_cap{0};

class MetalBackend final : public Backend {
 public:
  MetalBackend(CAMetalLayer* layer, int w, int h, const MetalOptions& o) : layer_(layer), opts_(o), client_w_(w), client_h_(h) { init(); }
  ~MetalBackend() override {
    if (compile_queue_) dispatch_barrier_sync(compile_queue_, ^{});   // pending background compiles still reference this
    if (precompile_queue_) dispatch_sync(precompile_queue_, ^{});
    wait_idle();
  }
  void submit_frame(const Frame& frame) override { submit(frame, nullptr); }
  void submit_frame(const Frame& frame, const DrawMatrices* overrides) override { submit(frame, overrides); }
  void set_skip_present(bool skip) override { skip_present_ = skip; }
  void resize(int w, int h) {
    wait_idle();
    client_w_ = std::max(w, 1); client_h_ = std::max(h, 1);
    layer_.drawableSize = CGSizeMake(client_w_, client_h_);
    if (opts_.efb_scale == 0 && pick_scale() != scale_) { efb_copies_.clear(); create_efb(); }
  }
  void set_options(const MetalOptions& o) {
    const bool rescale = o.efb_scale != opts_.efb_scale || o.ssaa != opts_.ssaa || o.widescreen != opts_.widescreen || o.upscaler != opts_.upscaler;
    const bool resample = o.anisotropy != opts_.anisotropy;
    opts_ = o;
#if TARGET_OS_OSX
    layer_.displaySyncEnabled = opts_.vsync;
#endif
    if (rescale && pick_scale() != scale_) { wait_idle(); efb_copies_.clear(); create_efb(); }
    if (resample) { wait_idle(); samplers_.clear(); }
  }
  uint64_t frames_presented() const { return frames_presented_; }
  void log_scale_change() const { host::log("metal: internal resolution now %ux%u (EFB x%d)", EFB_WIDTH * scale_, EFB_HEIGHT * scale_, scale_); }
  void set_overlay(OverlayProvider provider) { overlay_ = std::move(provider); }

 private:
  // ---- setup
  void init() {
    device_ = layer_.device ?: MTLCreateSystemDefaultDevice();
    if (!device_) host::die("metal: no GPU device");
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    layer_.opaque = YES;   // with a full-screen window this lets the compositor hand the drawable straight to the display (no extra frame of latency)
    layer_.drawableSize = CGSizeMake(client_w_, client_h_);
#if TARGET_OS_OSX
    layer_.displaySyncEnabled = opts_.vsync;
#endif
    init_fx_support();
    // Double-buffered swapchain: the simulation runs at 60 Hz and a finished frame should reach
    // the next refresh slot (120 Hz on ProMotion) instead of queueing behind another drawable.
    layer_.maximumDrawableCount = 2;
    queue_ = [device_ newCommandQueue];
    frame_semaphore_ = dispatch_semaphore_create(FRAME_SLOTS);
    for (int i = 0; i < FRAME_SLOTS; ++i) {
      vertex_ring_[i].init(device_, VERTEX_RING);
      index_ring_[i].init(device_, INDEX_RING);
      constant_ring_[i].init(device_, CONSTANT_RING);
    }
    vertex_descriptor_ = [MTLVertexDescriptor vertexDescriptor];
    auto attr = [&](int index, MTLVertexFormat format, int offset) {
      vertex_descriptor_.attributes[index].format = format;
      vertex_descriptor_.attributes[index].offset = offset;
      vertex_descriptor_.attributes[index].bufferIndex = 0;
    };
    attr(0, MTLVertexFormatFloat3, 0);
    attr(1, MTLVertexFormatFloat3, 12);
    attr(2, MTLVertexFormatUChar4Normalized, 24);
    attr(3, MTLVertexFormatUChar4Normalized, 28);
    for (int i = 0; i < 8; ++i) attr(4 + i, MTLVertexFormatFloat2, 32 + 8 * i);
    attr(12, MTLVertexFormatUChar4, 96);
    attr(13, MTLVertexFormatUChar4, 100);
    vertex_descriptor_.layouts[0].stride = sizeof(Vertex);
    vertex_descriptor_.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    create_blit_pipelines();
    create_efb();
    host::log("metal: %s", device_.name.UTF8String);
  }

  void create_blit_pipelines() {
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kBlitSource] options:options error:&error];
    if (!lib) host::die("metal: blit shaders: %s", error.localizedDescription.UTF8String);
    auto make = [&](const char* vs, const char* ps, MTLPixelFormat color, bool depth, bool blend) {
      MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:vs]];
      d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:ps]];
      d.colorAttachments[0].pixelFormat = color;
      if (blend) {   // premultiplied alpha
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      }
      if (depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
      if (!state) host::die("metal: pipeline %s/%s: %s", vs, ps, error.localizedDescription.UTF8String);
      return state;
    };
    blit_present_ = make("blit_vs", "blit_ps", MTLPixelFormatBGRA8Unorm, false, false);
    blit_copy_ = make("blit_vs", "blit_ps", MTLPixelFormatRGBA8Unorm, false, false);
    clear_pipeline_ = make("clear_vs", "clear_ps", MTLPixelFormatRGBA8Unorm, true, false);
    overlay_pipeline_ = make("overlay_vs", "overlay_ps", MTLPixelFormatBGRA8Unorm, false, true);
    text_pipeline_ = make("text_vs", "text_ps", MTLPixelFormatBGRA8Unorm, false, true);
    create_overlay_atlas();
    create_glyph_atlas();
    if (!opts_.capture_path.empty()) async_compile_ = false;   // captured frames must be complete and deterministic
    precompile_from_cache();
    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = YES;
    clear_depth_state_ = [device_ newDepthStencilStateWithDescriptor:ds];
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear; sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge; sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    blit_sampler_ = [device_ newSamplerStateWithDescriptor:sd];
  }

  float output_aspect() const { return opts_.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f; }
  bool fx_wanted() const { return opts_.upscaler > 0 && fx_supported_; }
  int pick_scale() const {
    constexpr int max_scale = 16384 / EFB_WIDTH;
    const int ssaa = std::clamp(opts_.ssaa, 1, 2);
    int cap = max_scale;
    if (const int d = g_device_scale_cap.load(std::memory_order_relaxed)) cap = std::min(cap, d);
    if (const int t = g_thermal_scale_cap.load(std::memory_order_relaxed)) cap = std::min(cap, t);
    if (opts_.efb_scale > 0) return std::clamp(std::min(opts_.efb_scale, cap) * ssaa, 1, max_scale);
    float ww = (float)std::max(client_w_, 1), wh = (float)std::max(client_h_, 1);
    float aspect = output_aspect();
    float vw = ww, vh = ww / aspect;
    if (vh > wh) { vh = wh; vw = wh * aspect; }
    if (fx_wanted()) { vw *= 0.5f; vh *= 0.5f; }   // MetalFX reconstructs to the window size at present
    int s = std::max((int)std::ceil(vw / (480.0f * aspect)), (int)std::ceil(vh / 480.0f));
    return std::clamp(std::min(s, cap) * ssaa, 1, max_scale);
  }

  void create_efb() {
    scale_ = pick_scale();
    efb_w_ = EFB_WIDTH * scale_; efb_h_ = EFB_HEIGHT * scale_;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:efb_w_ height:efb_h_ mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    efb_color_ = [device_ newTextureWithDescriptor:td];
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:efb_w_ height:efb_h_ mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    efb_depth_ = [device_ newTextureWithDescriptor:dd];
    efb_needs_clear_ = true;
    host::log("metal: internal resolution %dx%d (EFB x%d, drawable %dx%d)", efb_w_, efb_h_, scale_, client_w_, client_h_);
  }

  // MetalFX spatial upscaling (option 1/2): the EFB copy is blitted at its internal
  // resolution into the scaler's input, reconstructed to the presented content size,
  // and composited 1:1. Only an upscale is valid; anything else falls back to the
  // plain blit. Quality 1 = balanced, 2 = quality (the scaler's two presets).
  void init_fx_support() {
#if defined(MELEE_HAS_METALFX)
    if (@available(macOS 13.0, iOS 16.0, *))
      fx_supported_ = [MTLFXSpatialScalerDescriptor supportsDevice:device_];
#endif
    if (opts_.upscaler && !fx_supported_) host::log("metal: MetalFX unavailable on this GPU; upscaler off");
  }

  bool fx_rebuild(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h) {
    const int quality = std::clamp(opts_.upscaler, 1, 2);
    if (fx_scaler_ && quality == fx_quality_ && in_w == fx_in_w_ && in_h == fx_in_h_ && out_w == fx_out_w_ && out_h == fx_out_h_) return true;
#if !defined(MELEE_HAS_METALFX)
    (void)quality;
    return false;
#else
    if (!(@available(macOS 13.0, iOS 16.0, *))) return false;
    @try {
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:in_w height:in_h mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      fx_in_ = [device_ newTextureWithDescriptor:td];
      MTLTextureDescriptor* od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
      od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
      od.storageMode = MTLStorageModePrivate;
      fx_out_ = [device_ newTextureWithDescriptor:od];
      MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
      d.inputWidth = in_w; d.inputHeight = in_h;
      d.outputWidth = out_w; d.outputHeight = out_h;
      d.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      d.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      // Quality presets (0 balanced / 1 quality) shipped with the macOS 13 API and were
      // dropped from later SDKs: apply the value when the OS still accepts it.
      @try { [d setValue:@(quality - 1) forKey:@"scalerQuality"]; } @catch (NSException*) {}
      id<MTLFXSpatialScaler> scaler = [d newSpatialScalerWithDevice:device_];
      if (!scaler) {
        if (!fx_reported_) { fx_reported_ = true; host::log("metal: MetalFX scaler unavailable; upscaler off"); }
        fx_supported_ = false; fx_scaler_ = nil;
        return false;
      }
      fx_scaler_ = scaler;
      fx_in_w_ = in_w; fx_in_h_ = in_h; fx_out_w_ = out_w; fx_out_h_ = out_h; fx_quality_ = quality;
      return true;
    } @catch (NSException*) {
      if (!fx_reported_) { fx_reported_ = true; host::log("metal: MetalFX scaler creation failed; upscaler off"); }
      fx_supported_ = false; fx_scaler_ = nil;
      return false;
    }
#endif
  }

  // Opens and closes the MetalFX input pass and encodes the scaler, when the option is
  // active and the geometry allows an upscale. Returns false to use the plain blit.
  bool fx_present(const EfbCopy& c, const host::GameRect& gr) {
    if (!fx_wanted() || !c.src_w || !c.src_h) return false;
    const uint32_t in_w = c.src_w * scale_, in_h = c.src_h * scale_;
    const uint32_t out_w = (uint32_t)std::lround(gr.w), out_h = (uint32_t)std::lround(gr.h);
    if (!in_w || !in_h || !out_w || !out_h) return false;
    if (in_w > out_w || in_h > out_h) {   // the EFB already exceeds the window: a plain downsample beats the scaler
      if (!fx_reported_) { fx_reported_ = true; host::log("metal: EFB %ux%u exceeds the %ux%u content; upscaler idle", in_w, in_h, out_w, out_h); }
      return false;
    }
    if (!fx_rebuild(in_w, in_h, out_w, out_h)) return false;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = fx_in_;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [command_ renderCommandEncoderWithDescriptor:rp];
    BlitConstants bc{{(float)c.src_w / EFB_WIDTH, (float)c.src_h / EFB_HEIGHT, (float)c.src_x / EFB_WIDTH, (float)c.src_y / EFB_HEIGHT},
                     {1.0f / std::max((float)efb_w_, 1.0f), 1.0f / std::max((float)efb_h_, 1.0f), 0.0f, 0.0f},
                     {1, 1, 0, 0}};
    bc.box[0] = 1; bc.box[1] = 1;   // no downsampling: the scaler wants the full detail
    [enc setRenderPipelineState:blit_present_];
    [enc setViewport:MTLViewport{0, 0, (double)in_w, (double)in_h, 0, 1}];
    [enc setVertexBytes:&bc length:sizeof bc atIndex:0];
    [enc setFragmentBytes:&bc length:sizeof bc atIndex:0];
    [enc setFragmentTexture:efb_color_ atIndex:0];
    [enc setFragmentSamplerState:blit_sampler_ atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
#if defined(MELEE_HAS_METALFX)
    fx_scaler_.colorTexture = fx_in_;
    fx_scaler_.inputContentWidth = fx_in_w_;
    fx_scaler_.inputContentHeight = fx_in_h_;
    fx_scaler_.outputTexture = fx_out_;
    [fx_scaler_ encodeToCommandBuffer:command_];
#endif
    return true;
  }

  void wait_idle() {
    for (int i = 0; i < FRAME_SLOTS; ++i) dispatch_semaphore_wait(frame_semaphore_, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < FRAME_SLOTS; ++i) dispatch_semaphore_signal(frame_semaphore_);
  }

  // ---- render passes
  void ensure_efb_pass() {
    if (encoder_) return;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = efb_color_;
    rp.colorAttachments[0].loadAction = efb_needs_clear_ ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = efb_depth_;
    rp.depthAttachment.loadAction = efb_needs_clear_ ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth = 0.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    efb_needs_clear_ = false;
    encoder_ = [command_ renderCommandEncoderWithDescriptor:rp];
    [encoder_ setFrontFacingWinding:winding_ccw_ ? MTLWindingCounterClockwise : MTLWindingClockwise];
    if (z_clamp_) [encoder_ setDepthClipMode:MTLDepthClipModeClamp];
    [encoder_ setVertexBuffer:vertex_ring_[slot_].buffer offset:0 atIndex:0];
    bound_pipeline_ = nil; bound_depth_ = nil; bound_cull_ = -1;
  }
  void end_pass() {
    if (!encoder_) return;
    [encoder_ endEncoding];
    encoder_ = nil;
  }

  // ---- pipelines
  id<MTLDepthStencilState> depth_state(uint32_t zmode) {
    auto it = depth_states_.find(zmode);
    if (it != depth_states_.end()) return it->second;
    static const MTLCompareFunction cmp[] = {MTLCompareFunctionNever, MTLCompareFunctionGreater, MTLCompareFunctionEqual, MTLCompareFunctionGreaterEqual,
                                             MTLCompareFunctionLess, MTLCompareFunctionNotEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionAlways};
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    const bool enable = bits(zmode, 0, 1);
    d.depthCompareFunction = enable ? cmp[bits(zmode, 1, 3)] : MTLCompareFunctionAlways;
    d.depthWriteEnabled = enable && bits(zmode, 4, 1);
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:d];
    depth_states_[zmode] = state;
    return state;
  }

  // A pipeline is looked up by (shader hashes, blend, depth, EFB format, topology). Misses are
  // compiled in the background and the draw is skipped until the state arrives (a frame or two,
  // the first time a stage or effect is seen); with MELEE_METAL_SYNC_COMPILE=1, or when frames
  // are being captured, the compile happens inline so output is deterministic.
  id<MTLRenderPipelineState> get_pipeline(const DrawCall& dc, MTLPrimitiveType prim) {
    if (dc.cached_pipeline && dc.cached_pipeline_owner == backend_id_) return (__bridge id<MTLRenderPipelineState>)dc.cached_pipeline;
    VSUid vsu = make_vs_uid(dc);
    PSUid psu = make_ps_uid(dc);
    const uint64_t vh = vsu.hash(), ph = psu.hash();
    const uint32_t topology = prim == MTLPrimitiveTypeLine ? 1 : 0;
    PsoKey key{vh, ph, dc.bp.blendmode() & 0xFFFF, dc.bp.zmode() & 0x1F, dc.bp.zcontrol() & 7, topology};
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
      dc.cached_pipeline_owner = backend_id_; dc.cached_pipeline = (__bridge void*)it->second;
      return it->second;
    }
    if (failed_.count(key)) return nil;   // a pipeline that would not build: skip the draw, do not retry every frame
    if (async_compile_) {
      if (!pending_.count(key)) { pending_.insert(key); start_compile_job(key, vsu, psu, compile_queue()); }
      return nil;
    }
    id<MTLFunction> vs = shader_function(true, vh, vsu, psu);
    id<MTLFunction> ps = shader_function(false, ph, vsu, psu);
    const double t0 = host::now_seconds();
    id<MTLRenderPipelineState> state = build_pipeline(key, vs, ps);
    pso_ms_ += (host::now_seconds() - t0) * 1000.0; ++psos_;
    if (!state) { failed_.insert(key); return nil; }
    record_pipeline(key, vsu, psu);
    pipelines_[key] = state;
    dc.cached_pipeline_owner = backend_id_; dc.cached_pipeline = (__bridge void*)state;
    return state;
  }
  MTLRenderPipelineDescriptor* make_descriptor(const PsoKey& key, id<MTLFunction> vs, id<MTLFunction> ps) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs; d.fragmentFunction = ps;
    d.vertexDescriptor = vertex_descriptor_;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.inputPrimitiveTopology = key.topology ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
    auto* ca = d.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatRGBA8Unorm;
    const uint32_t bm = key.blend;
    const bool alpha_in_efb = key.pixel_format == 1;
    static const MTLBlendFactor src_factors[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
                                                 MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha};
    static const MTLBlendFactor dst_factors[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
                                                 MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha};
    MTLBlendFactor s = src_factors[bits(bm, 8, 3)], dst = dst_factors[bits(bm, 5, 3)];
    if (!alpha_in_efb) {
      if (s == MTLBlendFactorDestinationAlpha) s = MTLBlendFactorOne; if (s == MTLBlendFactorOneMinusDestinationAlpha) s = MTLBlendFactorZero;
      if (dst == MTLBlendFactorDestinationAlpha) dst = MTLBlendFactorOne; if (dst == MTLBlendFactorOneMinusDestinationAlpha) dst = MTLBlendFactorZero;
    }
    ca.blendingEnabled = bits(bm, 0, 1);
    ca.sourceRGBBlendFactor = s; ca.destinationRGBBlendFactor = dst;
    ca.rgbBlendOperation = bits(bm, 11, 1) ? MTLBlendOperationReverseSubtract : MTLBlendOperationAdd;
    ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationAlphaBlendFactor = MTLBlendFactorZero; ca.alphaBlendOperation = MTLBlendOperationAdd;
    ca.writeMask = (bits(bm, 3, 1) ? (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue) : 0) | (bits(bm, 4, 1) ? MTLColorWriteMaskAlpha : 0);
    return d;
  }
  id<MTLRenderPipelineState> build_pipeline(const PsoKey& key, id<MTLFunction> vs, id<MTLFunction> ps) {
    if (!vs || !ps) return nil;
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:make_descriptor(key, vs, ps) error:&error];
    if (!state) host::log("metal: pipeline build failed: %s", error.localizedDescription.UTF8String);
    return state;
  }

  // ---- shader functions: one cache shared by the inline path (render thread) and the compile queues.
  id<MTLFunction> shader_function(bool vertex, uint64_t hash, const VSUid& vsu, const PSUid& psu) {
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      auto& cache = vertex ? vs_functions_ : ps_functions_;
      auto it = cache.find(hash);
      if (it != cache.end()) return it->second;
    }
    bool early = false;
    id<MTLFunction> f = vertex ? compile(msl::vertex(generate_vertex_shader(vsu), vsu.numTexGens), "vs_main", "vertex shader", hash, false)
                               : compile(msl::pixel(generate_pixel_shader(psu), psu.numTexGens, &early), "ps_main", "pixel shader", hash, false);
    std::lock_guard<std::mutex> lock(async_mutex_);
    (vertex ? vs_functions_ : ps_functions_)[hash] = f;
    return f;
  }
  // ---- background compilation
  dispatch_queue_t compile_queue() {   // in-game misses: concurrent, user-initiated
    if (!compile_queue_) compile_queue_ = dispatch_queue_create("app.dashdance.metal.compile", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
    return compile_queue_;
  }
  dispatch_queue_t precompile_queue() {   // the previous session's list at boot: serial, so it never floods the thread pool
    if (!precompile_queue_) precompile_queue_ = dispatch_queue_create("app.dashdance.metal.precompile", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    return precompile_queue_;
  }
  void start_compile_job(const PsoKey& key, const VSUid& vsu, const PSUid& psu, dispatch_queue_t queue) {
    __block MetalBackend* self_ = this;   // the destructor drains both queues before this pointer dies
    const PsoKey k = key; const VSUid v = vsu; const PSUid ps_uid = psu;
    dispatch_async(queue, ^{
      const double t0 = host::now_seconds();
      id<MTLFunction> vs = self_->shader_function(true, k.vs, v, ps_uid);
      id<MTLFunction> ps = self_->shader_function(false, k.ps, v, ps_uid);
      id<MTLRenderPipelineState> state = self_->build_pipeline(k, vs, ps);
      if (state) self_->record_pipeline(k, v, ps_uid);   // off the render thread, and only for pipelines that build
      std::lock_guard<std::mutex> lock(self_->async_mutex_);
      self_->ready_.push_back({k, state});
      self_->async_ms_ += (host::now_seconds() - t0) * 1000.0; ++self_->async_done_;
    });
  }
  // Render thread, once per frame: adopt finished pipelines.
  void drain_ready() {
    std::vector<std::pair<PsoKey, id<MTLRenderPipelineState>>> ready;
    { std::lock_guard<std::mutex> lock(async_mutex_); ready.swap(ready_); }
    for (auto& r : ready) { if (r.second) pipelines_[r.first] = r.second; else failed_.insert(r.first); pending_.erase(r.first); }
    if (frame_counter_ % 60 == 0) {
      unsigned done; double ms;
      { std::lock_guard<std::mutex> lock(async_mutex_); done = async_done_; ms = async_ms_; async_done_ = 0; async_ms_ = 0; }
      if (done) host::log("metal: %u pipelines compiled in the background (%.0f ms of compiler time, %zu still pending)", done, ms, pending_.size());
    }
  }
  // Every pipeline ever needed is appended to <cache_dir>/pipelines.bin; the next launch compiles
  // the whole list in the background while the game boots, so a stage seen once never stalls again.
  void record_pipeline(const PsoKey& key, const VSUid& vsu, const PSUid& psu) {
    if (opts_.cache_dir.empty()) return;
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (!known_.insert(key).second) return;
    const std::string path = opts_.cache_dir + "/pipelines.bin";
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return;
    if (f.tellp() == 0) { const uint32_t header[3] = {0x4C504D31u, (uint32_t)sizeof(VSUid), (uint32_t)sizeof(PSUid)}; f.write((const char*)header, sizeof header); }
    const uint32_t fields[4] = {key.blend, key.zmode, key.pixel_format, key.topology};
    f.write((const char*)fields, sizeof fields);
    f.write((const char*)&vsu, sizeof vsu);
    f.write((const char*)&psu, sizeof psu);
  }
  void precompile_from_cache() {
    if (opts_.cache_dir.empty() || !async_compile_) return;
    std::ifstream f(opts_.cache_dir + "/pipelines.bin", std::ios::binary);
    if (!f) return;
    uint32_t header[3] = {};
    f.read((char*)header, sizeof header);
    if (!f || header[0] != 0x4C504D31u || header[1] != sizeof(VSUid) || header[2] != sizeof(PSUid)) { host::log("metal: ignoring an incompatible pipeline cache"); return; }
    size_t count = 0;
    for (;;) {
      uint32_t fields[4]; VSUid vsu; PSUid psu;
      f.read((char*)fields, sizeof fields); f.read((char*)&vsu, sizeof vsu); f.read((char*)&psu, sizeof psu);
      if (!f) break;
      PsoKey key{vsu.hash(), psu.hash(), fields[0], fields[1], fields[2], fields[3]};
      { std::lock_guard<std::mutex> lock(record_mutex_); if (!known_.insert(key).second) continue; }
      pending_.insert(key);
      start_compile_job(key, vsu, psu, precompile_queue());
      ++count;
    }
    if (count) host::log("metal: compiling %zu pipelines from the previous session in the background", count);
  }

  id<MTLFunction> compile(const std::string& source, const char* entry, const char* what, uint64_t hash, bool count = true) {
    NSError* error = nil;
    if (const char* dir = std::getenv("MELEE_MSL_DUMP")) {   // developer aid: every compiled shader as a file
      std::ofstream dump(std::string(dir) + "/" + what + "_" + std::to_string(hash) + ".metal");
      dump << source;
    }
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    const double t0 = host::now_seconds();
    id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (count) { compile_ms_ += (host::now_seconds() - t0) * 1000.0; ++compiles_; }
    if (!lib) {
      host::log("metal: %s %016llX failed: %s", what, (unsigned long long)hash, error.localizedDescription.UTF8String);
      if (shader_failures_++ < 4) {
        std::ofstream dump("metal_shader_fail_" + std::to_string(hash) + ".metal");
        dump << source;
      }
      return nil;
    }
    return [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
  }
  // ---- textures and samplers
  id<MTLTexture> get_texture(const TextureRef& t) {
    auto ec = efb_copies_.find(t.addr);
    if (ec != efb_copies_.end() && ec->second.texture) { ec->second.last_used = frame_counter_; return ec->second.texture; }
    if (!t.data) return nil;
    const uint32_t meta[] = {t.width, t.height, t.format, t.mip_levels, t.tlut_format};
    const uint64_t key = t.data->hash ^ hash_bytes(meta, sizeof meta);
    auto it = textures_.find(key);
    if (it != textures_.end()) { it->second.last_used = frame_counter_; return it->second.texture; }
    host::SimCostScope texture_cost(host::SIM_TEXTURE);
    TextureEntry e;
    e.width = t.width; e.height = t.height; e.levels = std::max(1u, t.mip_levels); e.last_used = frame_counter_;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:t.width height:t.height mipmapped:e.levels > 1];
    td.mipmapLevelCount = e.levels;
    td.usage = MTLTextureUsageShaderRead;
    // Private storage: the GPU keeps the texture in its optimal (compressed, tiled) layout, which is what
    // makes sampling cheap on Apple GPUs. Levels are decoded into a write-combined staging buffer and
    // blitted in an upload command buffer that is committed ahead of the frame's.
    td.storageMode = MTLStorageModePrivate;
    e.texture = [device_ newTextureWithDescriptor:td];
    uint32_t lw = t.width, lh = t.height;
    const uint8_t* level_src = t.data->image.data();
    const size_t available = t.data->image.size();
    size_t consumed = 0;
    id<MTLBlitCommandEncoder> blit = nil;
    for (uint32_t l = 0; l < e.levels && lw && lh; ++l) {
      const uint32_t bytes = texture_level_bytes(lw, lh, t.format);
      if (consumed + bytes > available) break;
      decode_texture(level_src, lw, lh, t.format, t.data->palette.data(), t.tlut_format, decode_scratch_);
      const size_t level_bytes = (size_t)lw * lh * 4;
      id<MTLBuffer> staging = [device_ newBufferWithBytes:decode_scratch_.data() length:level_bytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
      if (!upload_cb_) upload_cb_ = [queue_ commandBuffer];
      if (!blit) blit = [upload_cb_ blitCommandEncoder];
      [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:lw * 4 sourceBytesPerImage:level_bytes sourceSize:MTLSizeMake(lw, lh, 1)
                 toTexture:e.texture destinationSlice:0 destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, 0)];
      level_src += bytes; consumed += bytes;
      lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
    }
    if (blit) [blit endEncoding];
    id<MTLTexture> tex = e.texture;
    textures_[key] = std::move(e);
    return tex;
  }

  id<MTLSamplerState> get_sampler(uint32_t m0, uint32_t m1) {
    const uint64_t key = (uint64_t)m0 << 32 | m1;
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    static const MTLSamplerAddressMode wrap[] = {MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeRepeat};
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.sAddressMode = wrap[bits(m0, 0, 2)]; sd.tAddressMode = wrap[bits(m0, 2, 2)]; sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    const bool mag_linear = bits(m0, 4, 1);
    const uint32_t minf = bits(m0, 5, 3);
    const bool min_linear = minf & 4;
    const uint32_t mip = minf & 3;
    sd.minFilter = min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped : mip == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.lodMinClamp = bits(m1, 0, 8) / 16.0f;
    sd.lodMaxClamp = mip ? bits(m1, 8, 8) / 16.0f : 0.0f;
    if (opts_.anisotropy > 1 && min_linear && mag_linear) sd.maxAnisotropy = std::clamp(opts_.anisotropy, 1, 16);
    sd.normalizedCoordinates = YES;
    id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:sd];
    samplers_[key] = state;
    return state;
  }

  // ---- commands
  void execute_draw(const Frame& frame, const DrawCall& dc, const DrawMatrices* override_matrices) {
    auto& idx = index_scratch_; idx.clear();
    const uint32_t n = dc.vertex_count;
    MTLPrimitiveType prim = MTLPrimitiveTypeTriangle;
    switch (dc.primitive) {
      case 0x80: case 0x88:
        for (uint32_t i = 0; i + 3 < n; i += 4) idx.insert(idx.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
        break;
      case 0x90:
        for (uint32_t i = 0; i + 2 < n; i += 3) idx.insert(idx.end(), {i, i + 1, i + 2});
        break;
      case 0x98:
        for (uint32_t i = 2; i < n; ++i) { if (i & 1) idx.insert(idx.end(), {i - 1, i - 2, i}); else idx.insert(idx.end(), {i - 2, i - 1, i}); }
        break;
      case 0xA0:
        for (uint32_t i = 2; i < n; ++i) idx.insert(idx.end(), {0, i - 1, i});
        break;
      case 0xA8:
        prim = MTLPrimitiveTypeLine;
        for (uint32_t i = 0; i + 1 < n; i += 2) idx.insert(idx.end(), {i, i + 1});
        break;
      case 0xB0:
        prim = MTLPrimitiveTypeLine;
        for (uint32_t i = 1; i < n; ++i) idx.insert(idx.end(), {i - 1, i});
        break;
      default:
        return;   // points are not drawn
    }
    if (idx.empty()) return;
    // Viewport and scissor first: an empty scissor skips the whole draw.
    const float* vp = (const float*)&dc.xf_regs[0x1A];
    const float s = (float)scale_;
    float X = (vp[3] - vp[0] - 342.0f) * s, Y = (vp[4] + vp[1] - 342.0f) * s, W = 2.0f * vp[0] * s, H = -2.0f * vp[1] * s;
    if (W < 0) { X += W; W = -W; }
    if (H < 0) { Y += H; H = -H; }
    float min_depth = 1.0f - vp[5] / 16777216.0f, max_depth = 1.0f - (vp[5] - vp[2]) / 16777216.0f;
    min_depth = std::clamp(min_depth, 0.0f, 1.0f); max_depth = std::clamp(max_depth, 0.0f, 1.0f);
    if (max_depth < min_depth) std::swap(min_depth, max_depth);
    const uint32_t tl = dc.bp.reg[BP_SCISSORTL], br = dc.bp.reg[BP_SCISSORBR], so = dc.bp.reg[BP_SCISSOROFFSET];
    const int xoff = (int)bits(so, 0, 10) * 2 - 342, yoff = (int)bits(so, 10, 10) * 2 - 342;
    int sl = (int)bits(tl, 12, 12) - xoff - 342, st = (int)bits(tl, 0, 12) - yoff - 342;
    int sr = (int)bits(br, 12, 12) - xoff - 341, sb = (int)bits(br, 0, 12) - yoff - 341;
    sl = std::clamp(sl, 0, EFB_WIDTH); sr = std::clamp(sr, 0, EFB_WIDTH); st = std::clamp(st, 0, EFB_HEIGHT); sb = std::clamp(sb, 0, EFB_HEIGHT);
    if (sr <= sl || sb <= st) return;

    uint8_t* vcpu; size_t voffset;
    const Vertex* vsrc = (override_matrices && override_matrices->vertices) ? override_matrices->vertices : &frame.vertices[dc.first_vertex];
    size_t ioffset = 0;
    if (no_index_) {
      // Diagnostic non-indexed path: the vertex stream is expanded through the index list.
      const size_t vbytes = idx.size() * sizeof(Vertex);
      if (!vertex_ring_[slot_].alloc(vbytes, 256, &vcpu, &voffset)) { if (!ring_full_logged_) { host::log("metal: vertex ring full"); ring_full_logged_ = true; } return; }
      Vertex* vdst = (Vertex*)vcpu;
      for (size_t i = 0; i < idx.size(); ++i) vdst[i] = vsrc[idx[i]];
    } else {
      const size_t vbytes = (size_t)n * sizeof(Vertex);
      if (!vertex_ring_[slot_].alloc(vbytes, 256, &vcpu, &voffset)) { if (!ring_full_logged_) { host::log("metal: vertex ring full"); ring_full_logged_ = true; } return; }
      std::memcpy(vcpu, vsrc, vbytes);
      uint8_t* icpu;
      if (!index_ring_[slot_].alloc(idx.size() * 4, 256, &icpu, &ioffset)) { if (!ring_full_logged_) { host::log("metal: index ring full"); ring_full_logged_ = true; } return; }
      std::memcpy(icpu, idx.data(), idx.size() * 4);
    }
    uint8_t* ccpu; size_t vs_offset, ps_offset;
    if (!constant_ring_[slot_].alloc(sizeof(VSConstants), 256, &ccpu, &vs_offset)) { if (!ring_full_logged_) { host::log("metal: constant ring full"); ring_full_logged_ = true; } return; }
    VSConstants vs_constants;
    fill_vs_constants(dc, vs_constants, scale_, override_matrices);
    std::memcpy(ccpu, &vs_constants, sizeof vs_constants);
    if (!constant_ring_[slot_].alloc(sizeof(PSConstants), 256, &ccpu, &ps_offset)) return;
    PSConstants ps_constants;
    fill_ps_constants(dc, ps_constants, scale_);
    std::memcpy(ccpu, &ps_constants, sizeof ps_constants);

    ++draw_index_;
    if (max_draws_ && draw_index_ > max_draws_) return;   // MELEE_METAL_MAXDRAWS: bisect a broken frame
    if (trace_interval_ && frame_counter_ % trace_interval_ == 1 && trace_draws_++ < 600) {
      uint8_t pm_lo = 255, pm_hi = 0; float px[3] = {vsrc[0].pos[0], vsrc[0].pos[1], vsrc[0].pos[2]};
      for (uint32_t i = 0; i < n; ++i) { pm_lo = std::min(pm_lo, vsrc[i].posmtx); pm_hi = std::max(pm_hi, vsrc[i].posmtx); }
      const float* T = &vs_constants.transformmatrices[pm_lo * 3][0];
      const float* P = &vs_constants.projection[0][0];
      host::log("trace f%llu d%u prim=%u n=%u idx=%zu posmtx=%u..%u v0=(%.2f %.2f %.2f) vp=(%.0f %.0f %.0f %.0f z=%.4f..%.4f) sc=(%d %d %d %d) zmode=%02X blend=%08X "
                "proj=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f] T0=[%.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f]",
                (unsigned long long)frame_counter_, trace_draws_, (unsigned)prim, n, idx.size(), pm_lo, pm_hi, px[0], px[1], px[2], X, Y, W, H, min_depth, max_depth,
                sl, st, sr, sb, dc.bp.zmode() & 0x1F, dc.bp.reg[BP_BLENDMODE],
                P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15],
                T[0], T[1], T[2], T[3], T[4], T[5], T[6], T[7], T[8], T[9], T[10], T[11]);
    }
    id<MTLRenderPipelineState> pipeline = get_pipeline(dc, prim);
    if (!pipeline) return;
    ensure_efb_pass();
    if (pipeline != bound_pipeline_) { [encoder_ setRenderPipelineState:pipeline]; bound_pipeline_ = pipeline; }
    id<MTLDepthStencilState> depth = depth_state(no_depth_ ? 0u : (dc.bp.zmode() & 0x1F));
    if (depth != bound_depth_) { [encoder_ setDepthStencilState:depth]; bound_depth_ = depth; }
    static const MTLCullMode cull_modes[] = {MTLCullModeNone, MTLCullModeBack, MTLCullModeFront, MTLCullModeBack};
    const int cull = cull_none_ ? 0 : (dc.bp.cullmode() & 3);
    if (cull != bound_cull_) { [encoder_ setCullMode:cull_modes[cull]]; bound_cull_ = cull; }
    if (per_draw_buffers_) {
      // Diagnostic path: every draw gets its own freshly allocated buffers (no ring offsets at all).
      id<MTLBuffer> vb = [device_ newBufferWithBytes:vcpu length:(no_index_ ? idx.size() : n) * sizeof(Vertex) options:MTLResourceStorageModeShared];
      id<MTLBuffer> cb = [device_ newBufferWithBytes:&vs_constants length:sizeof vs_constants options:MTLResourceStorageModeShared];
      id<MTLBuffer> pb = [device_ newBufferWithBytes:&ps_constants length:sizeof ps_constants options:MTLResourceStorageModeShared];
      [encoder_ setVertexBuffer:vb offset:0 atIndex:0];
      [encoder_ setVertexBuffer:cb offset:0 atIndex:1];
      [encoder_ setFragmentBuffer:pb offset:0 atIndex:1];
      per_draw_keep_.push_back(vb); per_draw_keep_.push_back(cb); per_draw_keep_.push_back(pb);
    } else {
      // Always a full bind: the iOS Simulator's Metal shim mishandles setVertexBufferOffset: (draws
      // after the first read stale vertex data), and a full bind costs nothing on real devices.
      [encoder_ setVertexBuffer:vertex_ring_[slot_].buffer offset:voffset atIndex:0];
      [encoder_ setVertexBuffer:constant_ring_[slot_].buffer offset:vs_offset atIndex:1];
      [encoder_ setFragmentBuffer:constant_ring_[slot_].buffer offset:ps_offset atIndex:1];
    }
    id<MTLTexture> textures[8]; id<MTLSamplerState> samplers[8];
    for (int i = 0; i < 8; ++i) {
      textures[i] = dc.textures[i].used ? get_texture(dc.textures[i]) : nil;
      samplers[i] = dc.textures[i].used ? get_sampler(dc.textures[i].mode0, dc.textures[i].mode1) : blit_sampler_;
      if (!textures[i]) textures[i] = white_texture();
    }
    [encoder_ setFragmentTextures:textures withRange:NSMakeRange(0, 8)];
    [encoder_ setFragmentSamplerStates:samplers withRange:NSMakeRange(0, 8)];
    if (full_z_) { min_depth = 0.0f; max_depth = 1.0f; }
    MTLViewport viewport{X, Y, std::max(W, 1.0f), std::max(H, 1.0f), min_depth, max_depth};
    [encoder_ setViewport:viewport];
    MTLScissorRect scissor{(NSUInteger)(sl * scale_), (NSUInteger)(st * scale_), (NSUInteger)((sr - sl) * scale_), (NSUInteger)((sb - st) * scale_)};
    [encoder_ setScissorRect:scissor];
    if (no_index_) [encoder_ drawPrimitives:prim vertexStart:0 vertexCount:idx.size()];
    else if (per_draw_buffers_) {
      id<MTLBuffer> ib = [device_ newBufferWithBytes:idx.data() length:idx.size() * 4 options:MTLResourceStorageModeShared];
      per_draw_keep_.push_back(ib);
      [encoder_ drawIndexedPrimitives:prim indexCount:idx.size() indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];
    } else [encoder_ drawIndexedPrimitives:prim indexCount:idx.size() indexType:MTLIndexTypeUInt32 indexBuffer:index_ring_[slot_].buffer indexBufferOffset:ioffset];
    ++draws_this_frame_;
  }

  id<MTLTexture> white_texture() {
    if (white_) return white_;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    white_ = [device_ newTextureWithDescriptor:td];
    const uint8_t px[4] = {255, 255, 255, 255};
    [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
    return white_;
  }

  void clear_efb(const EfbCopy& c) {
    ensure_efb_pass();
    ClearConstants cc{};
    cc.color[0] = ((c.clear_color >> 16) & 0xFF) / 255.0f; cc.color[1] = ((c.clear_color >> 8) & 0xFF) / 255.0f;
    cc.color[2] = (c.clear_color & 0xFF) / 255.0f; cc.color[3] = ((c.clear_color >> 24) & 0xFF) / 255.0f;
    cc.depth = 1.0f - (float)c.clear_z / 16777215.0f;
    const uint32_t x0 = std::min<uint32_t>(c.src_x * scale_, efb_w_), y0 = std::min<uint32_t>(c.src_y * scale_, efb_h_);
    const uint32_t x1 = std::min<uint32_t>((c.src_x + c.src_w) * scale_, efb_w_), y1 = std::min<uint32_t>((c.src_y + c.src_h) * scale_, efb_h_);
    if (x1 <= x0 || y1 <= y0) return;
    [encoder_ setRenderPipelineState:clear_pipeline_];
    [encoder_ setDepthStencilState:clear_depth_state_];
    [encoder_ setCullMode:MTLCullModeNone];
    [encoder_ setViewport:MTLViewport{0, 0, (double)efb_w_, (double)efb_h_, 0, 1}];
    [encoder_ setScissorRect:MTLScissorRect{x0, y0, x1 - x0, y1 - y0}];
    [encoder_ setVertexBytes:&cc length:sizeof cc atIndex:0];
    [encoder_ setFragmentBytes:&cc length:sizeof cc atIndex:0];
    [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    bound_pipeline_ = nil; bound_depth_ = nil; bound_cull_ = -1;
  }

  void execute_copy(const EfbCopy& c) {
    end_pass();
    uint32_t w = c.src_w, h = c.src_h;
    if (c.half_scale) { w = std::max(1u, w / 2); h = std::max(1u, h / 2); }
    const uint32_t sw = w * scale_, sh = h * scale_;
    TextureEntry& e = efb_copies_[c.dest_addr];
    if (!e.texture || e.width != sw || e.height != sh) {
      MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:sw height:sh mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      e.texture = [device_ newTextureWithDescriptor:td];
      e.width = sw; e.height = sh; e.levels = 1;
    }
    e.last_used = frame_counter_;
    if (c.half_scale) {
      MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = e.texture;
      rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> enc = [command_ renderCommandEncoderWithDescriptor:rp];
      BlitConstants bc{{(float)c.src_w / EFB_WIDTH, (float)c.src_h / EFB_HEIGHT, (float)c.src_x / EFB_WIDTH, (float)c.src_y / EFB_HEIGHT},
                       {0, 0, 0, 0}, {1, 1, 0, 0}};
      [enc setRenderPipelineState:blit_copy_];
      [enc setVertexBytes:&bc length:sizeof bc atIndex:0];
      [enc setFragmentBytes:&bc length:sizeof bc atIndex:0];
      [enc setFragmentTexture:efb_color_ atIndex:0];
      [enc setFragmentSamplerState:blit_sampler_ atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [enc endEncoding];
    } else {
      const uint32_t x = std::min<uint32_t>(c.src_x * scale_, efb_w_), y = std::min<uint32_t>(c.src_y * scale_, efb_h_);
      const uint32_t cw = std::min<uint32_t>(sw, efb_w_ - x), ch = std::min<uint32_t>(sh, efb_h_ - y);
      if (!cw || !ch) return;
      id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
      [blit copyFromTexture:efb_color_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(cw, ch, 1)
                  toTexture:e.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
    }
  }

  bool present_efb(const EfbCopy& c) {
    end_pass();
    // A second XFB copy in the same frame re-renders into the drawable already acquired.
    const bool again = drawable_ != nil;
    id<CAMetalDrawable> drawable = drawable_;
    if (!drawable) { host::SimCostScope wait_cost(host::SIM_DRAWABLE); drawable = [layer_ nextDrawable]; }   // blocks while the display still holds both drawables
    if (!drawable) return false;
    const float ww = (float)drawable.texture.width, wh = (float)drawable.texture.height;
    const float aspect = output_aspect();
    const host::GameRect gr = host::window_game_rect(ww, wh, aspect);   // shared with the touch layout and the letterbox artwork
    const float vw = gr.w, vh = gr.h;
    const bool fx = fx_present(c, gr);   // MetalFX reconstructs into fx_out_ before this pass
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = again ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.05, 0.15, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [command_ renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:MTLViewport{gr.x, gr.y, vw, vh, 0, 1}];
    BlitConstants bc{{(float)c.src_w / EFB_WIDTH, (float)c.src_h / EFB_HEIGHT, (float)c.src_x / EFB_WIDTH, (float)c.src_y / EFB_HEIGHT},
                     {1.0f / std::max((float)efb_w_, 1.0f), 1.0f / std::max((float)efb_h_, 1.0f), std::clamp(opts_.sharpness, 0.0f, 1.0f), 0.0f},
                     {1, 1, 0, 0}};
    if (fx) {
      // Composite the reconstructed content 1:1 (the scaler owns the sharpening).
      bc = {{1, 1, 0, 0}, {1.0f / fx_out_w_, 1.0f / fx_out_h_, 0.0f, 0.0f}, {1, 1, 0, 0}};
    } else {
      bc.box[0] = (float)std::clamp((int)std::lround((double)c.src_w * scale_ / std::max(vw, 1.0f)), 1, 4);
      bc.box[1] = (float)std::clamp((int)std::lround((double)c.src_h * scale_ / std::max(vh, 1.0f)), 1, 4);
    }
    [enc setRenderPipelineState:blit_present_];
    [enc setVertexBytes:&bc length:sizeof bc atIndex:0];
    [enc setFragmentBytes:&bc length:sizeof bc atIndex:0];
    [enc setFragmentTexture:(fx ? fx_out_ : efb_color_) atIndex:0];
    [enc setFragmentSamplerState:blit_sampler_ atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    draw_overlay(enc, ww, wh);
    [enc endEncoding];
    drawable_ = drawable;
    fx_presented_ = fx;
    last_present_ = c;
    return true;
  }

  struct OvShape { float rect[4]; float color[4]; float params[4]; uint32_t label; float label_w, label_h, pad; };
  struct OvConstants { float size[2]; float alpha; float pad; float labels[16][4]; };

  // Renders the button labels once with the bold system font into an R8 atlas.
  void create_overlay_atlas() {
    const int W = 2048, H = 128, font_px = 88;
    std::vector<uint8_t> pixels((size_t)W * H, 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), W, H, 8, W, gray, kCGImageAlphaNone);
    if (!ctx) { CGColorSpaceRelease(gray); host::log("metal: no CoreGraphics context for the overlay label atlas"); return; }
    CGFloat white[] = {1.0, 1.0};
    CGColorRef fg = CGColorCreate(gray, white);
    CGColorSpaceRelease(gray);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, font_px, nullptr);
    // Work in top-down coordinates: flip the CTM and the text matrix so glyphs stay upright.
    CGContextTranslateCTM(ctx, 0, H);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
    int x = 4;
    for (int i = 1; i < host::kOverlayLabelCount && i < 16; ++i) {
      NSDictionary* attrs = @{(id)kCTFontAttributeName: (__bridge id)font, (id)kCTForegroundColorAttributeName: (__bridge id)fg};
      NSAttributedString* text = [[NSAttributedString alloc] initWithString:[NSString stringWithUTF8String:host::kOverlayLabels[i]] attributes:attrs];
      CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
      CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
      const int w = (int)std::ceil(bounds.size.width) + 4, h = (int)std::ceil(bounds.size.height) + 4;
      if (x + w > W || h > H) { CFRelease(line); host::log("metal: overlay label '%s' does not fit the atlas", host::kOverlayLabels[i]); continue; }
      // Glyph box 2px below the top edge, 2px right of `x` (top-down space, flipped text matrix).
      CGContextSetTextPosition(ctx, x + 2 - bounds.origin.x, 2 + bounds.origin.y + bounds.size.height);
      CTLineDraw(line, ctx);
      CFRelease(line);
      overlay_labels_[i][0] = (float)x; overlay_labels_[i][1] = 0.0f; overlay_labels_[i][2] = (float)w; overlay_labels_[i][3] = (float)h;
      x += w + 4;
    }
    CFRelease(font); CFRelease(fg); CGContextRelease(ctx);
    std::vector<uint8_t>& flipped = pixels;   // already top-down
    if (const char* dump = std::getenv("MELEE_DUMP_ATLAS")) {
      std::ofstream out(dump, std::ios::binary);
      out << "P5\n" << W << " " << H << "\n255\n";
      out.write((const char*)flipped.data(), (std::streamsize)flipped.size());
    }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    overlay_atlas_ = [device_ newTextureWithDescriptor:td];
    [overlay_atlas_ replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:flipped.data() bytesPerRow:W];
  }
  // ASCII 32..126 rendered once with CoreText into an R8 atlas; per-glyph placement metrics kept for layout.
  struct Glyph { float u, v, w, h; float bearing_x, top; float advance; };
  void create_glyph_atlas() {
    const int W = 1024, H = 320, font_px = 40, cell_w = 48, cell_h = 60, per_row = W / cell_w;
    std::vector<uint8_t> pixels((size_t)W * H, 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), W, H, 8, W, gray, kCGImageAlphaNone);
    if (!ctx) { CGColorSpaceRelease(gray); host::log("metal: no CoreGraphics context for the glyph atlas"); return; }
    CGFloat white[] = {1.0, 1.0};
    CGColorRef fg = CGColorCreate(gray, white);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, font_px, nullptr);
    CGContextTranslateCTM(ctx, 0, H); CGContextScaleCTM(ctx, 1, -1);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));   // same flip as the label atlas: upright glyphs in top-down space
    glyph_ascent_ = (float)CTFontGetAscent(font);
    NSDictionary* attrs = @{(id)kCTFontAttributeName: (__bridge id)font, (id)kCTForegroundColorAttributeName: (__bridge id)fg};
    for (int code = 32; code < 127; ++code) {
      const int i = code - 32, cx = (i % per_row) * cell_w, cy = (i / per_row) * cell_h;
      unichar ch = (unichar)code;
      NSAttributedString* text = [[NSAttributedString alloc] initWithString:[NSString stringWithCharacters:&ch length:1] attributes:attrs];
      CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
      CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
      const double advance = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
      const int w = (int)std::ceil(bounds.size.width) + 2, h = (int)std::ceil(bounds.size.height) + 2;
      Glyph& gl = glyphs_[i];
      gl.advance = (float)advance; gl.bearing_x = (float)bounds.origin.x - 1; gl.top = (float)(bounds.origin.y + bounds.size.height) + 1;
      gl.u = (float)(cx + 1); gl.v = (float)(cy + 1); gl.w = (float)w; gl.h = (float)h;
      if (bounds.size.width <= 0 || bounds.size.height <= 0 || w > cell_w || h > cell_h) { gl.w = gl.h = 0; CFRelease(line); continue; }
      // Ink starts 1 px inside the box (top-down space, flipped text matrix), exactly like the label atlas.
      CGContextSetTextPosition(ctx, cx + 2 - bounds.origin.x, cy + 2 + bounds.origin.y + bounds.size.height);
      CTLineDraw(line, ctx);
      CFRelease(line);
    }
    CFRelease(fg);
    CFRelease(font); CGContextRelease(ctx); CGColorSpaceRelease(gray);
    size_t ink = 0; for (uint8_t v : pixels) ink += v > 0;
    host::log("metal: glyph atlas %dx%d, %zu lit texels, ascent %.1f, 'A' box %.0fx%.0f advance %.1f", W, H, ink, glyph_ascent_, glyphs_['A' - 32].w, glyphs_['A' - 32].h, glyphs_['A' - 32].advance);
    if (const char* dump = std::getenv("MELEE_DUMP_GLYPHS")) { std::ofstream out(dump, std::ios::binary); out << "P5\n" << W << " " << H << "\n255\n"; out.write((const char*)pixels.data(), (std::streamsize)pixels.size()); }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    glyph_atlas_ = [device_ newTextureWithDescriptor:td];
    [glyph_atlas_ replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:W];
    glyph_font_px_ = (float)font_px;
  }
  float text_width(const std::string& t, float size) const {
    const float k = size / glyph_font_px_; float w = 0;
    for (unsigned char c : t) if (c >= 32 && c < 127) w += glyphs_[c - 32].advance * k;
    return w;
  }
  void draw_texts(id<MTLRenderCommandEncoder> enc, float ww, float wh) {
    if (!glyph_atlas_ || overlay_frame_.texts.empty()) return;
    struct TxGlyphCpu { float rect[4]; float uv[4]; float color[4]; };
    std::vector<TxGlyphCpu> quads;
    for (const host::OverlayText& t : overlay_frame_.texts) {
      float size = t.size;
      if (t.max_w > 0.0f) { const float w = text_width(t.text, t.size); if (w > t.max_w) size = t.size * t.max_w / w; }   // fit, don't overflow
      const float k = size / glyph_font_px_;
      float x = t.x;
      if (t.align) { const float w = text_width(t.text, size); x -= t.align == 1 ? w * 0.5f : w; }
      const float baseline = t.y + (t.size - size) * 0.5f + glyph_ascent_ * k;
      for (unsigned char c : t.text) {
        if (c < 32 || c >= 127) continue;
        const Glyph& g = glyphs_[c - 32];
        if (g.w > 0) {
          const float gx = x + g.bearing_x * k, gy = baseline - g.top * k;
          quads.push_back({{gx, gy, gx + g.w * k, gy + g.h * k}, {g.u, g.v, g.w, g.h}, {t.r, t.g, t.b, t.a}});
        }
        x += g.advance * k;
      }
    }
    if (quads.empty()) return;
    if (!text_logged_) { text_logged_ = true; host::log("metal: text pass: %zu glyph quads, first at (%.0f,%.0f)-(%.0f,%.0f) uv (%.0f,%.0f,%.0f,%.0f)", quads.size(), quads[0].rect[0], quads[0].rect[1], quads[0].rect[2], quads[0].rect[3], quads[0].uv[0], quads[0].uv[1], quads[0].uv[2], quads[0].uv[3]); }
    struct TxCCpu { float size[2]; float atlas[2]; } tc{{ww, wh}, {(float)glyph_atlas_.width, (float)glyph_atlas_.height}};
    [enc setRenderPipelineState:text_pipeline_];
    id<MTLBuffer> gb = [device_ newBufferWithBytes:quads.data() length:quads.size() * sizeof(TxGlyphCpu) options:MTLResourceStorageModeShared];
    [enc setVertexBuffer:gb offset:0 atIndex:0];
    [enc setVertexBytes:&tc length:sizeof tc atIndex:1];
    [enc setFragmentBuffer:gb offset:0 atIndex:0];
    [enc setFragmentBytes:&tc length:sizeof tc atIndex:1];
    [enc setFragmentTexture:glyph_atlas_ atIndex:0];
    [enc setFragmentSamplerState:blit_sampler_ atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:quads.size()];
  }
  void draw_overlay(id<MTLRenderCommandEncoder> enc, float ww, float wh) {
    if (!overlay_ || !overlay_(overlay_frame_)) return;
    [enc setViewport:MTLViewport{0, 0, ww, wh, 0, 1}];
    draw_shapes(enc, ww, wh);
    draw_texts(enc, ww, wh);
  }
  void draw_shapes(id<MTLRenderCommandEncoder> enc, float ww, float wh) {
    if (overlay_frame_.shapes.empty()) return;
    static_assert(sizeof(OvShape) == 64, "overlay shape layout must match the shader");
    static_assert(sizeof(OvConstants) == 272, "overlay constants layout must match the shader");
    static_assert(host::kOverlayLabelCount <= 16, "the shader holds 16 label rects");
    std::vector<OvShape> shapes;
    shapes.reserve(overlay_frame_.shapes.size());
    for (const host::OverlayShape& s : overlay_frame_.shapes)
      shapes.push_back({{s.x0, s.y0, s.x1, s.y1}, {s.r, s.g, s.b, s.a}, {s.corner, s.ring, s.pressed, 0.0f}, s.label, s.label_w, s.label_h, 0.0f});
    OvConstants oc{{ww, wh}, overlay_frame_.alpha, 0.0f, {}};
    std::memcpy(oc.labels, overlay_labels_, sizeof oc.labels);
    [enc setRenderPipelineState:overlay_pipeline_];
    [enc setVertexBytes:shapes.data() length:shapes.size() * sizeof(OvShape) atIndex:0];
    [enc setVertexBytes:&oc length:sizeof oc atIndex:1];
    [enc setFragmentBytes:shapes.data() length:shapes.size() * sizeof(OvShape) atIndex:0];
    [enc setFragmentBytes:&oc length:sizeof oc atIndex:1];
    [enc setFragmentTexture:overlay_atlas_ atIndex:0];
    [enc setFragmentSamplerState:blit_sampler_ atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:shapes.size()];
  }


  void capture(const EfbCopy& c, const std::string& path) {
    // Reads the presented EFB region back through a shared texture (development aid).
    const uint32_t x = std::min<uint32_t>(c.src_x * scale_, efb_w_), y = std::min<uint32_t>(c.src_y * scale_, efb_h_);
    const uint32_t w = std::min<uint32_t>(c.src_w * scale_, efb_w_ - x), h = std::min<uint32_t>(c.src_h * scale_, efb_h_ - y);
    if (!w || !h) return;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> staging = [device_ newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:efb_color_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
                toTexture:staging destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    std::vector<uint8_t> pixels((size_t)w * h * 4);
    [staging getBytes:pixels.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i < (size_t)w * h; ++i) f.write((const char*)&pixels[i * 4], 3);
    host::log("captured %s (%ux%u)", path.c_str(), w, h);
  }

  void submit(const Frame& frame, const DrawMatrices* overrides) {
    host::SimCostScope render_cost(host::SIM_RENDER);   // the Metal backend encodes on the simulation thread
    trace_draws_ = 0; draw_index_ = 0;
    per_draw_keep_.clear();   // previous frames' diagnostic buffers (retained by their command buffers until completion)
    static thread_local bool qos_set = false;
    if (!qos_set) { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); qos_set = true; }   // render thread on performance cores
    @autoreleasepool {
      if (pick_scale() != scale_) { wait_idle(); efb_copies_.clear(); create_efb(); log_scale_change(); }   // window, ProMotion, thermal or device caps
      { host::SimCostScope wait_cost(host::SIM_GPUWAIT); dispatch_semaphore_wait(frame_semaphore_, DISPATCH_TIME_FOREVER); }   // blocks only when FRAME_SLOTS command buffers are still executing
      ++frame_counter_;
      if (frame_counter_ % 120 == 1) refresh_period_ms_ = 1000.0 / std::max(host::window_refresh_rate(), 1.0);   // display can change (window moved, ProMotion)
      slot_ = (int)(frame_counter_ % FRAME_SLOTS);
      vertex_ring_[slot_].used = index_ring_[slot_].used = constant_ring_[slot_].used = 0;
      ring_full_logged_ = false;
      draws_this_frame_ = 0;
      fx_presented_ = false;
      if (async_compile_) drain_ready();
      command_ = [queue_ commandBuffer];
      drawable_ = nil;
      bool presented = false;
      for (const FrameCommand& cmd : frame.commands) {
        if (cmd.kind == FrameCommand::Draw) {
          execute_draw(frame, frame.draws[cmd.index], overrides ? overrides + cmd.index : nullptr);
        } else {
          const EfbCopy& c = frame.copies[cmd.index];
          if (c.to_xfb) {
            if (!skip_present_ && present_efb(c)) presented = true;
            // Snapshot the presented EFB before the game's clear erases it:
            // encoding this after the loop reads the cleared EFB (all-black
            // captures). The blit joins this frame's command buffer, so it
            // cannot race the draws. In drain mode (hidden window) the frame
            // number keys on executed frames, which still tick one per retrace.
            if (!opts_.capture_path.empty() && !capture_staging_) {
              const uint64_t n = frame_counter_ + 1;   // executed frames: deterministic and 1:1 with retraces
              const bool wanted = (opts_.capture_frame && n == opts_.capture_frame) || (opts_.capture_every && n % opts_.capture_every == 0);
              if (wanted && command_) {
                  end_pass();   // a lazily-opened draw pass must close first (drain mode never presents)
                capture_bgra_ = false;
                uint32_t x = 0, y = 0, w = 0, h = 0;
                id<MTLTexture> source = nil;
                if (fx_presented_ && fx_out_) {   // capture the presented, upscaled content
                  capture_bgra_ = true;
                  source = fx_out_; w = fx_out_w_; h = fx_out_h_;
                } else {
                  x = std::min<uint32_t>(c.src_x * scale_, efb_w_); y = std::min<uint32_t>(c.src_y * scale_, efb_h_);
                  w = std::min<uint32_t>(c.src_w * scale_, efb_w_ - x); h = std::min<uint32_t>(c.src_h * scale_, efb_h_ - y);
                  source = efb_color_;
                }
                if (w && h) {
                  capture_w_ = w; capture_h_ = h;
                  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(capture_bgra_ ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm) width:w height:h mipmapped:NO];
                  td.storageMode = MTLStorageModeShared;
                  capture_staging_ = [device_ newTextureWithDescriptor:td];
                  capture_path_pending_ = opts_.capture_path;
                  capture_cmd_ = command_;
                  id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
                  [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
                              toTexture:capture_staging_ destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                  [blit endEncoding];
                }
              }
            }
          }
          else execute_copy(c);
          if (c.clear) clear_efb(c);
        }
      }
      end_pass();
      if (drawable_) {
        [command_ presentDrawable:drawable_];
        // Latency accounting: XFB copy on the simulation thread -> pixels on the panel. The
        // window average and worst are logged every 60 presented frames (MELEE_METAL_LATENCY=1).
#if !TARGET_OS_SIMULATOR   // presented-time APIs do not exist in the Simulator SDK
        if (latency_log_ || phase_lock_) {
          const double submitted = frame.time > 0.0 ? frame.time : host::now_seconds();
          const double render_start = CACurrentMediaTime();
          const double clock_offset = host::now_seconds() - render_start;   // presentedTime is on the CA clock
          __block MetalBackend* self_ = this;
          [drawable_ addPresentedHandler:^(id<MTLDrawable> d) {
            const double presented = d.presentedTime > 0.0 ? d.presentedTime : CACurrentMediaTime();
            self_->latency_note((presented + clock_offset - submitted) * 1000.0, (presented - render_start) * 1000.0);
          }];
        }
#endif
      }
      if (upload_cb_) { [upload_cb_ commit]; upload_cb_ = nil; }   // texture uploads land before the frame that samples them
      dispatch_semaphore_t semaphore = frame_semaphore_;
      __block MetalBackend* self_gpu = this;
      [command_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        dispatch_semaphore_signal(semaphore);
        self_gpu->gpu_note((cb.GPUEndTime - cb.GPUStartTime) * 1000.0, (cb.GPUStartTime - cb.kernelStartTime) * 1000.0);
      }];
      [command_ commit];
      if (capture_staging_) {
        [command_ waitUntilCompleted];
        [capture_cmd_ waitUntilCompleted];
        std::string path = capture_path_pending_;
        if (opts_.capture_every) {
          char suffix[32]; std::snprintf(suffix, sizeof suffix, "_%05llu.ppm", (unsigned long long)(frames_presented_ + 1));
          const bool ppm = path.size() > 4 && path.compare(path.size() - 4, 4, ".ppm") == 0;
          path = path.substr(0, ppm ? path.size() - 4 : path.size()) + suffix;
        }
        std::vector<uint8_t> pixels((size_t)capture_w_ * capture_h_ * 4);
        [capture_staging_ getBytes:pixels.data() bytesPerRow:capture_w_ * 4 fromRegion:MTLRegionMake2D(0, 0, capture_w_, capture_h_) mipmapLevel:0];
        if (capture_bgra_)
          for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);   // BGRA -> RGB order
        std::ofstream f(path, std::ios::binary);
        f << "P6\n" << capture_w_ << ' ' << capture_h_ << "\n255\n";
        for (size_t i = 0; i < (size_t)capture_w_ * capture_h_; ++i) f.write((const char*)&pixels[i * 4], 3);
        host::log("captured %s (%ux%u)", path.c_str(), capture_w_, capture_h_);
        capture_staging_ = nil;
        capture_cmd_ = nil;
      }
      command_ = nil; drawable_ = nil;
      if (compiles_ || psos_) {
        if (compile_ms_ + pso_ms_ >= 4.0 || compile_log_all_)
          host::log("metal: frame %llu compiled %u shaders (%.1f ms) and %u pipelines (%.1f ms) on the render path", (unsigned long long)frame_counter_, compiles_, compile_ms_, psos_, pso_ms_);
        compile_total_ms_ += compile_ms_ + pso_ms_; compile_ms_ = pso_ms_ = 0; compiles_ = psos_ = 0;
      }
      if (presented) ++frames_presented_;
      if (frame_counter_ % 600 == 0) evict();
    }
  }

  // Called from the command buffer completed handler (any thread): GPU execution time and the
  // scheduling gap between the kernel accepting the buffer and the GPU starting it.
  void gpu_note(double exec_ms, double queue_ms) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    gpu_sum_ += exec_ms; gpu_queue_sum_ += queue_ms; if (exec_ms > gpu_worst_) gpu_worst_ = exec_ms; ++gpu_count_;
  }
  // Called from the CAMetalDrawable presented handler (any thread).
  void latency_note(double sim_to_panel_ms, double render_to_panel_ms) {
    if (phase_lock_) host::present_feedback(sim_to_panel_ms, refresh_period_ms_);
    if (!latency_log_) return;
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latency_sum_ += sim_to_panel_ms; latency_render_sum_ += render_to_panel_ms;
    if (sim_to_panel_ms > latency_worst_) latency_worst_ = sim_to_panel_ms;
    if (++latency_count_ >= 60) {
      host::log("[latency] xfb-copy -> panel %.1f ms avg (worst %.1f), commit -> panel %.1f ms avg over %u frames | gpu %.1f ms avg (worst %.1f), queue %.1f ms",
                latency_sum_ / latency_count_, latency_worst_, latency_render_sum_ / latency_count_, latency_count_,
                gpu_sum_ / std::max(1u, gpu_count_), gpu_worst_, gpu_queue_sum_ / std::max(1u, gpu_count_));
      latency_sum_ = latency_render_sum_ = latency_worst_ = 0; latency_count_ = 0;
      gpu_sum_ = gpu_queue_sum_ = gpu_worst_ = 0; gpu_count_ = 0;
    }
  }
  std::mutex latency_mutex_;
  double refresh_period_ms_ = 1000.0 / 60.0;
  double latency_sum_ = 0, latency_render_sum_ = 0, latency_worst_ = 0; unsigned latency_count_ = 0;
  double gpu_sum_ = 0, gpu_queue_sum_ = 0, gpu_worst_ = 0; unsigned gpu_count_ = 0;
  const bool latency_log_ = [] { const char* e = std::getenv("MELEE_METAL_LATENCY"); return e && *e && *e != '0'; }();
  const bool phase_lock_ = [] { const char* e = std::getenv("MELEE_PHASE_LOCK"); return e && *e && *e != '0'; }();   // experimental, MELEE_PHASE_LOCK=1 enables

  void evict() {
    // Drop textures unused for ten seconds; the GPU work referencing them is long complete.
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (it->second.last_used + 600 < frame_counter_) it = textures_.erase(it); else ++it;
    }
  }

  CAMetalLayer* layer_;
  MetalOptions opts_;
  int client_w_, client_h_;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLCommandBuffer> command_ = nil, upload_cb_ = nil;
  id<MTLRenderCommandEncoder> encoder_ = nil;
  id<CAMetalDrawable> drawable_ = nil;
  dispatch_semaphore_t frame_semaphore_ = nullptr;
  Ring vertex_ring_[FRAME_SLOTS], index_ring_[FRAME_SLOTS], constant_ring_[FRAME_SLOTS];
  MTLVertexDescriptor* vertex_descriptor_ = nil;
  id<MTLRenderPipelineState> blit_present_ = nil, blit_copy_ = nil, clear_pipeline_ = nil, overlay_pipeline_ = nil;
  OverlayProvider overlay_;
  host::OverlayFrame overlay_frame_;
  uint64_t trace_interval_ = [] { const char* e = std::getenv("MELEE_METAL_TRACE"); return e ? (uint64_t)std::strtoull(e, nullptr, 0) : 0ull; }();
  uint32_t trace_draws_ = 0, draw_index_ = 0;
  uint32_t max_draws_ = [] { const char* e = std::getenv("MELEE_METAL_MAXDRAWS"); return e ? (uint32_t)std::strtoul(e, nullptr, 0) : 0u; }();
  bool winding_ccw_ = [] { const char* e = std::getenv("MELEE_METAL_WINDING"); return e && e[0] == 'c' && e[1] == 'c'; }();
  bool cull_none_ = [] { const char* e = std::getenv("MELEE_METAL_NOCULL"); return e && *e != '0'; }();
  bool no_depth_ = [] { const char* e = std::getenv("MELEE_METAL_NODEPTH"); return e && *e != '0'; }();
  bool per_draw_buffers_ = [] { const char* e = std::getenv("MELEE_METAL_PERDRAWBUF"); return e && *e != '0'; }();
  std::vector<id<MTLBuffer>> per_draw_keep_;
  bool z_clamp_ = [] { const char* e = std::getenv("MELEE_METAL_ZCLAMP"); return e && *e != '0'; }();
  bool full_z_ = [] { const char* e = std::getenv("MELEE_METAL_FULLZ"); return e && *e != '0'; }();
  bool no_index_ = [] { const char* e = std::getenv("MELEE_METAL_NOINDEX"); return e && *e != '0'; }();   // diagnostic
  id<MTLTexture> overlay_atlas_ = nil;
  float overlay_labels_[16][4] = {};
  id<MTLRenderPipelineState> text_pipeline_ = nil;
  id<MTLTexture> glyph_atlas_ = nil;
  Glyph glyphs_[96] = {};
  float glyph_ascent_ = 0, glyph_font_px_ = 40;
  bool text_logged_ = false;
  id<MTLDepthStencilState> clear_depth_state_ = nil;
  id<MTLSamplerState> blit_sampler_ = nil;
  id<MTLTexture> efb_color_ = nil, efb_depth_ = nil, white_ = nil;
#if defined(MELEE_HAS_METALFX)
  id<MTLFXSpatialScaler> fx_scaler_ = nil;   // macOS 13 / iOS 16 spatial upscaler (FSR-class)
#else
  void* fx_scaler_ = nullptr;
#endif
  id<MTLTexture> fx_in_ = nil, fx_out_ = nil;
  uint32_t fx_in_w_ = 0, fx_in_h_ = 0, fx_out_w_ = 0, fx_out_h_ = 0;
  int fx_quality_ = -1;
  bool fx_supported_ = false, fx_reported_ = false;
  bool efb_needs_clear_ = true, skip_present_ = false, ring_full_logged_ = false, pending_capture_ = false;
  int scale_ = 1, efb_w_ = EFB_WIDTH, efb_h_ = EFB_HEIGHT, slot_ = 0;
  uint64_t frame_counter_ = 0, frames_presented_ = 0, backend_id_ = (uint64_t)(uintptr_t)this;
  std::atomic<int> shader_failures_{0}; int draws_this_frame_ = 0;
  bool async_compile_ = [] { const char* e = std::getenv("MELEE_METAL_SYNC_COMPILE"); return !(e && *e && *e != '0'); }();
  dispatch_queue_t compile_queue_ = nullptr, precompile_queue_ = nullptr;
  std::mutex async_mutex_;                       // vs_functions_/ps_functions_, ready_, async counters
  std::mutex record_mutex_;                      // known_ and pipelines.bin
  std::vector<std::pair<PsoKey, id<MTLRenderPipelineState>>> ready_;
  unsigned async_done_ = 0; double async_ms_ = 0;
  std::unordered_set<PsoKey, PsoKeyHash> pending_, known_, failed_;
  double compile_ms_ = 0, pso_ms_ = 0, compile_total_ms_ = 0; unsigned compiles_ = 0, psos_ = 0;
  const bool compile_log_all_ = [] { const char* e = std::getenv("MELEE_METAL_COMPILE_LOG"); return e && *e && *e != '0'; }();
  id<MTLRenderPipelineState> bound_pipeline_ = nil;
  id<MTLDepthStencilState> bound_depth_ = nil;
  int bound_cull_ = -1;
  EfbCopy last_present_{};
  // Capture staging: the EFB is cleared after its XFB copy, so the blit is
  // encoded inside the frame (before the clear) and read back after commit.
  id<MTLTexture> capture_staging_ = nil;
  id<MTLCommandBuffer> capture_cmd_ = nil;
  std::string capture_path_pending_;
  uint32_t capture_w_ = 0, capture_h_ = 0;
  bool capture_bgra_ = false, fx_presented_ = false;
  std::unordered_map<PsoKey, id<MTLRenderPipelineState>, PsoKeyHash> pipelines_;
  std::unordered_map<uint64_t, id<MTLFunction>> vs_functions_, ps_functions_;
  std::unordered_map<uint32_t, id<MTLDepthStencilState>> depth_states_;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  std::unordered_map<uint32_t, TextureEntry> efb_copies_;
  std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
  std::vector<uint32_t> index_scratch_;
  std::vector<uint8_t> decode_scratch_;
};


// ---- Render thread. The simulation hands each frame to a queue and never waits on the GPU or the
// display: nextDrawable can block for 15-30 ms when the display pipeline holds both drawables
// (measured on a ProMotion MacBook Pro during refresh-rate changes), and on the simulation thread
// that was a dropped game frame every time. The worker executes every frame in order (EFB copies
// feed later frames) and presents the newest; when it falls behind it drains the backlog without
// presenting, so a display stall costs a shown frame, never a simulated one.
class MetalThreaded final : public Backend {
 public:
  explicit MetalThreaded(MetalBackend* inner) : inner_(inner) { worker_ = std::thread([this] { run(); }); }
  ~MetalThreaded() override { queue_.finish(); if (worker_.joinable()) worker_.join(); delete inner_; }
  void submit_frame(const Frame& frame) override { host::SimCostScope cost(host::SIM_QUEUE); if (!queue_.push(frame)) throw ExitRequested{host::exit_code()}; }
  void submit_and_recycle(Frame& frame) override { host::SimCostScope cost(host::SIM_QUEUE); if (!queue_.push_and_recycle(frame)) throw ExitRequested{host::exit_code()}; }
  // Control from other threads is applied by the worker between frames.
  void resize(int w, int h) { std::lock_guard<std::mutex> lock(pending_mutex_); pending_w_ = w; pending_h_ = h; pending_resize_ = true; }
  void set_options(const MetalOptions& o) { std::lock_guard<std::mutex> lock(pending_mutex_); pending_options_ = o; pending_options_set_ = true; }
  void set_overlay(OverlayProvider p) { std::lock_guard<std::mutex> lock(pending_mutex_); pending_overlay_ = std::move(p); pending_overlay_set_ = true; }
  uint64_t frames_presented() const { return frames_presented_.load(); }

 private:
  void apply_pending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_resize_) { inner_->resize(pending_w_, pending_h_); pending_resize_ = false; }
    if (pending_options_set_) { inner_->set_options(pending_options_); pending_options_set_ = false; }
    if (pending_overlay_set_) { inner_->set_overlay(std::move(pending_overlay_)); pending_overlay_set_ = false; }
  }
  void run() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    host::thread_realtime("render", 4.0);
    uint64_t drained = 0, executed = 0;
    try {
      Frame frame;
      for (;;) {
        if (!queue_.try_pop(frame)) {
          if (queue_.drained()) break;
          queue_.wait_available(std::chrono::milliseconds(2));
          continue;
        }
        apply_pending();
        const size_t backlog = queue_.size();
        if (backlog > 0) {
          inner_->set_skip_present(true); inner_->submit_frame(frame); inner_->set_skip_present(false);
          if (++drained == 1 || drained % 300 == 0) host::log("renderer: display stalled; drained a backlog of %zu frames without presenting (%llu so far)", backlog + 1, (unsigned long long)drained);
        } else {
          inner_->submit_frame(frame);
        }
        ++executed;
        frames_presented_.store(inner_->frames_presented());
        queue_.recycle(std::move(frame));
      }
    } catch (const std::exception& e) {
      host::log("renderer: fatal error on the render thread: %s", e.what());
      queue_.finish(true); host::request_exit(3);
    } catch (...) {
      host::log("renderer: fatal error on the render thread");
      queue_.finish(true); host::request_exit(3);
    }
    host::log("renderer: %llu frames executed on the render thread, %llu drained without presenting", (unsigned long long)executed, (unsigned long long)drained);
  }
  MetalBackend* inner_;
  FrameQueue queue_;
  std::thread worker_;
  std::atomic<uint64_t> frames_presented_{0};
  std::mutex pending_mutex_;
  bool pending_resize_ = false, pending_options_set_ = false, pending_overlay_set_ = false;
  int pending_w_ = 0, pending_h_ = 0;
  MetalOptions pending_options_;
  OverlayProvider pending_overlay_;
};
static std::mutex g_threaded_mutex;
static std::unordered_map<Backend*, MetalThreaded*> g_threaded;   // backends created with a render thread
static MetalThreaded* threaded(Backend* b) { std::lock_guard<std::mutex> lock(g_threaded_mutex); auto it = g_threaded.find(b); return it == g_threaded.end() ? nullptr : it->second; }

}  // namespace

Backend* create_metal_backend(void* layer, int w, int h, const MetalOptions& options) {
  MetalBackend* inner = new MetalBackend((__bridge CAMetalLayer*)layer, w, h, options);
  const char* env = std::getenv("MELEE_RENDER_THREAD");
  if (env && *env == '0') return inner;   // MELEE_RENDER_THREAD=0: render on the simulation thread (diagnostics)
  MetalThreaded* t = new MetalThreaded(inner);
  std::lock_guard<std::mutex> lock(g_threaded_mutex); g_threaded[t] = t;
  host::log("metal: rendering on its own thread; the simulation never waits for the display");
  return t;
}
void metal_scale_caps(int device_cap, int thermal_cap) { g_device_scale_cap.store(std::max(0, device_cap)); g_thermal_scale_cap.store(std::max(0, thermal_cap)); }
void metal_resize(Backend* backend, int w, int h) { if (MetalThreaded* t = threaded(backend)) t->resize(w, h); else static_cast<MetalBackend*>(backend)->resize(w, h); }
void metal_set_options(Backend* backend, const MetalOptions& options) { if (MetalThreaded* t = threaded(backend)) t->set_options(options); else static_cast<MetalBackend*>(backend)->set_options(options); }
uint64_t metal_frames_presented(Backend* backend) { if (MetalThreaded* t = threaded(backend)) return t->frames_presented(); return static_cast<MetalBackend*>(backend)->frames_presented(); }
void metal_set_overlay(Backend* backend, OverlayProvider provider) { if (MetalThreaded* t = threaded(backend)) t->set_overlay(std::move(provider)); else static_cast<MetalBackend*>(backend)->set_overlay(std::move(provider)); }

}  // namespace gx
