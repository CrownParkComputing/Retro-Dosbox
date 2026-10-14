//
//  DosSetupView.swift
//
//  The DOS wizard: what to run, then what to run it on.
//
//  Two pages, in that order, because "which game" is the question somebody
//  came to answer and "which graphics card" is the question they only care
//  about when a game will not start. Both are on the way to the same button.
//
//  The machine page is deliberately complete. Every DOSBox-X setting that
//  decides whether a DOS title runs is here, with the reason it matters under
//  it, because that is the difference between an emulator you can fix a game
//  on and one where a game simply does not work.
//

import SwiftUI
import UniformTypeIdentifiers

struct DosSetupView: View {
    @EnvironmentObject var engine: Engine
    /// Back to the front door. Not a dismissal: the wizard is a route.
    var onCancel: () -> Void

    private enum Page { case software, machine }
    @State private var page: Page = .software

    @State private var machine = Engine.loadMachine()
    @State private var titles: [Storage.Title] = []
    @State private var discs: [Storage.Disc] = []
    @State private var floppies: [Storage.Disc] = []

    @State private var chosen: Storage.Title?
    @State private var chosenDisc: Storage.Disc?
    @State private var chosenFloppy: Storage.Disc?

    @State private var importing = false
    @State private var busy: String?
    @State private var problem: String?

    /// The bare prompt. Not a fallback -- for a lot of people this IS the app:
    /// a DOS machine with their whole folder on C: and nothing started for
    /// them.
    private let promptEntry = Storage.Title(id: "::prompt", name: "DOS prompt",
                                            kind: .prompt, url: nil, bytes: 0)

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            VStack(spacing: 14) {
                header
                Group {
                    switch page {
                    case .software: softwarePage
                    case .machine:  machinePage
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                controls
            }
            .padding(.horizontal, 22)
            .padding(.vertical, 14)

            if let busy { BusyOverlay(message: busy) }
        }
        .onAppear(perform: reload)
        .fileImporter(isPresented: $importing,
                      allowedContentTypes: [.folder, .data],
                      allowsMultipleSelection: true) { result in
            handleImport(result)
        }
        .alert("DOSDeck", isPresented: Binding(get: { problem != nil },
                                               set: { if !$0 { problem = nil } })) {
            Button("OK", role: .cancel) { problem = nil }
        } message: {
            Text(problem ?? "")
        }
    }

    // MARK: - Chrome

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            ScreenTitle(text: page == .software ? "What shall I run?" : "The machine",
                        step: page == .software ? "1 of 2" : "2 of 2")
            Button("Cancel") { onCancel() }
                .font(Theme.small)
                .foregroundColor(Theme.amberDim)
                .padding(.leading, 12)
        }
    }

    private var controls: some View {
        HStack(spacing: 12) {
            if page == .machine {
                DeckButton(title: "Back", icon: "chevron.left") { page = .software }
                    .frame(width: 140)
            } else {
                DeckButton(title: "Add files…", icon: "plus") { importing = true }
                    .frame(width: 180)
            }
            Spacer(minLength: 0)
            Text(summary)
                .font(Theme.tiny)
                .foregroundColor(Theme.amberDim)
                .lineLimit(2)
                .multilineTextAlignment(.trailing)
            if page == .software {
                DeckButton(title: "Machine", icon: "slider.horizontal.3") { page = .machine }
                    .frame(width: 170)
            }
            DeckButton(title: "Start", icon: "play.fill", prominent: true,
                       enabled: engine.started == false) { start() }
                .frame(width: 150)
        }
    }

    private var summary: String {
        let what = chosen?.name ?? "a DOS prompt"
        let cpu = machine.cycles_max != 0 ? "max cycles" : "\(machine.cycles_fixed) cycles"
        return "\(what) · \(CField.get(machine.machine)) · \(cpu)"
    }

    // MARK: - Page 1: software

    private var softwarePage: some View {
        HStack(spacing: 14) {
            Panel(title: "DOS folder") {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        row(for: promptEntry)
                        if titles.isEmpty {
                            EmptyNote(title: "Nothing here yet",
                                      detail: "Put a folder of DOS files, or a .ZIP of one, "
                                            + "into DOSDeck's DOS folder in the Files app — "
                                            + "or tap Add files below. DOSDeck has no "
                                            + "catalogue and cannot download software.")
                        } else {
                            ForEach(titles) { row(for: $0) }
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity)

            VStack(spacing: 14) {
                Panel(title: "Disc in drive E:") {
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            discRow(nil, label: "No disc")
                            ForEach(discs) { discRow($0, label: $0.name) }
                        }
                    }
                }
                Panel(title: "Boot a floppy instead") {
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            floppyRow(nil, label: "Don't boot a floppy")
                            ForEach(floppies) { floppyRow($0, label: $0.name) }
                        }
                    }
                }
            }
            .frame(width: 320)
        }
    }

    private func row(for title: Storage.Title) -> some View {
        Button {
            chosen = title
        } label: {
            ChoiceRow(title: title.name,
                      subtitle: subtitle(for: title),
                      icon: title.icon,
                      selected: chosen?.id == title.id)
        }
        .buttonStyle(.plain)
    }

    private func subtitle(for title: Storage.Title) -> String? {
        switch title.kind {
        case .prompt:
            return "C:\\ is your whole DOS folder. Type your own commands."
        case .archive:
            return "ZIP · \(Storage.describe(bytes: title.bytes)) · unpacked when you run it"
        case .folder:
            guard let url = title.url else { return nil }
            let root = Engine.mountRoot(of: url)
            return Storage.runnable(in: root).map { "runs \($0)" } ?? "no program found in it"
        }
    }

    private func discRow(_ disc: Storage.Disc?, label: String) -> some View {
        Button { chosenDisc = disc } label: {
            ChoiceRow(title: label,
                      subtitle: disc.map { Storage.describe(bytes: $0.bytes) },
                      icon: disc == nil ? "circle.slash" : "opticaldisc",
                      selected: chosenDisc?.id == disc?.id)
        }
        .buttonStyle(.plain)
    }

    private func floppyRow(_ disc: Storage.Disc?, label: String) -> some View {
        Button { chosenFloppy = disc } label: {
            ChoiceRow(title: label,
                      subtitle: disc == nil
                        ? "Use DOSBox-X's own DOS"
                        : "boots this disk instead of DOSBox-X's DOS",
                      icon: disc == nil ? "circle.slash" : "externaldrive",
                      selected: chosenFloppy?.id == disc?.id)
        }
        .buttonStyle(.plain)
    }

    // MARK: - Page 2: the machine

    private var machinePage: some View {
        ScrollView {
            HStack(alignment: .top, spacing: 14) {
                VStack(spacing: 14) {
                    Panel(title: "Graphics") {
                        OptionRow(label: "Card",
                                  note: "The biggest compatibility lever there is. An early "
                                      + "title written for CGA or Tandy can be confused by "
                                      + "an S3 and a few refuse outright.") {
                            DeckPicker(options: Self.machines, selection: stringBinding(\.machine))
                        }
                        divider
                        OptionRow(label: "Video memory",
                                  note: "Let DOSBox-X choose unless a later title wants "
                                      + "higher VESA modes.") {
                            DeckPicker(options: Self.vmem, selection: intBinding(\.vmemsize))
                        }
                        divider
                        OptionRow(label: "Picture",
                                  note: "320x200 is a 4:3 picture, not 16:10. Auto uses the "
                                      + "ratio the engine reports for the mode.") {
                            DeckPicker(options: Self.aspect, selection: intBinding(\.aspect_mode))
                        }
                        divider
                        OptionRow(label: "Whole pixels",
                                  note: "Sharper, with bigger borders.") {
                            DeckToggle(on: boolBinding(\.integer_scale))
                        }
                    }
                    Panel(title: "Sound") {
                        OptionRow(label: "Sound Blaster",
                                  note: "sbpro2 is the safest broad default for the DOS era.") {
                            DeckPicker(options: Self.sbtypes, selection: stringBinding(\.sbtype))
                        }
                        divider
                        OptionRow(label: "PC speaker",
                                  note: "The only sound many pre-1990 titles have.") {
                            DeckToggle(on: boolBinding(\.pcspeaker))
                        }
                        divider
                        OptionRow(label: "Game port",
                                  note: "Off unless a title wants a stick: several auto-select "
                                      + "joystick control and then steer on their own.") {
                            DeckToggle(on: boolBinding(\.joystick))
                        }
                    }
                }
                VStack(spacing: 14) {
                    Panel(title: "Processor") {
                        OptionRow(label: "Speed",
                                  note: "Max suits later titles. A fixed count is what saves "
                                      + "the early ones, which busy-wait for timing and run "
                                      + "absurdly fast otherwise.") {
                            DeckPicker(options: Self.speeds, selection: speedBinding)
                        }
                        if machine.cycles_max == 0 {
                            divider
                            OptionRow(label: "Cycles", note: "\(machine.cycles_fixed) per millisecond") {
                                Stepper("", value: cyclesBinding, in: 300...120_000, step: 1_000)
                                    .labelsHidden()
                                    .tint(Theme.amber)
                            }
                        }
                        divider
                        OptionRow(label: "CPU type",
                                  note: "Matters when a title refuses to start on a CPU it "
                                      + "thinks is too slow.") {
                            DeckPicker(options: Self.cputypes, selection: stringBinding(\.cputype))
                        }
                        divider
                        OptionRow(label: "FPU",
                                  note: "A handful of titles find a 387 and then use it badly.") {
                            DeckToggle(on: boolBinding(\.fpu))
                        }
                    }
                    Panel(title: "Memory and DOS") {
                        OptionRow(label: "Memory",
                                  note: "32MB, not more: DOS/4GW 1.97 miscalculates with large "
                                      + "memory and a pile of early-90s titles refuse above it.") {
                            DeckPicker(options: Self.memsizes, selection: intBinding(\.memsize))
                        }
                        divider
                        OptionRow(label: "EMS",
                                  note: "Some titles find expanded memory and then misuse it.") {
                            DeckToggle(on: boolBinding(\.ems))
                        }
                        divider
                        OptionRow(label: "UMB",
                                  note: "A few titles will not load with upper memory present.") {
                            DeckToggle(on: boolBinding(\.umb))
                        }
                        divider
                        OptionRow(label: "Reported DOS",
                                  note: "A few titles check the version and refuse. 7.1 also "
                                      + "turns on long filenames.") {
                            DeckPicker(options: Self.dosVersions, selection: stringBinding(\.dos_ver))
                        }
                    }
                }
            }
            .padding(.bottom, 8)
        }
    }

    private var divider: some View {
        Rectangle().fill(Theme.rule.opacity(0.5)).frame(height: 1).padding(.leading, 14)
    }

    // MARK: - Starting

    private func start() {
        Engine.save(machine: machine)
        let disc = chosenDisc?.url
        let floppy = chosenFloppy?.url

        // A floppy with its own operating system takes the machine over: there
        // is no DOSBox-X DOS to mount anything into, so nothing else is.
        if let floppy {
            _ = engine.startDOS(machine: machine, mount: nil, runCommand: nil,
                                cd: disc, bootFloppy: floppy,
                                title: floppy.deletingPathExtension().lastPathComponent)
            return
        }

        let title = chosen ?? promptEntry
        switch title.kind {
        case .prompt:
            // C: is the whole DOS folder, so everything the user has is one
            // CD away from the prompt.
            _ = engine.startDOS(machine: machine, mount: Storage.dos, runCommand: nil,
                                cd: disc, title: "DOS")

        case .folder:
            guard let url = title.url else { return }
            let root = Engine.mountRoot(of: url)
            _ = engine.startDOS(machine: machine, mount: root,
                                runCommand: Storage.runnable(in: root),
                                cd: disc, title: title.name)

        case .archive:
            guard let url = title.url else { return }
            busy = "Unpacking \(title.name)…"
            engine.prepareArchive(url, named: title.name) { folder in
                busy = nil
                guard let folder else {
                    problem = engine.notice ?? "That archive could not be unpacked."
                    engine.notice = nil
                    return
                }
                _ = engine.startDOS(machine: machine, mount: folder,
                                    runCommand: Storage.runnable(in: folder),
                                    cd: disc, title: title.name)
            }
        }
    }

    // MARK: - Files

    private func reload() {
        titles = Storage.titles()
        discs = Storage.discImages()
        // A floppy image is a disc-shaped file that DOSBox-X can boot. Only
        // the ones that actually carry an operating system are offered --
        // booting a data floppy gets you "This is not a bootable disk".
        floppies = Storage.discImages().filter {
            ["img", "ima"].contains($0.url.pathExtension.lowercased())
                && dd_image_has_os($0.url.path)
        }
        if chosen == nil { chosen = promptEntry }
    }

    private func handleImport(_ result: Result<[URL], Error>) {
        guard case .success(let urls) = result else { return }
        for url in urls {
            // A document picked from outside the app's own container is only
            // readable inside this pair of calls.
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }

            let isDisc = ["iso", "cue", "bin", "img", "chd", "ima"]
                .contains(url.pathExtension.lowercased())
            let destination = (isDisc ? Storage.discs : Storage.dos)
                .appendingPathComponent(url.lastPathComponent)
            try? FileManager.default.removeItem(at: destination)
            do {
                try FileManager.default.copyItem(at: url, to: destination)
            } catch {
                problem = "\(url.lastPathComponent) could not be copied in: "
                        + error.localizedDescription
            }
        }
        reload()
    }

    // MARK: - Bindings
    //
    // DDMachine is a C struct, so its fields are Int32 and fixed char arrays.
    // These turn each one into something a SwiftUI control can hold.

    private func boolBinding(_ key: WritableKeyPath<DDMachine, Int32>) -> Binding<Bool> {
        Binding(get: { machine[keyPath: key] != 0 },
                set: { machine[keyPath: key] = $0 ? 1 : 0 })
    }

    private func intBinding(_ key: WritableKeyPath<DDMachine, Int32>) -> Binding<String> {
        Binding(get: { String(machine[keyPath: key]) },
                set: { machine[keyPath: key] = Int32($0) ?? 0 })
    }

    private var cyclesBinding: Binding<Int> {
        Binding(get: { Int(machine.cycles_fixed) },
                set: { machine.cycles_fixed = Int32($0) })
    }

    private var speedBinding: Binding<String> {
        Binding(get: { machine.cycles_max != 0 ? "max" : "fixed" },
                set: { machine.cycles_max = $0 == "max" ? 1 : 0 })
    }

    /// A `char name[32]` arrives in Swift as a tuple of 32 CChars, so it is
    /// generic in the tuple's type rather than in String.
    private func stringBinding<T>(_ key: WritableKeyPath<DDMachine, T>) -> Binding<String> {
        Binding(get: { CField.get(machine[keyPath: key]) },
                set: { CField.set($0, into: &machine[keyPath: key]) })
    }

    // MARK: - The option lists

    static let machines: [(value: String, label: String)] = [
        ("svga_s3", "S3 Trio (SVGA)"), ("svga_et4000", "Tseng ET4000"),
        ("svga_paradise", "Paradise PVGA"), ("vesa_nolfb", "VESA, no LFB"),
        ("vgaonly", "Plain VGA"), ("ega", "EGA"), ("cga", "CGA"),
        ("tandy", "Tandy"), ("pcjr", "PCjr"), ("hercules", "Hercules"),
    ]
    static let cputypes: [(value: String, label: String)] = [
        ("auto", "Auto"), ("8086", "8086"), ("286", "286"), ("386", "386"),
        ("486old", "486 (early)"), ("486", "486"), ("pentium", "Pentium"),
        ("pentium_mmx", "Pentium MMX"),
    ]
    static let sbtypes: [(value: String, label: String)] = [
        ("sbpro2", "Sound Blaster Pro 2"), ("sb16", "Sound Blaster 16"),
        ("sbpro1", "Sound Blaster Pro"), ("sb2", "Sound Blaster 2.0"),
        ("sb1", "Sound Blaster 1.5"), ("gb", "Game Blaster"), ("none", "None"),
    ]
    static let memsizes: [(value: String, label: String)] = [
        ("4", "4 MB"), ("8", "8 MB"), ("16", "16 MB"), ("32", "32 MB"),
        ("64", "64 MB"), ("128", "128 MB"),
    ]
    static let vmem: [(value: String, label: String)] = [
        ("0", "Automatic"), ("1", "1 MB"), ("2", "2 MB"), ("4", "4 MB"), ("8", "8 MB"),
    ]
    static let aspect: [(value: String, label: String)] = [
        ("0", "Auto"), ("1", "4:3"), ("2", "16:9"), ("3", "Fill the screen"),
    ]
    static let speeds: [(value: String, label: String)] = [("max", "As fast as it will go"), ("fixed", "Fixed")]
    static let dosVersions: [(value: String, label: String)] = [
        ("", "DOSBox-X default (5.0)"), ("6.22", "6.22"), ("7.1", "7.1"),
    ]
}

/// A full-screen "working on it". Unpacking an archive is the one thing in the
/// wizard that takes long enough to need one.
struct BusyOverlay: View {
    let message: String
    var body: some View {
        ZStack {
            Color.black.opacity(0.7).ignoresSafeArea()
            VStack(spacing: 14) {
                ProgressView().tint(Theme.amber)
                Text(message)
                    .font(Theme.body)
                    .foregroundColor(Theme.amber)
            }
            .padding(28)
            .background(Theme.panel)
            .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
        }
    }
}
