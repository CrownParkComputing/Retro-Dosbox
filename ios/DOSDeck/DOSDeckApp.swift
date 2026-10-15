//
//  DOSDeckApp.swift
//
//  DOSDeck — a DOS and Windows 98 machine for iPhone and iPad, running
//  DOSBox-X (GNU GPL v2) behind a front end written for this platform.
//
//  SWIFT OWNS THE APPLICATION, AND THAT IS THE POINT
//
//  The interface this replaces was SDL3 and Dear ImGui -- the same C++ file
//  that is the interface of the Android app and of every other Retro-* title
//  we ship. Apple rejected the family under guideline 4.3: several apps from
//  one developer, too alike. New artwork over the same screens would not have
//  answered that. A different app does: every screen here is written for iOS,
//  in SwiftUI, and is shared with nothing else. What is shared is the GPL
//  engine underneath, which is credited by name on the first screen and in
//  full on the About screen.
//
//  So SDL does not own main() any more, and does not own the window, the
//  event loop or the application lifecycle. It is linked because DOSBox-X
//  needs it for audio and timing, and the engine gets SDL's DUMMY video
//  driver so that it cannot touch UIKit from the emulator thread. The picture
//  arrives as finished frames through the engine's framebuffer tap and is
//  drawn by Metal -- see EmuSurface.
//

import AVFoundation
import SwiftUI

@main
struct DOSDeckApp: App {
    @StateObject private var engine = Engine()
    @Environment(\.scenePhase) private var phase

    init() {
        Storage.prepare()
        configureAudio()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(engine)
                .preferredColorScheme(.dark)
                .onChange(of: phase) { newPhase in
                    // iOS stops an app that keeps burning CPU in the
                    // background, and a paused emulator is also a correctly
                    // paused game: the last frame is kept, so coming back
                    // looks like nothing happened.
                    switch newPhase {
                    case .active:                 engine.setPaused(false)
                    case .inactive, .background:  engine.setPaused(true)
                    @unknown default:             break
                    }
                }
        }
    }

    /// Playback, so the machine can be heard with the ring switch on.
    ///
    /// An emulator's audio is the point of it rather than incidental to it --
    /// a DOS game's music IS the game -- and .ambient would leave somebody
    /// with a silent machine and no way to tell why. SDL opens the device
    /// itself; this only decides what the device is for.
    private func configureAudio() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [])
        try? session.setActive(true)
    }
}

/// The front door until a machine has been started, and the machine after
/// that. There is no way back, and the front door says so before you choose:
/// DOSBox-X initialises its globals once, so a second machine in the same
/// process boots into a triple fault. See Engine.
///
/// The wizards are ROUTES rather than modals. A modal presented by the front
/// door would be presented by a view that is removed the instant the machine
/// starts -- which is a sheet dismissing itself and its presenter in the same
/// frame, and the one arrangement SwiftUI is reliably bad at. Here starting a
/// machine simply changes what the root shows.
struct RootView: View {
    @EnvironmentObject var engine: Engine
    @State private var route: Route = .frontDoor

    enum Route { case frontDoor, dos, windows }

    var body: some View {
        ZStack {
            if engine.started {
                SessionView().environmentObject(engine)
            } else {
                switch route {
                case .frontDoor:
                    FrontDoor(onChoose: { route = $0 },
                              openAbout: Shot.requested == .about)
                        .environmentObject(engine)
                case .dos:
                    DosSetupView(onCancel: { route = .frontDoor },
                                 startOnMachine: Shot.requested == .machine)
                        .environmentObject(engine)
                case .windows:
                    WindowsSetupView(onCancel: { route = .frontDoor })
                        .environmentObject(engine)
                }
            }
        }
        .animation(.easeInOut(duration: 0.2), value: engine.started)
        .onAppear(perform: openRequestedScreen)
    }

    /// The store captures cannot tap, so the app is told where to open.
    /// Does nothing in a shipped app -- see Shot.
    private func openRequestedScreen() {
        switch Shot.requested {
        case .none, .front, .about:
            route = .frontDoor
        case .dos, .machine:
            route = .dos
        case .windows:
            route = .windows
        case .session:
            // The bundled demo, which is the one thing guaranteed to be there
            // on a fresh install.
            let demo = Storage.dos.appendingPathComponent("Demo", isDirectory: true)
            _ = engine.startDOS(machine: Engine.loadMachine(), mount: demo,
                                runCommand: Storage.runnable(in: demo),
                                title: "Demo")
        }
    }
}
