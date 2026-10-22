// AR Mode renderer: the game picture on a floating screen in the room, drawn straight into a
// CompositorLayer at display rate. The structure follows Apple's "Drawing fully immersive content
// using Metal" and the "Interacting with virtual content blended with passthrough" sample (the
// mixed-style CompositorServices reference). Placement gestures are AR Mode's: pinch-drag moves the
// screen, a second pinched hand turns it (twist) and resizes it (spread); the adopted spot is
// stored in a world anchor, which ARKit persists across launches.
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import CompositorServices
import _CompositorServices_SwiftUI
import Metal
import ARKit
import QuartzCore
import simd
import Spatial

extension LayerRenderer.Clock.Instant.Duration {
    var timeInterval: TimeInterval {
        let nanoseconds = TimeInterval(components.attoseconds / 1_000_000_000)
        return TimeInterval(components.seconds) + (nanoseconds / TimeInterval(NSEC_PER_SEC))
    }
}

// The sample's configuration: HDR float output, depth for the compositor, foveation as
// supported. The quad's textures stay 8-bit sRGB views, so their bytes convert to linear on
// sample and back through the HDR output with correct blending.
struct ARConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.depthFormat = .depth32Float
        configuration.colorFormat = .rgba16Float

        let foveationEnabled = capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveationEnabled

        let options: LayerRenderer.Capabilities.SupportedLayoutsOptions = foveationEnabled ? [.foveationEnabled] : []
        let supportedLayouts = capabilities.supportedLayouts(options: options)

        configuration.layout = supportedLayouts.contains(.layered) ? .layered : .dedicated
    }
}

// One dedicated task runs the frame loop for the whole session; the main thread only appends
// gesture events (under a lock), so a plain class is enough.
final class ARScreenRenderer {
    static let spaceID = "melee"

    private let layerRenderer: LayerRenderer
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let corners: MTLBuffer

    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()

    // Placement: centre, yaw-only rotation and width in metres — AR Mode's T · R_y · S. The picture
    // is upright at all times; height follows the aspect.
    private var center = SIMD3<Float>(0, -0.1, -0.85)
    private var yaw: Float = 0
    private var width: Float = 0.7   // metres; AR Mode's default screen width
    private var aspect: Float = 4.0 / 3.0
    private var placedThisSession = false
    private var placementDirty = false

    // Pinch gestures: the system delivers look+pinch as spatial events; we project the pointing
    // hand onto the screen's plane and drag/twist/scale from there.
    private struct Touch { var point: SIMD3<Float> }
    private var touches: [SpatialEventCollection.Event.ID: Touch] = [:]
    private var grabStart: (distance: Float, angle: Float, width: Float, yaw: Float)?
    private struct EventIn { let id: SpatialEventCollection.Event.ID; let active: Bool; let point: SIMD3<Float> }
    private var pendingEvents: [EventIn] = []
    private let eventLock = NSLock()

    private var viewCache: [ObjectIdentifier: MTLTexture] = [:]
    private var haveLoggedFrame = false

    static let anchorKey = "ar_mode_anchor_uuid"

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut quad_vs(uint vid [[vertex_id]], uint view [[amplification_id]],
                        const device float4* corners [[buffer(0)]],
                        constant float4x4* mvp [[buffer(1)]]) {
        float4 c = corners[vid];
        VOut o;
        o.pos = mvp[view] * float4(c.xy, 0.0, 1.0);
        o.uv = c.zw;
        return o;
    }
    // The game picture underneath, the in-game menu premultiplied on top; both textures are
    // sRGB views over the game's 8-bit output, so the bytes pass through with linear blending.
    fragment float4 quad_fs(VOut i [[stage_in]],
                            texture2d<float> game [[texture(0)]],
                            texture2d<float> overlay [[texture(1)]],
                            sampler s [[sampler(0)]]) {
        float2 uv = float2(i.uv.x, 1.0 - i.uv.y);   // texture row 0 is the top of the picture
        float3 g = game.sample(s, uv).rgb;
        float4 o = overlay.sample(s, uv);
        return float4(g * (1.0 - o.a) + o.rgb, 1.0);
    }
    """

    init(layerRenderer: LayerRenderer) throws {
        self.layerRenderer = layerRenderer
        self.device = layerRenderer.device
        self.commandQueue = device.makeCommandQueue()!

        let library = try device.makeLibrary(source: Self.shaderSource, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "quad_vs")
        descriptor.fragmentFunction = library.makeFunction(name: "quad_fs")
        descriptor.colorAttachments[0].pixelFormat = layerRenderer.configuration.colorFormat
        descriptor.maxVertexAmplificationCount = layerRenderer.properties.viewCount
        self.pipeline = try device.makeRenderPipelineState(descriptor: descriptor)

        // x, y in half-units (the pose matrix owns the size); u, v across the picture.
        let quad: [Float] = [
            -0.5, -0.5, 0.0, 0.0,
             0.5, -0.5, 1.0, 0.0,
            -0.5,  0.5, 0.0, 1.0,
             0.5,  0.5, 1.0, 1.0,
        ]
        self.corners = device.makeBuffer(bytes: quad, length: quad.count * 4, options: .storageModeShared)!
    }

    func enqueue(_ collection: SpatialEventCollection) {
        var batch: [EventIn] = []
        for event in collection {
            guard let device = event.inputDevicePose else { continue }
            let p = device.pose3D.position
            batch.append(EventIn(id: event.id, active: event.phase == .active,
                                 point: SIMD3<Float>(Float(p.x), Float(p.y), Float(p.z))))
        }
        guard !batch.isEmpty else { return }
        eventLock.lock()
        pendingEvents.append(contentsOf: batch)
        eventLock.unlock()
    }

    // MARK: - Frame loop

    func run() async {
        if WorldTrackingProvider.isSupported {
            _ = await arSession.requestAuthorization(for: WorldTrackingProvider.requiredAuthorizations)
            do {
                try await arSession.run([worldTracking])
            } catch {
                NSLog("ar mode: ARKit session failed (\(error)); the screen follows the head instead of the room")
            }
        }
        while true {
            if layerRenderer.state == .invalidated { return }
            if layerRenderer.state == .paused {
                layerRenderer.waitUntilRunning()
                continue
            }
            await renderFrame()
        }
    }

    private func renderFrame() async {
        guard let frame = layerRenderer.queryNextFrame() else { return }

        frame.startUpdate()
        drainEvents()

        var snap = DashdanceArFrame()
        let haveGame = dashdance_ar_snapshot(&snap) != 0 && snap.sequence > 0
        if haveGame, abs(snap.aspect - aspect) > 0.01 { aspect = snap.aspect }
        if !placedThisSession {
            await placeOrRestore()
            placedThisSession = true
        }
        guard let timing = frame.predictTiming() else {
            frame.endUpdate()
            return
        }
        frame.endUpdate()

        // Pace, then submit: everything below is encoded against the predicted presentation pose.
        do {
            try await LayerRenderer.Clock().sleep(until: timing.optimalInputTime, tolerance: nil)
        } catch { return }
        frame.startSubmission()

        guard let drawable = frame.queryDrawable() else {
            frame.endSubmission()
            return
        }
        let presentationTime = LayerRenderer.Clock.Instant.epoch
            .duration(to: drawable.frameTiming.presentationTime).timeInterval
        drawable.deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: presentationTime)

        guard let commandBuffer = commandQueue.makeCommandBuffer() else {
            frame.endSubmission()
            return
        }

        var gameTexture: MTLTexture?
        var overlayTexture: MTLTexture?
        var sequence: UInt64 = 0
        if haveGame {
            sequence = snap.sequence
            // The game published sequence s into ring slot s % 3 (its frame counter starts at 1).
            let slot = { (i: Int) -> Int in (i % 3 + 3) % 3 }
            var index = slot(Int(snap.sequence))
            if mirrorTexture(snap.game, index) == nil {   // early frames: not every slot exists yet
                for candidate in 0..<3 where mirrorTexture(snap.game, candidate) != nil { index = candidate; break }
            }
            gameTexture = mirrorTexture(snap.game, index)
            overlayTexture = mirrorTexture(snap.overlay, index)
            if !haveLoggedFrame {
                haveLoggedFrame = true
                NSLog("ar mode: quad consuming the mirror: sequence %llu, slot %d, %dx%d, textures %@/%@", sequence, index, snap.width, snap.height,
                      gameTexture.map { "\($0.width)x\($0.height)" } ?? "nil", overlayTexture.map { "\($0.width)x\($0.height)" } ?? "nil")
            }
            if let done = snap.done {
                commandBuffer.encodeWaitForEvent(Unmanaged<MTLSharedEvent>.fromOpaque(done).takeUnretainedValue(),
                                                 value: snap.sequence)
            }
        }

        let deviceAnchor = drawable.deviceAnchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        let pose = quadPose()
        var mvps: [simd_float4x4] = drawable.views.enumerated().map { index, view in
            let viewMatrix = (deviceAnchor * view.transform).inverse
            let projection = drawable.computeProjection(convention: .rightUpBack, viewIndex: index)
            return projection * viewMatrix * pose
        }
        if mvps.isEmpty { mvps = [pose] }

        let layered = layerRenderer.configuration.layout == .layered
        let encodePass: (MTLRenderCommandEncoder) -> Void = { encoder in
            encoder.setRenderPipelineState(self.pipeline)
            encoder.setVertexBuffer(self.corners, offset: 0, index: 0)
            encoder.setVertexBytes(&mvps, length: MemoryLayout<simd_float4x4>.stride * max(mvps.count, 1), index: 1)
            if let g = gameTexture { encoder.setFragmentTexture(g, index: 0) }
            if let o = overlayTexture { encoder.setFragmentTexture(o, index: 1) }
            if gameTexture != nil {
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
        }

        if layered {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.colorTextures[0]
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = drawable.depthTextures[0]
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.0
            pass.depthAttachment.storeAction = .store
            pass.renderTargetArrayLength = drawable.views.count
            if let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
                encoder.setViewports(drawable.views.map { $0.textureMap.viewport })
                if drawable.views.count > 1 {
                    var mappings = (0..<drawable.views.count).map {
                        MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0),
                                                          renderTargetArrayIndexOffset: UInt32($0))
                    }
                    encoder.setVertexAmplificationCount(drawable.views.count, viewMappings: &mappings)
                }
                encodePass(encoder)
                encoder.endEncoding()
            }
        } else {
            for (index, view) in drawable.views.enumerated() {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = drawable.colorTextures[min(index, drawable.colorTextures.count - 1)]
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = drawable.depthTextures[0]
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 0.0
                pass.depthAttachment.storeAction = .store
                if let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
                    encoder.setViewport(view.textureMap.viewport)
                    encodePass(encoder)
                    encoder.endEncoding()
                }
            }
        }

        if sequence > 0, let read = snap.read {
            commandBuffer.encodeSignalEvent(Unmanaged<MTLSharedEvent>.fromOpaque(read).takeUnretainedValue(),
                                            value: sequence)
        }
        drawable.encodePresent(commandBuffer: commandBuffer)
        commandBuffer.commit()
        frame.endSubmission()
    }

    private func mirrorTexture(_ tuple: (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?),
                               _ slot: Int) -> MTLTexture? {
        let pointer = slot == 0 ? tuple.0 : slot == 1 ? tuple.1 : tuple.2
        guard let pointer else { return nil }
        let texture = Unmanaged<MTLTexture>.fromOpaque(pointer).takeUnretainedValue()
        let key = ObjectIdentifier(texture)
        if let cached = viewCache[key] { return cached }
        let view = texture.makeTextureView(pixelFormat: .bgra8Unorm_srgb)
        viewCache[key] = view
        return view
    }

    // MARK: - Placement

    private func quadPose() -> simd_float4x4 {
        let height = width / max(aspect, 0.01)
        let c = cos(yaw), s = sin(yaw)
        return simd_float4x4(
            SIMD4<Float>(c * width, 0, -s * width, 0),
            SIMD4<Float>(0, height, 0, 0),
            SIMD4<Float>(s, 0, c, 0),
            SIMD4<Float>(center.x, center.y, center.z, 1)
        )
    }

    private func normal() -> SIMD3<Float> {
        SIMD3<Float>(sin(yaw), 0, cos(yaw))
    }

    private func projectOnPlane(_ point: SIMD3<Float>) -> SIMD3<Float> {
        let n = normal()
        return point + n * simd_dot(center - point, n)
    }

    // Restore the anchored spot, or start in front of where the head is (AR Mode's default).
    private func placeOrRestore() async {
        var anchors: [WorldAnchor]?
        anchors = await worldTracking.allAnchors
        if let stored = UserDefaults.standard.string(forKey: Self.anchorKey),
           let id = UUID(uuidString: stored),
           let anchors,
           let anchor = anchors.first(where: { $0.id == id }) {
            let m = anchor.originFromAnchorTransform
            center = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
            let x = SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z)
            let y = SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z)
            let z = SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
            width = max(simd_length(x), 0.2)
            aspect = max(width / max(simd_length(y), 0.05), 0.5)
            yaw = atan2(z.x, z.z)
            NSLog("ar mode: restored the anchored spot at (%.2f, %.2f, %.2f)", center.x, center.y, center.z)
            return
        }
        if let device = worldTracking.queryDeviceAnchor(atTimestamp: CACurrentMediaTime()) {
            placeInFront(of: device.originFromAnchorTransform)
        }
    }

    private func placeInFront(of devicePose: simd_float4x4) {
        let forward = -SIMD3<Float>(devicePose.columns.2.x, devicePose.columns.2.y, devicePose.columns.2.z)
        let flat = SIMD3<Float>(forward.x, 0, forward.z)
        let dir = simd_length(flat) > 1e-4 ? simd_normalize(flat) : SIMD3<Float>(0, 0, -1)
        let head = SIMD3<Float>(devicePose.columns.3.x, devicePose.columns.3.y, devicePose.columns.3.z)
        center = head + dir * 0.85 + SIMD3<Float>(0, -0.25, 0)
        yaw = atan2(-dir.x, -dir.z)
        width = 0.7
    }

    private func persistPlacement() {
        let pose = quadPose()
        let previous = UserDefaults.standard.string(forKey: Self.anchorKey).flatMap(UUID.init(uuidString:))
        Task {
            if let previous {
                try? await worldTracking.removeAnchor(forID: previous)
            }
            do {
                let anchor = WorldAnchor(originFromAnchorTransform: pose)
                try await worldTracking.addAnchor(anchor)
                UserDefaults.standard.set(anchor.id.uuidString, forKey: Self.anchorKey)
                NSLog("ar mode: screen spot anchored (%@)", anchor.id.uuidString)
            } catch {
                NSLog("ar mode: anchoring failed: \(error)")
            }
        }
    }

    // MARK: - Gestures

    private func drainEvents() {
        eventLock.lock()
        let batch = pendingEvents
        pendingEvents.removeAll()
        eventLock.unlock()
        guard !batch.isEmpty else { return }

        for event in batch {
            if event.active {
                let hit = projectOnPlane(event.point)
                if touches[event.id] == nil {
                    touches[event.id] = Touch(point: hit)
                    if touches.count == 2 { startTwoHandGrab() }
                } else {
                    let previous = touches[event.id]!.point
                    touches[event.id]?.point = hit
                    if touches.count == 1 {
                        move(by: hit - previous)
                    } else {
                        updateTwoHandGrab()
                    }
                }
            } else {
                touches[event.id] = nil
                if touches.count < 2 { grabStart = nil }
                if touches.isEmpty && placementDirty {
                    persistPlacement()
                    placementDirty = false
                }
            }
        }
    }

    private func move(by delta: SIMD3<Float>) {
        center += delta
        center.y = min(max(center.y, -2.2), 2.2)
        let flat = SIMD2<Float>(center.x, center.z)
        let radius = simd_length(flat)
        if radius > 5.0 {
            let scaled = flat / radius * 5.0
            center.x = scaled.x
            center.z = scaled.y
        }
        placementDirty = true
    }

    private func startTwoHandGrab() {
        let points = touches.values.map(\.point)
        guard points.count == 2 else { return }
        grabStart = (simd_distance(points[0], points[1]), angleXZ(points[0], points[1]), width, yaw)
    }

    private func updateTwoHandGrab() {
        guard let start = grabStart else { return }
        let points = touches.values.map(\.point)
        guard points.count == 2 else { return }
        let distance = simd_distance(points[0], points[1])
        if start.distance > 1e-4 {
            width = min(max(start.width * distance / start.distance, 0.35), 3.0)
        }
        yaw = start.yaw + (angleXZ(points[0], points[1]) - start.angle)
        placementDirty = true
    }

    private func angleXZ(_ a: SIMD3<Float>, _ b: SIMD3<Float>) -> Float {
        atan2(b.z - a.z, b.x - a.x)
    }
}
