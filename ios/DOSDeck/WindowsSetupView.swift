//
//  WindowsSetupView.swift
//
//  The Windows wizard: your disc, the machine, then the installation.
//
//  Three pages before the machine starts, and then a walkthrough that stays
//  with you inside the session, because installing Windows 98 is not something
//  that finishes while you watch. Setup restarts the machine several times and
//  asks questions in between; the panel says which step you are on and what to
//  do on it, and it reads the answer off the disk image rather than asking.
//
//  DOSDeck supplies no operating system and cannot obtain one. The disc is the
//  user's, from the Files app, and that is the only way one gets here.
//

import Combine
import SwiftUI
import UniformTypeIdentifiers

struct WindowsSetupView: View {
    @EnvironmentObject var engine: Engine
    /// Back to the front door. Not a dismissal: the wizard is a route.
    var onCancel: () -> Void
    /// Open on the steps rather than the disc list, so a capture shows what
    /// the walkthrough is rather than an empty folder.
    var startOnInstall = false
    @StateObject private var win = WindowsMachine()

    private enum Page: Int { case disc, machine, install }
    @State private var page: Page = .disc
    @State private var discs: [Storage.Disc] = []
    @State private var importing = false
    @State private var problem: String?

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            VStack(spacing: 14) {
                header
                Group {
                    switch page {
                    case .disc:    discPage
                    case .machine: machinePage
                    case .install: installPage
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                controls
            }
            .padding(.horizontal, 22)
            .padding(.vertical, 14)
        }
        .onAppear {
            discs = Storage.discImages()
            win.load()
            if startOnInstall { page = .install }
        }
        .fileImporter(isPresented: $importing,
                      allowedContentTypes: [.data],
                      allowsMultipleSelection: true) { result in
            guard case .success(let urls) = result else { return }
            for url in urls {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                let dest = Storage.discs.appendingPathComponent(url.lastPathComponent)
                try? FileManager.default.removeItem(at: dest)
                do { try FileManager.default.copyItem(at: url, to: dest) }
                catch { problem = error.localizedDescription }
            }
            discs = Storage.discImages()
        }
        .alert("DOSDeck", isPresented: Binding(get: { problem != nil },
                                               set: { if !$0 { problem = nil } })) {
            Button("OK", role: .cancel) { problem = nil }
        } message: { Text(problem ?? "") }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            ScreenTitle(text: pageTitle, step: "\(page.rawValue + 1) of 3")
            Button("Cancel") { onCancel() }
                .font(Theme.small)
                .foregroundColor(Theme.amberDim)
                .padding(.leading, 12)
        }
    }

    private var pageTitle: String {
        switch page {
        case .disc:    return "Your Windows 98 disc"
        case .machine: return "The machine"
        case .install: return win.diskExists ? "Carry on installing" : "Install Windows"
        }
    }

    private var controls: some View {
        HStack(spacing: 12) {
            if page != .disc {
                DeckButton(title: "Back", icon: "chevron.left") {
                    page = page == .install ? .machine : .disc
                }
                .frame(width: 140)
            } else {
                DeckButton(title: "Add a disc…", icon: "plus") { importing = true }
                    .frame(width: 180)
            }
            Spacer(minLength: 0)
            if page == .install {
                DeckButton(title: "Start the machine", icon: "play.fill",
                           prominent: true, enabled: win.blocker == nil) { start() }
                    .frame(width: 230)
            } else {
                DeckButton(title: "Next", icon: "chevron.right", prominent: true,
                           enabled: page != .disc || win.iso != nil) {
                    page = page == .disc ? .machine : .install
                }
                .frame(width: 150)
            }
        }
    }

    // MARK: - 1. The disc

    private var discPage: some View {
        HStack(spacing: 14) {
            Panel(title: "Discs folder") {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        if discs.isEmpty {
                            EmptyNote(title: "No discs yet",
                                      detail: "Copy your Windows 98 CD image — an .ISO or a "
                                            + ".CUE with its .BIN — into DOSDeck's Discs "
                                            + "folder in the Files app, or tap Add a disc.")
                        } else {
                            ForEach(discs) { disc in
                                Button { win.iso = disc.url } label: {
                                    ChoiceRow(title: disc.name,
                                              subtitle: Storage.describe(bytes: disc.bytes),
                                              icon: "opticaldisc",
                                              selected: win.iso?.path == disc.url.path)
                                }
                                .buttonStyle(.plain)
                            }
                        }
                    }
                }
            }
            Panel(title: "What DOSDeck does and does not do") {
                VStack(alignment: .leading, spacing: 10) {
                    note("The disc is yours.",
                         "DOSDeck does not include Windows, cannot download it, and has "
                       + "nowhere to download it from. It installs the disc you already own.")
                    note("It must be a bootable Full or OEM disc.",
                         "DOSDeck starts Setup by booting the CD. An upgrade disc with no "
                       + "boot record fails with “El Torito CD-ROM boot record not found”.")
                    note("Nothing else is needed.",
                         "No boot floppy: the startup disk is taken out of the CD itself.")
                    Spacer(minLength: 0)
                }
                .padding(14)
            }
            .frame(width: 330)
        }
    }

    private func note(_ title: String, _ body: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(Theme.small.weight(.semibold))
                .foregroundColor(Theme.amber)
            Text(body)
                .font(Theme.tiny)
                .foregroundColor(Theme.amberDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - 2. The machine

    private var machinePage: some View {
        HStack(alignment: .top, spacing: 14) {
            Panel(title: "Hardware") {
                OptionRow(label: "Hard disk",
                          note: "8 GB is the size the DOSBox-X guide uses. Anything over "
                              + "512 MB is formatted FAT32, and Windows 98's own IDE driver "
                              + "cannot handle a volume over 128 GB.") {
                    DeckPicker(options: Self.diskSizes,
                               selection: Binding(get: { String(win.sizeMB) },
                                                  set: { win.sizeMB = Int32($0) ?? 0 }))
                }
                dividerLine
                OptionRow(label: "Memory",
                          note: "128 MB is the guide's default. Windows 98 does not boot "
                              + "reliably above 512 MB and nothing it runs needs more.") {
                    DeckPicker(options: Self.memory,
                               selection: Binding(get: { String(win.memsize) },
                                                  set: { win.memsize = Int32($0) ?? 128 }))
                }
                dividerLine
                OptionRow(label: "3dfx Voodoo",
                          note: "Without one, the 3D games of Windows 98's own era cannot "
                              + "run at all — they look for Glide or Direct3D and find "
                              + "neither. It is emulated in software, so it is also the "
                              + "most expensive thing in the machine.") {
                    DeckToggle(on: Binding(get: { win.voodoo }, set: { win.voodoo = $0 }))
                }
            }
            Panel(title: "The disk") {
                VStack(alignment: .leading, spacing: 10) {
                    if win.diskExists {
                        note("There is already a disk.",
                             "DOSDeck found \(win.diskURL.lastPathComponent) in the Windows "
                           + "folder and will carry on with it. "
                           + (win.installed
                              ? "Windows is installed on it."
                              : "Windows is not installed on it yet."))
                        DeckButton(title: "Start again with an empty disk",
                                   icon: "trash") {
                            win.startOver()
                        }
                        Text("That deletes the disk image and everything installed on it. "
                           + "A half-installed Windows cannot be pointed at Setup a second "
                           + "time, so starting again means a new disk.")
                            .font(Theme.tiny)
                            .foregroundColor(Theme.amberDim)
                            .fixedSize(horizontal: false, vertical: true)
                    } else {
                        note("DOSDeck will make one.",
                             "An empty hard disk image, made the moment the machine starts. "
                           + "It takes a few seconds and then the CD boots.")
                    }
                    Spacer(minLength: 0)
                }
                .padding(14)
            }
            .frame(width: 330)
        }
    }

    private var dividerLine: some View {
        Rectangle().fill(Theme.rule.opacity(0.5)).frame(height: 1).padding(.leading, 14)
    }

    // MARK: - 3. What happens next

    private var installPage: some View {
        HStack(alignment: .top, spacing: 14) {
            Panel(title: "What happens when you start") {
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        ForEach(Array(win.steps.enumerated()), id: \.element.id) { index, step in
                            HStack(alignment: .top, spacing: 10) {
                                Text("\(index + 1)")
                                    .font(Theme.small.weight(.bold))
                                    .foregroundColor(Theme.screenTop)
                                    .frame(width: 20, height: 20)
                                    .background(Theme.amberDim)
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(step.title)
                                        .font(Theme.body.weight(.semibold))
                                        .foregroundColor(Theme.amber)
                                    Text(step.detail)
                                        .font(Theme.tiny)
                                        .foregroundColor(Theme.amberDim)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }
                        }
                    }
                    .padding(14)
                }
            }
            VStack(spacing: 14) {
                Panel(title: "This machine") {
                    VStack(alignment: .leading, spacing: 6) {
                        line("Disc", win.iso?.lastPathComponent ?? "none")
                        line("Disk", win.sizeMB == 0 ? "8 GB" : "\(win.sizeMB) MB")
                        line("Memory", "\(win.memsize) MB")
                        line("Voodoo", win.voodoo ? "yes" : "no")
                        line("Step", win.currentStep.title)
                    }
                    .padding(14)
                }
                if let blocker = win.blocker {
                    Panel(title: "Not ready") {
                        Text(blocker)
                            .font(Theme.small)
                            .foregroundColor(Theme.alarm)
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(14)
                    }
                }
                Spacer(minLength: 0)
            }
            .frame(width: 330)
        }
    }

    private func line(_ label: String, _ value: String) -> some View {
        HStack(alignment: .top) {
            Text(label)
                .font(Theme.small)
                .foregroundColor(Theme.amberDim)
                .frame(width: 80, alignment: .leading)
            Text(value)
                .font(Theme.small)
                .foregroundColor(Theme.amber)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func start() {
        win.save()
        guard engine.startWindows(win) else {
            problem = engine.notice ?? "The machine could not be started."
            engine.notice = nil
            return
        }
        // Nothing to close: the root shows the session as soon as the engine
        // reports it has started.
    }

    static let diskSizes: [(value: String, label: String)] = [
        ("0", "8 GB (recommended)"), ("512", "512 MB"), ("1024", "1 GB"),
        ("2048", "2 GB"), ("4096", "4 GB"), ("16384", "16 GB"),
    ]
    static let memory: [(value: String, label: String)] = [
        ("64", "64 MB"), ("128", "128 MB (recommended)"), ("256", "256 MB"), ("512", "512 MB"),
    ]
}

// MARK: - The walkthrough, inside the session

/// Shown over the running machine while Windows is being installed. It is a
/// panel rather than a page because the machine behind it is the thing being
/// worked on -- the user needs to read a step and then go and do it.
struct WindowsWalkthrough: View {
    @ObservedObject var win: WindowsMachine
    @EnvironmentObject var engine: Engine
    var onDismiss: () -> Void

    /// The disk image is written by the guest, so the only way to know where
    /// an installation has got to is to keep looking at it.
    private let poll = Timer.publish(every: 3, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("INSTALLING \(win.osName.uppercased())")
                    .font(Theme.small.weight(.semibold))
                    .kerning(1.4)
                    .foregroundColor(Theme.cursor)
                Spacer()
                Button { onDismiss() } label: {
                    Image(systemName: "chevron.down")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(Theme.amberDim)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            Rectangle().fill(Theme.rule).frame(height: 1)

            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(win.steps) { step in
                        stepRow(step)
                    }
                    if let action = win.currentStep.action {
                        Panel(title: "Do this now", raised: true) {
                            Text(action)
                                .font(Theme.small)
                                .foregroundColor(Theme.amber)
                                .fixedSize(horizontal: false, vertical: true)
                                .padding(12)
                        }
                    }
                    if win.phase == Phase.run {
                        Text("Windows is installed. You can put a disc in the drive from "
                           + "the Discs panel at any time.")
                            .font(Theme.tiny)
                            .foregroundColor(Theme.amberDim)
                    }
                }
                .padding(14)
            }
        }
        .background(Theme.panel)
        .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
        .onReceive(poll) { _ in win.refresh() }
    }

    private func stepRow(_ step: WindowsMachine.Step) -> some View {
        let done = step.id < win.phase
        let now = step.id == win.phase
        return HStack(alignment: .top, spacing: 10) {
            Image(systemName: done ? "checkmark.square.fill"
                                   : (now ? "arrowtriangle.right.fill" : "square"))
                .font(.system(size: 12))
                .foregroundColor(done ? Theme.amberDim : (now ? Theme.cursor : Theme.rule))
                .frame(width: 16)
            VStack(alignment: .leading, spacing: 2) {
                Text(step.title)
                    .font(Theme.small.weight(now ? .semibold : .regular))
                    .foregroundColor(now ? Theme.amber : Theme.amber.opacity(done ? 0.5 : 0.7))
                if now {
                    Text(step.detail)
                        .font(Theme.tiny)
                        .foregroundColor(Theme.amberDim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
}
