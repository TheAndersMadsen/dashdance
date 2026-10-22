// AR Mode on Apple Vision Pro: a mixed-style immersive space with the game on a floating
// screen. The Swift @main owns the process on visionOS only — macOS and iOS keep the plain
// main() builds. The game flow (dashdance_vision_main) runs on its own thread: it hosts the
// dashboard here via a UIViewControllerRepresentable, waits for Play, then runs the game while
// SwiftUI keeps serving the main thread.
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit
import CompositorServices
import _CompositorServices_SwiftUI

// Shared flags between the C++ game flow and SwiftUI. `phase` moves idle → dashboard when the
// game thread has a launcher controller to host; `wantOpen` requests the immersive space; when
// the space closes, `reopenMain` brings the main window back.
final class SpaceBridge: ObservableObject {
    enum Phase { case idle, dashboard }
    static let shared = SpaceBridge()
    @Published var phase: Phase = .idle
    @Published var wantOpen = false
    @Published var reopenMain = false
    private let lock = NSLock()
    private var opened = false

    func setOpened(_ value: Bool) {
        lock.lock(); let was = opened; opened = value; lock.unlock()
        if was && !value { DispatchQueue.main.async { SpaceBridge.shared.reopenMain = true } }
    }
    func isOpen() -> Bool {
        lock.lock(); defer { lock.unlock() }; return opened
    }
}

// C function-pointer trampolines (no captures allowed there).
func dashdanceRequestOpenSpace() {
    DispatchQueue.main.async { SpaceBridge.shared.wantOpen = true }
}
func dashdanceIsSpaceOpen() -> Int32 {
    SpaceBridge.shared.isOpen() ? 1 : 0
}
func dashdanceShowDashboard() {
    DispatchQueue.main.async { SpaceBridge.shared.phase = .dashboard }
}

// The dashboard: the launcher's UIKit controller, hosted by SwiftUI so its lifecycle (layout,
// appearance transitions, rendering) is driven by the app's normal main-thread cycle.
struct DashboardHost: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        Unmanaged<UIViewController>.fromOpaque(dashdance_vision_take_controller()).takeRetainedValue()
    }
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}

struct RootView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openWindow) private var openWindow
    @ObservedObject var bridge = SpaceBridge.shared

    var body: some View {
        Group {
            if bridge.phase == .dashboard {
                DashboardHost()
                    .ignoresSafeArea()
            } else {
                // Clear, not black: in AR Mode the SwiftUI window would otherwise draw a pane
                // over the room in front of the quad — and while the space is open the window is
                // dismissed entirely (see onChange below), leaving the room as the display.
                Color.clear.ignoresSafeArea()
            }
        }
        .onChange(of: bridge.wantOpen) { _, want in
            guard want else { return }
            bridge.wantOpen = false
            Task { @MainActor in
                switch await openImmersiveSpace(id: ARScreenRenderer.spaceID) {
                case .opened:
                    NSLog("ar mode: immersive space opened")
                    SpaceBridge.shared.setOpened(true)
                    // The room is the display now: the window's glass would sit in front of the
                    // quad. Reopened when the space closes.
                    dismissWindow(id: "main")
                case .error, .userCancelled:
                    fallthrough
                @unknown default:
                    NSLog("ar mode: immersive space did not open")
                    SpaceBridge.shared.setOpened(false)
                }
            }
        }
        .onChange(of: bridge.reopenMain) { _, reopen in
            guard reopen else { return }
            bridge.reopenMain = false
            openWindow(id: "main")
        }
        .onAppear {
            // The game flow runs on its own thread from here; the main thread stays with SwiftUI.
            // Deferred past the launch screen's transaction so it comes down cleanly.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                dashdance_vision_main()
            }
        }
    }
}

// Entry point: the Swift runtime links us as a static library, where @main does not synthesize
// the C entry, so provide it: store the launch arguments for the game flow, then run the SwiftUI
// app (it never returns).
@_cdecl("main")
func dashdanceSwiftMain(_ argc: Int32, _ argv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32 {
    dashdance_vision_store_args(argc, argv)
    DashdanceVisionApp.main()
    return 0
}

struct DashdanceVisionApp: App {
    init() {
        dashdance_vision_set_space_hooks(dashdanceRequestOpenSpace, dashdanceIsSpaceOpen)
        dashdance_vision_set_dashboard_hook(dashdanceShowDashboard)
    }

    var body: some Scene {
        WindowGroup(id: "main") { RootView() }
        // No style modifiers here: a CompositorLayer space configures its own immersion
        // (Apple's guidance), and a mixed passthrough comes from the layer's alpha.
        ImmersiveSpace(id: ARScreenRenderer.spaceID) {
            CompositorLayer(configuration: ARConfiguration()) { layer in
                let renderer: ARScreenRenderer
                do {
                    renderer = try ARScreenRenderer(layerRenderer: layer)
                } catch {
                    fatalError("ar mode: renderer setup failed: \(error)")
                }
                Task(priority: .high) {
                    await renderer.run()
                    SpaceBridge.shared.setOpened(false)   // the game's mirror notices and stops rendering into the ring
                }
                layer.onSpatialEvent = { collection in
                    renderer.enqueue(collection)
                }
            }
        }
    }
}
