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

// HDR float output, depth for the compositor and foveation as supported. The shader converts
// the game's composited display-space picture to linear light for this output.
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
    private let depthState: MTLDepthStencilState
    private let sampler: MTLSamplerState
    private let corners: MTLBuffer

    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private var worldTrackingAvailable = false

    // Placement: centre, yaw-only rotation and width in metres — AR Mode's T · R_y · S. The picture
    // is upright at all times; height follows the aspect.
    private var center = SIMD3<Float>(0, -0.1, -0.85)
    private var yaw: Float = 0
    private var width: Float = 0.7   // metres; AR Mode's default screen width
    private var aspect: Float = 4.0 / 3.0
    private var placedThisSession = false
    private var placementDirty = false

    // Pinch gestures: the system delivers look+pinch as spatial events. A pinch that lands on the
    // picture plays — it becomes a finger on the quad's touch surface (buttons, sticks, the menu)
    // — and a pinch that lands off it moves, turns and scales the screen, exactly like xelee's
    // grab: one hand drags, two hands twist and spread.
    private struct Touch { var point: SIMD3<Float>; var play = false; var handBased = false }
    private var touches: [SpatialEventCollection.Event.ID: Touch] = [:]
    private struct Grab {
        let first, second: SpatialEventCollection.Event.ID
        let distance, angle, width, yaw: Float
        let midpoint, center: SIMD3<Float>
    }
    private var grabStart: Grab?
    private struct EventIn {
        let id: SpatialEventCollection.Event.ID
        let active: Bool
        let point: SIMD3<Float>
        let hand: SIMD3<Float>?
        let ray: (origin: SIMD3<Float>, direction: SIMD3<Float>)?
    }
    private var pendingEvents: [EventIn] = []
    private let eventLock = NSLock()
    private var persistenceTask: Task<Void, Never>?

    private var haveLoggedFrame = false

    static let anchorKey = "ar_mode_anchor_uuid"
    private static let widthKey = "ar_mode_screen_width"

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
    // Match the flat renderer's display-space blend: the overlay is already premultiplied
    // in that space. Decode the finished picture, rather than decoding premultiplied channels.
    fragment float4 quad_fs(VOut i [[stage_in]],
                            texture2d<float> game [[texture(0)]],
                            texture2d<float> overlay [[texture(1)]],
                            sampler s [[sampler(0)]]) {
        float2 uv = float2(i.uv.x, 1.0 - i.uv.y);   // texture row 0 is the top of the picture
        float3 g = game.sample(s, uv).rgb;
        float4 o = overlay.sample(s, uv);
        float3 srgb = g * (1.0 - o.a) + o.rgb;
        float3 linear = select(pow((srgb + 0.055) / 1.055, float3(2.4)),
                               srgb / 12.92, srgb <= 0.04045);
        return float4(linear, 1.0);
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
        descriptor.depthAttachmentPixelFormat = layerRenderer.configuration.depthFormat
        descriptor.maxVertexAmplificationCount = layerRenderer.properties.viewCount
        self.pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let depth = MTLDepthStencilDescriptor()
        depth.depthCompareFunction = .greater
        depth.isDepthWriteEnabled = true   // CompositorServices uses reverse-Z, including the quad's depth.
        self.depthState = device.makeDepthStencilState(descriptor: depth)!
        let sampling = MTLSamplerDescriptor()
        sampling.minFilter = .linear
        sampling.magFilter = .linear
        sampling.sAddressMode = .clampToEdge
        sampling.tAddressMode = .clampToEdge
        self.sampler = device.makeSamplerState(descriptor: sampling)!

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
            let p = event.location3D
            let hand = event.inputDevicePose.map { device in
                let position = device.pose3D.position
                return SIMD3<Float>(Float(position.x), Float(position.y), Float(position.z))
            }
            let ray = event.selectionRay.map { ray in
                (origin: SIMD3<Float>(Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z)),
                 direction: SIMD3<Float>(Float(ray.direction.x), Float(ray.direction.y), Float(ray.direction.z)))
            }
            // End/cancel can arrive after hand tracking is lost, with no input-device pose.
            batch.append(EventIn(id: event.id, active: event.phase == .active,
                                 point: SIMD3<Float>(Float(p.x), Float(p.y), Float(p.z)), hand: hand, ray: ray))
        }
        guard !batch.isEmpty else { return }
        eventLock.lock()
        pendingEvents.append(contentsOf: batch)
        eventLock.unlock()
    }

    // MARK: - Frame loop

    func run() async {
        defer {
            releaseTouches()
            arSession.stop()
        }
        if WorldTrackingProvider.isSupported {
            _ = await arSession.requestAuthorization(for: WorldTrackingProvider.requiredAuthorizations)
            do {
                try await arSession.run([worldTracking])
                worldTrackingAvailable = true
            } catch {
                NSLog("ar mode: ARKit session failed (\(error)); the screen follows the head instead of the room")
            }
        }
        while true {
            if layerRenderer.state == .invalidated { return }
            if layerRenderer.state == .paused {
                releaseTouches()
                layerRenderer.waitUntilRunning()
                continue
            }
            await renderFrame()
        }
    }

    private func renderFrame() async {
        guard let frame = layerRenderer.queryNextFrame() else {
            // The compositor also returns nil while its frame pool is full. Yield instead of
            // spinning at full CPU until an in-flight frame becomes available.
            try? await Task.sleep(nanoseconds: 1_000_000)
            return
        }

        frame.startUpdate()
        drainEvents()

        var snap = DashdanceArFrame()
        let haveGame = dashdance_ar_snapshot(&snap) != 0 && snap.sequence > 0
        let lease = snap.lease
        var leaseSubmitted = false
        defer {
            if lease != 0 && !leaseSubmitted { dashdance_ar_release_frame(lease) }
        }
        if haveGame, abs(snap.aspect - aspect) > 0.01 { aspect = snap.aspect }
        if !placedThisSession {
            await placeOrRestore()
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
            let index = Int(snap.sequence % 3)
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
        let encodePass: (MTLRenderCommandEncoder, [simd_float4x4]) -> Void = { encoder, matrices in
            encoder.setRenderPipelineState(self.pipeline)
            encoder.setDepthStencilState(self.depthState)
            encoder.setVertexBuffer(self.corners, offset: 0, index: 0)
            matrices.withUnsafeBufferPointer { buffer in
                encoder.setVertexBytes(buffer.baseAddress!, length: MemoryLayout<simd_float4x4>.stride * buffer.count, index: 1)
            }
            encoder.setFragmentSamplerState(self.sampler, index: 0)
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
            pass.rasterizationRateMap = drawable.rasterizationRateMaps.first
            if let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
                encoder.setViewports(drawable.views.map { $0.textureMap.viewport })
                if drawable.views.count > 1 {
                    var mappings = (0..<drawable.views.count).map {
                        MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0),
                                                          renderTargetArrayIndexOffset: UInt32(drawable.views[$0].textureMap.sliceIndex))
                    }
                    encoder.setVertexAmplificationCount(drawable.views.count, viewMappings: &mappings)
                }
                encodePass(encoder, mvps)
                encoder.endEncoding()
            }
        } else {
            for (index, view) in drawable.views.enumerated() {
                let pass = MTLRenderPassDescriptor()
                let textureIndex = view.textureMap.textureIndex
                pass.colorAttachments[0].texture = drawable.colorTextures[textureIndex]
                pass.colorAttachments[0].slice = view.textureMap.sliceIndex
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = drawable.depthTextures[textureIndex]
                pass.depthAttachment.slice = view.textureMap.sliceIndex
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 0.0
                pass.depthAttachment.storeAction = .store
                if textureIndex < drawable.rasterizationRateMaps.count {
                    pass.rasterizationRateMap = drawable.rasterizationRateMaps[textureIndex]
                }
                if let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
                    encoder.setViewport(view.textureMap.viewport)
                    // A dedicated pass has amplification_id 0; bind this eye as matrix 0.
                    encodePass(encoder, [mvps[index]])
                    encoder.endEncoding()
                }
            }
        }

        if lease != 0 {
            // Each submission owns a lease, including repeated reads at headset refresh rate.
            // The producer cannot reuse this slot until this GPU command buffer is finished.
            commandBuffer.addCompletedHandler { _ in dashdance_ar_release_frame(lease) }
            leaseSubmitted = true
        }
        drawable.encodePresent(commandBuffer: commandBuffer)
        commandBuffer.commit()
        frame.endSubmission()
    }

    private func mirrorTexture(_ tuple: (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?),
                               _ slot: Int) -> MTLTexture? {
        let pointer = slot == 0 ? tuple.0 : slot == 1 ? tuple.1 : tuple.2
        guard let pointer else { return nil }
        // The frame lease retains this texture until GPU completion; no growing view cache.
        return Unmanaged<MTLTexture>.fromOpaque(pointer).takeUnretainedValue()
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

    private func hitPoint(_ event: EventIn) -> SIMD3<Float>? {
        if let ray = event.ray {
            let n = normal()
            let denominator = simd_dot(ray.direction, n)
            guard abs(denominator) > 1e-5 else { return nil }
            let distance = simd_dot(center - ray.origin, n) / denominator
            guard distance >= 0, distance.isFinite else { return nil }
            return ray.origin + ray.direction * distance
        }
        // Direct touches have a location even when there is no indirect selection ray.
        guard event.point.x.isFinite, event.point.y.isFinite, event.point.z.isFinite else { return nil }
        return projectOnPlane(event.point)
    }

    // The hit point in the screen's local metres: x across (±width/2), y up (±picture height/2).
    private func projectOnQuad(_ hit: SIMD3<Float>) -> SIMD3<Float> {
        let local = hit - center
        let c = cos(yaw), s = sin(yaw)
        return SIMD3<Float>(local.x * c - local.z * s, local.y, 0)
    }

    // Restore the anchored spot, or start in front of where the head is (AR Mode's default).
    private func placeOrRestore() async {
        guard worldTrackingAvailable else { placedThisSession = true; return }
        // run() can return before the first tracked device pose. Retry on the next frame instead
        // of permanently adopting a position beside the world's origin, below the player's eyes.
        guard let device = worldTracking.queryDeviceAnchor(atTimestamp: CACurrentMediaTime()), device.isTracked else { return }
        let anchors = await worldTracking.allAnchors
        if let stored = UserDefaults.standard.string(forKey: Self.anchorKey),
           let id = UUID(uuidString: stored),
           let anchors,
           let anchor = anchors.first(where: { $0.id == id && $0.isTracked }) {
            let m = anchor.originFromAnchorTransform
            center = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
            let x = SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z)
            let z = SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
            let storedWidth = UserDefaults.standard.float(forKey: Self.widthKey)
            // Migrate the old scaled-anchor representation; keep the game's current aspect.
            width = min(max(storedWidth > 0 ? storedWidth : simd_length(x), 0.35), 3.0)
            yaw = atan2(z.x, z.z)
            placedThisSession = true
            NSLog("ar mode: restored the anchored spot at (%.2f, %.2f, %.2f)", center.x, center.y, center.z)
            return
        }
        placeInFront(of: device.originFromAnchorTransform)
        placedThisSession = true
    }

    private func placeInFront(of devicePose: simd_float4x4) {
        let forward = -SIMD3<Float>(devicePose.columns.2.x, devicePose.columns.2.y, devicePose.columns.2.z)
        let flat = SIMD3<Float>(forward.x, 0, forward.z)
        let dir = simd_length(flat) > 1e-4 ? simd_normalize(flat) : SIMD3<Float>(0, 0, -1)
        let head = SIMD3<Float>(devicePose.columns.3.x, devicePose.columns.3.y, devicePose.columns.3.z)
        center = head + dir * 0.85 + SIMD3<Float>(0, -0.05, 0)
        yaw = atan2(-dir.x, -dir.z)
        width = 0.7
    }

    private func persistPlacement() {
        guard worldTrackingAvailable else { return }
        // World anchors describe rigid room poses; picture size belongs to the app.
        var pose = quadPose()
        pose.columns.0 /= width
        pose.columns.1 /= pictureHeight()
        let savedWidth = width
        let provider = worldTracking
        let previousTask = persistenceTask
        persistenceTask = Task {
            // Serialize quick successive releases so an older request cannot overwrite the
            // newest placement. Keep the previous anchor if adding the replacement fails.
            await previousTask?.value
            let previous = UserDefaults.standard.string(forKey: Self.anchorKey).flatMap(UUID.init(uuidString:))
            do {
                let anchor = WorldAnchor(originFromAnchorTransform: pose)
                try await provider.addAnchor(anchor)
                UserDefaults.standard.set(savedWidth, forKey: Self.widthKey)
                UserDefaults.standard.set(anchor.id.uuidString, forKey: Self.anchorKey)
                if let previous { try? await provider.removeAnchor(forID: previous) }
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
                guard let hit = hitPoint(event) else { continue }
                let local = projectOnQuad(hit)   // use the selection ray, not the physical hand's projection
                if touches[event.id] == nil {
                    let pictureHeight = width / max(aspect, 0.01)
                    let onPicture = abs(local.x) <= width * 0.5 && abs(local.y) <= pictureHeight * 0.5
                    if onPicture {
                        touches[event.id] = Touch(point: hit, play: true)
                        dashdance_ar_touch(finger(of: event.id), 0, local.x / width + 0.5, 0.5 - local.y / pictureHeight)
                    } else {
                        // Placement follows the hand in all three dimensions. Projecting both
                        // hands onto the screen would erase depth movement and all yaw changes.
                        touches[event.id] = Touch(point: event.hand ?? hit, handBased: event.hand != nil)
                        if placementCount() == 2 { startTwoHandGrab() }
                    }
                } else {
                    let previous = touches[event.id]!.point
                    if touches[event.id]!.play {
                        touches[event.id]?.point = hit
                        dashdance_ar_touch(finger(of: event.id), 1, local.x / width + 0.5, 0.5 - local.y / pictureHeight())
                    } else {
                        let point: SIMD3<Float>
                        if touches[event.id]!.handBased {
                            guard let hand = event.hand else { continue }
                            point = hand
                        } else { point = hit }
                        touches[event.id]?.point = point
                        if placementCount() == 1 { move(by: point - previous) }
                        else { updateTwoHandGrab() }
                    }
                }
            } else {
                if touches[event.id]?.play == true {
                    dashdance_ar_touch(finger(of: event.id), 2, 0, 0)
                }
                touches[event.id] = nil
                if placementCount() < 2 { grabStart = nil }
                if placementCount() == 0 && placementDirty {
                    persistPlacement()
                    placementDirty = false
                }
            }
        }
    }

    private func placementCount() -> Int {
        touches.values.filter { !$0.play }.count
    }

    private func releaseTouches() {
        for (id, touch) in touches where touch.play { dashdance_ar_touch(finger(of: id), 2, 0, 0) }
        touches.removeAll()
        grabStart = nil
        eventLock.lock()
        pendingEvents.removeAll()
        eventLock.unlock()
    }

    // A stable id per spatial event, for the quad's touch surface.
    private func finger(of id: SpatialEventCollection.Event.ID) -> Int64 {
        Int64(bitPattern: UInt64(bitPattern: Int64(id.hashValue)))
    }

    private func pictureHeight() -> Float {
        width / max(aspect, 0.01)
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
        let hands = touches.filter { !$0.value.play }
        guard hands.count == 2 else { return }
        let ids = Array(hands.keys)
        let a = hands[ids[0]]!.point, b = hands[ids[1]]!.point
        grabStart = Grab(first: ids[0], second: ids[1], distance: simd_distance(a, b),
                         angle: angleXZ(a, b), width: width, yaw: yaw,
                         midpoint: (a + b) * 0.5, center: center)
    }

    private func updateTwoHandGrab() {
        guard let start = grabStart else { return }
        guard let first = touches[start.first], let second = touches[start.second] else { return }
        let a = first.point, b = second.point
        let distance = simd_distance(a, b)
        if start.distance > 1e-4 {
            width = min(max(start.width * distance / start.distance, 0.35), 3.0)
        }
        let angle = angleXZ(a, b) - start.angle
        // Positive model yaw turns +X towards -Z, opposite the XZ atan2 convention.
        if simd_length(SIMD2<Float>(b.x - a.x, b.z - a.z)) > 0.01 {
            yaw = start.yaw - atan2(sin(angle), cos(angle))
        }
        move(by: start.center + (a + b) * 0.5 - start.midpoint - center)
        placementDirty = true
    }

    private func angleXZ(_ a: SIMD3<Float>, _ b: SIMD3<Float>) -> Float {
        atan2(b.z - a.z, b.x - a.x)
    }
}
