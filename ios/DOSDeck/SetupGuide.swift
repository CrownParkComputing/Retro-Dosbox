//
//  SetupGuide.swift
//
//  The first-run walkthrough: what DOSDeck needs from you, and where it goes.
//
//  The front door asks "DOS or Windows" and assumes the answer can be acted
//  on. On a fresh install it cannot: the DOS folder holds only the demo, there
//  is no Windows disc, and nobody has been told that the Files app is how
//  anything gets in. This is the screen that tells them, one step at a time,
//  and checks each step by looking rather than by asking.
//
//  Written for this app and shared with nothing else (see DOSDeckApp.swift on
//  why that matters): the amber screen, the C:\> heading, the app's own
//  buttons. It runs once, and the front door's footer reopens it.
//

import SwiftUI

struct SetupGuide: View {
    @EnvironmentObject var engine: Engine
    /// Where to go when the guide is done: the front door, or straight into
    /// the Windows wizard when that was the last thing chosen.
    var onFinish: (RootView.Route) -> Void

    @AppStorage(SetupGuide.doneKey) private var done = false
    @StateObject private var win = WindowsMachine()

    @State private var step = 0
    @State private var titles: [Storage.Title] = []
    @State private var discs: [Storage.Disc] = []

    static let doneKey = "setupGuideDone"

    private let names = ["Welcome", "Your folders", "Your software", "Discs",
                         "Windows 98", "Controls", "Ready"]

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            VStack(spacing: 14) {
                ScreenTitle(text: names[step], step: "\(step + 1) of \(names.count)")
                ScrollView {
                    VStack(alignment: .leading, spacing: 14) { page }
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                controls
            }
            .padding(.horizontal, 22)
            .padding(.vertical, 14)
        }
        .onAppear(perform: reload)
    }

    // MARK: - Chrome

    private var controls: some View {
        HStack(spacing: 12) {
            DeckButton(title: "Back", icon: "chevron.left", enabled: step > 0) {
                step -= 1
            }
            .frame(width: 140)
            Spacer(minLength: 0)
            if step < names.count - 1 {
                DeckButton(title: "Next", icon: "chevron.right", prominent: true) {
                    reload()
                    step += 1
                }
                .frame(width: 150)
            } else {
                DeckButton(title: "Done", icon: "checkmark", prominent: true) {
                    finish(.frontDoor)
                }
                .frame(width: 150)
            }
        }
    }

    // MARK: - Pages

    @ViewBuilder private var page: some View {
        switch step {
        case 0: welcome
        case 1: folders
        case 2: software
        case 3: discsPage
        case 4: windows
        case 5: controlsPage
        default: ready
        }
    }

    private var welcome: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("DOSDeck is a PC from the DOS years in your pocket: it runs DOS games "
               + "and programs, and Windows 98 if you own a copy.")
            Panel(title: "What the words mean") {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("Software: a folder of DOS files, or a .ZIP of one. Nothing to "
                         + "install — put it in the DOS folder and it is listed.")
                    bullet("Disc: a CD or floppy image (.ISO, .CUE, .IMG) that a game, or "
                         + "Windows Setup, needs in the drive.")
                    bullet("Machine: the PC itself — a DOS machine you configure, or a "
                         + "Windows 98 machine DOSDeck keeps on its own hard disk.")
                }
                .padding(14)
            }
            dim("The next few steps show where each of those goes, check that DOSDeck "
              + "can see them, and explain the controls. DOSDeck supplies no games and "
              + "no Windows: you bring your own.")
        }
    }

    private var folders: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("Everything goes in through the Files app. DOSDeck has four folders "
               + "there:")
            Panel(title: "Files › On My iPhone (or iPad) › DOSDeck") {
                VStack(spacing: 0) {
                    folderRow("DOS", "Games and programs", "folder", titles.count)
                    folderRow("Discs", "CD and floppy images", "opticaldisc", discs.count)
                    folderRow("Media", "Box art, named after the game", "photo", nil)
                    folderRow("Windows", "The Windows 98 machine — DOSDeck fills this",
                              "desktopcomputer", nil)
                }
            }
            dim("iPhone and iPad only let an app read its own folder, so these are the "
              + "only places DOSDeck can look. AirDrop, iCloud Drive, a USB stick and a "
              + "computer over Finder all arrive in Files, so all of them work.")
        }
    }

    private var software: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("To add a game or program:")
            Panel {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("Open Files and go to DOSDeck › DOS.")
                    bullet("Copy the game's folder in — or its .ZIP; DOSDeck unpacks it "
                         + "the first time it runs.")
                    bullet("Or use Add files… on the DOS screen, which copies it in for "
                         + "you.")
                }
                .padding(14)
            }
            found(ownTitles, one: "title", none: "Only the demo so far")
            dim("The program to start is found for you. If a game will not start, the "
              + "Machine page on the DOS screen changes the graphics card, CPU speed "
              + "and sound card.")
        }
    }

    private var discsPage: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("CD and floppy images go in DOSDeck › Discs: .ISO, .CUE with its .BIN, "
               + ".IMG or .CHD.")
            Panel {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("A game that needs its CD: choose the disc on the DOS screen "
                         + "before Start, or change it later from Discs while it runs.")
                    bullet("Your Windows 98 CD goes here too.")
                    bullet("FREEDOS.IMG is already here: a FreeDOS boot floppy, so any "
                         + "machine can start at a DOS prompt.")
                }
                .padding(14)
            }
            found(discs.count, one: "disc image", none: "No disc images yet")
        }
    }

    private var windows: some View {
        let windowsDiscs = discs.filter { $0.name.lowercased() != "freedos.img" }
        return VStack(alignment: .leading, spacing: 14) {
            para("Optional. Windows 98 runs the Windows games of the late 90s, on a hard "
               + "disk DOSDeck makes and keeps.")
            Panel(title: "You need") {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("Your own Windows 98 SE CD as an image, in the Discs folder, "
                         + "and its product key.")
                    bullet("About 20 minutes, and a few GB free.")
                }
                .padding(14)
            }
            if win.phase == Phase.run {
                good("Windows 98 is installed. Choose Windows on the next screen to "
                   + "start it.")
            } else {
                if windowsDiscs.isEmpty {
                    dim("No CD image in Discs yet. Add it, then come back to this step.")
                } else {
                    good("Disc found: \(windowsDiscs[0].name)")
                }
                DeckButton(title: "Set up Windows 98 now", icon: "opticaldisc",
                           enabled: !windowsDiscs.isEmpty) { finish(.windows) }
                    .frame(maxWidth: 320)
            }
            dim("Skip this if you like: Windows on the front door does it any time.")
        }
    }

    private var controlsPage: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("While a machine runs, the picture gets the whole screen. Four chips in "
               + "the corner do everything else:")
            Panel {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("Run: start something else from the DOS folder, without "
                         + "restarting.")
                    bullet("Discs: put a CD or floppy in the drive, or change it.")
                    bullet("Keys: a full PC keyboard — Esc, F-keys, arrows, Ctrl and Alt.")
                    bullet("More: screen shape, pointer speed and reset.")
                }
                .padding(14)
            }
            Panel(title: "The mouse") {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("The screen is a trackpad: drag to move the pointer.")
                    bullet("Tap to click; tap with two fingers to right-click.")
                    bullet("Press and hold, then drag, to drag and drop.")
                }
                .padding(14)
            }
            dim("A Bluetooth keyboard and mouse work as they would on a PC. One machine "
              + "runs per launch: to switch from DOS to Windows, close DOSDeck from the "
              + "app switcher and open it again.")
        }
    }

    private var ready: some View {
        VStack(alignment: .leading, spacing: 14) {
            para("That is everything.")
            Panel(title: "What DOSDeck can see") {
                VStack(alignment: .leading, spacing: 8) {
                    bullet("\(titles.count) title\(titles.count == 1 ? "" : "s") in DOS")
                    bullet("\(discs.count) disc image\(discs.count == 1 ? "" : "s") in Discs")
                    bullet("Windows 98: " + (win.phase == Phase.run ? "installed"
                                                                     : "not set up"))
                }
                .padding(14)
            }
            if let demo = demoFolder {
                DeckButton(title: "Try the demo now", icon: "play.fill") {
                    done = true
                    _ = engine.startDOS(machine: Engine.loadMachine(), mount: demo,
                                        runCommand: Storage.runnable(in: demo),
                                        title: "Demo")
                }
                .frame(maxWidth: 320)
                dim("The demo is a machine like any other, so it uses this launch's one "
                  + "session.")
            }
            dim("This guide is on the front door, as Setup guide, whenever you want it "
              + "again.")
        }
    }

    // MARK: - Pieces

    /// Everything in DOS but the bundled demo, which is not something you added.
    private var ownTitles: Int {
        titles.filter { $0.name.caseInsensitiveCompare("Demo") != .orderedSame }.count
    }

    private var demoFolder: URL? {
        let demo = Storage.dos.appendingPathComponent("Demo", isDirectory: true)
        return Storage.runnable(in: demo) == nil ? nil : demo
    }

    private func reload() {
        titles = Storage.titles()
        discs = Storage.discImages()
        win.load()
    }

    private func finish(_ route: RootView.Route) {
        done = true
        step = 0
        onFinish(route)
    }

    private func folderRow(_ name: String, _ what: String, _ icon: String,
                           _ count: Int?) -> some View {
        ChoiceRow(title: name, subtitle: what, icon: icon) {
            if let count {
                Text(count == 0 ? "empty" : "\(count)")
                    .font(Theme.small)
                    .foregroundColor(count == 0 ? Theme.amberDim : Theme.cursor)
            }
        }
    }

    /// The step checks itself: instructions followed in another app are ones
    /// you cannot tell you got right, so the count is what answers that.
    private func found(_ n: Int, one: String, none: String) -> some View {
        HStack(spacing: 12) {
            DeckButton(title: "Look again", icon: "arrow.clockwise") { reload() }
                .frame(width: 190)
            if n > 0 {
                Text("\(n) \(one)\(n == 1 ? "" : "s") found")
                    .font(Theme.body.weight(.semibold))
                    .foregroundColor(Theme.cursor)
                    .phosphor(Theme.cursor, strength: 0.5)
            } else {
                Text(none).font(Theme.small).foregroundColor(Theme.amberDim)
            }
        }
    }

    private func para(_ s: String) -> some View {
        Text(s).font(Theme.body).foregroundColor(Theme.amber)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func dim(_ s: String) -> some View {
        Text(s).font(Theme.small).foregroundColor(Theme.amberDim)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func good(_ s: String) -> some View {
        Text(s).font(Theme.body.weight(.semibold)).foregroundColor(Theme.cursor)
            .phosphor(Theme.cursor, strength: 0.5)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func bullet(_ s: String) -> some View {
        HStack(alignment: .top, spacing: 7) {
            Text("·").foregroundColor(Theme.amberDim)
            Text(s).font(Theme.small).foregroundColor(Theme.amber.opacity(0.85))
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
