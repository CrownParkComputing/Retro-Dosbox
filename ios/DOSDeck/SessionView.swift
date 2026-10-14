//
//  SessionView.swift
//
//  The machine, running.
//
//  Everything over the picture is transient: there is no rail, no persistent
//  list and nothing docked to an edge. One chip in the corner opens the
//  panels, and a panel is gone the moment you are done with it, because the
//  thing you came for is behind it.
//
//  The panels are what make one session enough. "Run something else" mounts a
//  folder on D: and types the program; "Discs" changes the CD in the drive the
//  machine already has. Neither needs the emulator restarted, which is
//  fortunate, because it cannot be.
//

import Combine
import SwiftUI
import UIKit

struct SessionView: View {
    @EnvironmentObject var engine: Engine
    @StateObject private var geometry = EmuGeometry()
    @StateObject private var win = WindowsMachine()

    @State private var panel: Panel?
    @State private var showKeyboard = false
    @State private var titles: [Storage.Title] = []
    @State private var discs: [Storage.Disc] = []
    @State private var insertedDisc: String?

    @AppStorage("pointerSpeed") private var pointerSpeed: Int = 100
    @AppStorage("machine.aspect_mode") private var aspectMode: Int = 0
    @AppStorage("machine.integer_scale") private var integerScale: Int = 0

    enum Panel: String, Identifiable {
        case software, discs, windows, settings
        var id: String { rawValue }
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            EmuSurface(aspectMode: Int32(aspectMode),
                       integerScale: integerScale != 0,
                       geometry: geometry)
                .ignoresSafeArea()

            PointerSurface(geometry: geometry, speed: pointerSpeed)
                .ignoresSafeArea()

            if !geometry.hasPicture { warmingUp }

            VStack(spacing: 0) {
                HStack(alignment: .top) {
                    Spacer()
                    chips
                }
                Spacer()
                if showKeyboard {
                    DosKeyboard { scancode, pressed in dd_key(scancode, pressed) }
                        .transition(.move(edge: .bottom))
                }
            }
            .padding(.horizontal, 14)
            .padding(.top, 10)

            if let panel { panelOverlay(panel) }

            if let notice = engine.notice { noticeBar(notice) }
        }
        .onAppear(perform: reload)
    }

    // MARK: - Chrome

    private var warmingUp: some View {
        VStack(spacing: 12) {
            ProgressView().tint(Theme.amber)
            Text(engine.mode == .windows ? "Starting the machine…" : "Booting DOS…")
                .font(Theme.body)
                .foregroundColor(Theme.amberDim)
        }
    }

    /// The only permanent interface over the picture: three small chips in a
    /// corner. A DOS game uses the whole screen and the whole screen is what
    /// it should get.
    private var chips: some View {
        HStack(spacing: 8) {
            if engine.mode == .windows {
                chip("list.bullet.clipboard", "Setup") { panel = .windows }
            } else {
                chip("square.grid.2x2", "Run") { panel = .software; reload() }
            }
            chip("opticaldisc", "Discs") { panel = .discs; reload() }
            chip("keyboard", "Keys") { withAnimation { showKeyboard.toggle() } }
            chip("slider.horizontal.3", "More") { panel = .settings }
        }
    }

    private func chip(_ icon: String, _ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 2) {
                Image(systemName: icon).font(.system(size: 13, weight: .semibold))
                Text(label).font(Theme.tiny)
            }
            .foregroundColor(Theme.amber)
            .frame(width: 52, height: 42)
            .background(Theme.panel.opacity(0.88))
            .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }

    private func noticeBar(_ text: String) -> some View {
        VStack {
            Spacer()
            Text(text)
                .font(Theme.small)
                .foregroundColor(Theme.amber)
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(Theme.panel.opacity(0.95))
                .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
                .padding(.bottom, showKeyboard ? 220 : 24)
                .onTapGesture { engine.notice = nil }
        }
    }

    // MARK: - Panels

    private func panelOverlay(_ which: Panel) -> some View {
        HStack(spacing: 0) {
            // Tapping the picture puts the panel away. The machine keeps
            // running behind it the whole time -- nothing here pauses it.
            Color.black.opacity(0.35)
                .ignoresSafeArea()
                .onTapGesture { panel = nil }
            Group {
                switch which {
                case .software: softwarePanel
                case .discs:    discsPanel
                case .windows:  WindowsWalkthrough(win: win) { panel = nil }
                                    .environmentObject(engine)
                case .settings: settingsPanel
                }
            }
            .frame(width: 360)
            .background(Theme.panel)
            .ignoresSafeArea(edges: .vertical)
            .transition(.move(edge: .trailing))
        }
    }

    private var softwarePanel: some View {
        VStack(alignment: .leading, spacing: 0) {
            panelHeader("Run something else")
            ScrollView {
                LazyVStack(spacing: 0) {
                    if titles.isEmpty {
                        EmptyNote(title: "Nothing in the DOS folder",
                                  detail: "Add a folder or a .ZIP through the Files app and "
                                        + "it appears here without restarting DOSDeck.")
                    }
                    ForEach(titles.filter { $0.kind == .folder || $0.kind == .archive }) { title in
                        Button {
                            engine.run(title: title)
                            panel = nil
                        } label: {
                            ChoiceRow(title: title.name,
                                      subtitle: subtitle(for: title),
                                      icon: title.icon)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            Text("Started from D:, so whatever is on C: is left alone.")
                .font(Theme.tiny)
                .foregroundColor(Theme.amberDim)
                .padding(14)
        }
    }

    /// The CD-ROM list: what the Windows session is really for, and useful in
    /// a DOS session too. This is a real disc change -- the same thing
    /// DOSBox-X's own "change CD image" does -- so it works after a guest OS
    /// has booted, when typing IMGMOUNT would just send letters to Windows.
    private var discsPanel: some View {
        VStack(alignment: .leading, spacing: 0) {
            panelHeader("Disc in drive")
            ScrollView {
                LazyVStack(spacing: 0) {
                    Button {
                        _ = engine.insertCD(nil, drive: cdDrive)
                        insertedDisc = nil
                    } label: {
                        ChoiceRow(title: "Eject", subtitle: "Leave the drive empty",
                                  icon: "eject", selected: insertedDisc == nil)
                    }
                    .buttonStyle(.plain)

                    if discs.isEmpty {
                        EmptyNote(title: "No discs",
                                  detail: "Copy .ISO or .CUE files into DOSDeck's Discs "
                                        + "folder in the Files app.")
                    }
                    ForEach(discs) { disc in
                        Button {
                            if engine.insertCD(disc.url, drive: cdDrive) {
                                insertedDisc = disc.id
                            } else {
                                engine.notice = "This machine has no CD-ROM drive to put a "
                                              + "disc in. A drive that was not there when "
                                              + "the machine started cannot be added."
                            }
                        } label: {
                            ChoiceRow(title: disc.name,
                                      subtitle: Storage.describe(bytes: disc.bytes),
                                      icon: "opticaldisc",
                                      selected: insertedDisc == disc.id)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            Text(engine.mode == .windows
                 ? "Windows sees the change straight away, as it would a real disc."
                 : "The disc is in E:.")
                .font(Theme.tiny)
                .foregroundColor(Theme.amberDim)
                .padding(14)
        }
    }

    /// Windows mounts its CD as D: (the conf does it); a DOS session puts one
    /// in E: so that C: can be the game.
    private var cdDrive: CChar { engine.mode == .windows ? 68 : Engine.cdDrive }

    private var settingsPanel: some View {
        VStack(alignment: .leading, spacing: 0) {
            panelHeader("While it runs")
            ScrollView {
                VStack(spacing: 0) {
                    OptionRow(label: "Picture", note: "320x200 is a 4:3 picture, not 16:10.") {
                        DeckPicker(options: DosSetupView.aspect,
                                   selection: Binding(get: { String(aspectMode) },
                                                      set: { aspectMode = Int($0) ?? 0 }))
                    }
                    OptionRow(label: "Whole pixels", note: "Sharper, bigger borders.") {
                        DeckToggle(on: Binding(get: { integerScale != 0 },
                                               set: { integerScale = $0 ? 1 : 0 }))
                    }
                    OptionRow(label: "Pointer speed", note: "\(pointerSpeed)% of finger travel") {
                        Stepper("", value: $pointerSpeed, in: 25...400, step: 25)
                            .labelsHidden().tint(Theme.amber)
                    }
                    Rectangle().fill(Theme.rule).frame(height: 1)
                    OptionRow(label: "Reset the machine",
                              note: "A power cycle. In a Windows installation this is also "
                                  + "how the next step starts.") {
                        DeckButton(title: "Reset") {
                            engine.reset(hard: true)
                            panel = nil
                        }
                        .frame(width: 110)
                    }
                    OptionRow(label: "Status",
                              note: "\(engine.fps) fps · \(engine.program.isEmpty ? "—" : engine.program)") {
                        EmptyView()
                    }
                }
            }
        }
    }

    private func panelHeader(_ title: String) -> some View {
        VStack(spacing: 0) {
            HStack {
                Text(title.uppercased())
                    .font(Theme.small.weight(.semibold))
                    .kerning(1.4)
                    .foregroundColor(Theme.cursor)
                Spacer()
                Button { panel = nil } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundColor(Theme.amberDim)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            Rectangle().fill(Theme.rule).frame(height: 1)
        }
    }

    private func subtitle(for title: Storage.Title) -> String? {
        guard title.kind == .folder, let url = title.url else {
            return "ZIP · unpacked when you run it"
        }
        return Storage.runnable(in: Engine.mountRoot(of: url))
            .map { "runs \($0)" } ?? "no program found"
    }

    private func reload() {
        titles = Storage.titles()
        discs = Storage.discImages()
        win.refresh()
        // The Windows conf puts the chosen disc in the drive before anything
        // runs, so the list starts out already showing what is in there.
        if engine.mode == .windows, insertedDisc == nil, let iso = win.iso {
            insertedDisc = iso.path
        }
    }
}

// MARK: - The pointer

/// The touchscreen read as a trackpad rather than as a mouse that teleports.
///
/// DOS and Windows both drive a RELATIVE pointer: the guest draws the cursor
/// itself and applies its own speed and acceleration on top, neither of which
/// can be read back. So a finger moves the pointer by the distance it
/// travelled on the glass, scaled by how much the picture was scaled to get
/// there -- which is the only definition that means the same thing on a
/// 320x200 game filling a phone and on an 800x600 Windows desktop.
///
/// UIKit rather than SwiftUI gestures, because a two-finger tap for the right
/// button and a press-and-hold for drag-and-drop are both awkward to express
/// as SwiftUI gestures and neither is optional: Windows 98 needs both.
struct PointerSurface: UIViewRepresentable {
    @ObservedObject var geometry: EmuGeometry
    var speed: Int

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIView {
        let view = UIView()
        view.backgroundColor = .clear
        view.isMultipleTouchEnabled = true

        let pan = UIPanGestureRecognizer(target: context.coordinator,
                                         action: #selector(Coordinator.pan(_:)))
        pan.maximumNumberOfTouches = 2
        view.addGestureRecognizer(pan)

        let tap = UITapGestureRecognizer(target: context.coordinator,
                                         action: #selector(Coordinator.tap(_:)))
        view.addGestureRecognizer(tap)

        let twoFinger = UITapGestureRecognizer(target: context.coordinator,
                                               action: #selector(Coordinator.rightTap(_:)))
        twoFinger.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFinger)

        let hold = UILongPressGestureRecognizer(target: context.coordinator,
                                                action: #selector(Coordinator.hold(_:)))
        hold.minimumPressDuration = 0.35
        view.addGestureRecognizer(hold)

        pan.delegate = context.coordinator
        hold.delegate = context.coordinator
        hold.allowableMovement = 24

        context.coordinator.view = view
        return view
    }

    func updateUIView(_ view: UIView, context: Context) {
        context.coordinator.speed = speed
        context.coordinator.gain = gain
    }

    /// Emulated pixels per point of finger travel.
    private var gain: CGFloat {
        guard geometry.picture.width > 1, geometry.frameSize.width > 1 else { return 1 }
        return geometry.frameSize.width / geometry.picture.width
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        /// Hold-and-drag is one gesture to a person and two to UIKit, and by
        /// default the second one to start cancels the first. Windows 98
        /// cannot be used without drag-and-drop, so they run together.
        func gestureRecognizer(_ g: UIGestureRecognizer,
                               shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
            true
        }

        weak var view: UIView?
        var speed = 100
        var gain: CGFloat = 1
        private var last = CGPoint.zero
        /// Fractional movement, kept rather than thrown away: at 320x200 on a
        /// big screen a slow finger produces less than one emulated pixel per
        /// event, and rounding each one to zero means the pointer does not
        /// move at all until you swipe.
        private var residue = CGPoint.zero

        @objc func pan(_ g: UIPanGestureRecognizer) {
            guard let view else { return }
            let point = g.translation(in: view)

            if g.state == .began {
                last = .zero
                residue = .zero
                return
            }
            let dx = point.x - last.x
            let dy = point.y - last.y
            last = point

            if g.numberOfTouches == 2 {
                // Two fingers is the wheel, which DOS applications of the era
                // mostly ignore and Windows 98 does not.
                if abs(dy) > 12 {
                    dd_mouse_wheel(dy > 0 ? -1 : 1)
                    last = point
                }
                return
            }

            let scale = gain * CGFloat(speed) / 100.0
            let fx = dx * scale + residue.x
            let fy = dy * scale + residue.y
            let ix = fx.rounded(.towardZero)
            let iy = fy.rounded(.towardZero)
            residue = CGPoint(x: fx - ix, y: fy - iy)
            if ix != 0 || iy != 0 { dd_mouse_move(Int32(ix), Int32(iy)) }
        }

        @objc func tap(_ g: UITapGestureRecognizer) {
            click(0)
        }

        @objc func rightTap(_ g: UITapGestureRecognizer) {
            click(1)
        }

        /// Press and hold holds the left button down, which is how a drag
        /// happens: hold, then move with a second finger or the same one.
        @objc func hold(_ g: UILongPressGestureRecognizer) {
            switch g.state {
            case .began: dd_mouse_button(0, true)
            case .ended, .cancelled, .failed: dd_mouse_button(0, false)
            default: break
            }
        }

        private func click(_ button: Int32) {
            dd_mouse_button(button, true)
            // A guest polls the button; a press and release in the same
            // instant can fall between two polls and be missed entirely.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                dd_mouse_button(button, false)
            }
        }
    }
}
