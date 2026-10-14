//
//  FrontDoor.swift
//
//  The first screen: one question, two answers.
//
//  "Windows or DOS" is not a menu of features, it is the machine you are
//  switching on, and it is asked once because a machine is chosen once. That
//  happens to be exactly what the emulator underneath allows -- one session
//  per launch -- but it is also the honest shape for what the app does. The
//  two paths diverge completely after this: a DOS machine with every DOSBox-X
//  option in front of you, or a Windows machine that walks you through an
//  installation and then lets you change discs while it runs.
//
//  There is no sidebar, no library rail, and no list of titles behind a rail,
//  which is the shape the rest of the family uses and the shape App Review
//  read as one app submitted several times.
//

import Combine
import SwiftUI

struct FrontDoor: View {
    @EnvironmentObject var engine: Engine
    /// Which door was opened. The root decides what that means.
    var onChoose: (RootView.Route) -> Void
    /// Open straight onto About. Only a store capture asks for this.
    var openAbout = false
    @State private var showAbout = false

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()

            VStack(spacing: 0) {
                header
                Spacer(minLength: 12)
                choices
                Spacer(minLength: 12)
                footer
            }
            .padding(.horizontal, 28)
            .padding(.vertical, 18)
        }
        .sheet(isPresented: $showAbout) { AboutView() }
        .onAppear { if openAbout { showAbout = true } }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text("DOSDECK")
                    .font(Theme.mono(34, .bold))
                    .kerning(4)
                    .foregroundColor(Theme.amber)
                    .phosphor()
                // The block cursor, blinking, because the icon has one and
                // this is the same screen the icon is a picture of.
                BlinkingCursor()
            }
            // The GPL asks for the credit and it belongs here anyway: this is
            // DOSBox-X doing the work, under its own name, on the first screen.
            Text("powered by DOSBox-X")
                .font(Theme.small)
                .foregroundColor(Theme.amberDim)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var choices: some View {
        HStack(spacing: 18) {
            DoorCard(
                title: "DOS",
                icon: "chevron.right.square",
                lines: ["A DOS machine, set up the way you want it.",
                        "Every DOSBox-X option: graphics card, CPU, cycles, "
                        + "memory, sound card, EMS and UMB.",
                        "Runs the folders and ZIP archives in your DOS folder."],
                action: { onChoose(.dos) })

            DoorCard(
                title: "WINDOWS",
                icon: "opticaldisc",
                lines: ["Install Windows 98 from your own CD, one step at a time.",
                        "DOSDeck makes the disk and boots the disc; Setup does the rest.",
                        "Change the CD in the drive while Windows is running."],
                action: { onChoose(.windows) })
        }
    }

    private var footer: some View {
        HStack(spacing: 16) {
            Text("Your software goes in the DOSDeck folder in Files.")
                .font(Theme.tiny)
                .foregroundColor(Theme.amberDim)
            Spacer()
            Button("About") { showAbout = true }
                .font(Theme.small)
                .foregroundColor(Theme.amber)
        }
    }
}

/// One of the two doors. Deliberately large and text-heavy: the choice decides
/// the whole session, so it is worth reading, and a card with three lines of
/// plain English is a better front door than two icons.
private struct DoorCard: View {
    let title: String
    let icon: String
    let lines: [String]
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 10) {
                    Image(systemName: icon)
                        .font(.system(size: 22, weight: .semibold))
                    Text(title)
                        .font(Theme.mono(24, .bold))
                        .kerning(2)
                }
                .foregroundColor(Theme.amber)
                .phosphor(strength: 0.7)

                Rectangle().fill(Theme.rule).frame(height: 1)

                VStack(alignment: .leading, spacing: 7) {
                    ForEach(lines, id: \.self) { line in
                        HStack(alignment: .top, spacing: 7) {
                            Text("·").foregroundColor(Theme.amberDim)
                            Text(line)
                                .font(Theme.small)
                                .foregroundColor(Theme.amber.opacity(0.78))
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                Spacer(minLength: 0)
                HStack(spacing: 6) {
                    Text("Set up")
                    Image(systemName: "arrow.right")
                }
                .font(Theme.small.weight(.semibold))
                .foregroundColor(Theme.cursor)
            }
            .padding(18)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(Theme.panel)
            .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

/// The block cursor from the icon. One timer, one rectangle; it is the app's
/// signature and it costs nothing.
struct BlinkingCursor: View {
    @State private var on = true
    // A timer rather than a repeating animation: a cursor does not fade, it is
    // on or it is off, and the animation form of this blinks at whatever rate
    // SwiftUI feels like when the view is rebuilt.
    private let tick = Timer.publish(every: 0.53, on: .main, in: .common).autoconnect()

    var body: some View {
        Rectangle()
            .fill(Theme.cursor)
            .frame(width: 14, height: 26)
            .opacity(on ? 1 : 0)
            .phosphor(Theme.cursor, strength: 0.8)
            .onReceive(tick) { _ in on.toggle() }
    }
}

// MARK: - About

struct AboutView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text("DOSDeck")
                        .font(Theme.title)
                        .foregroundColor(Theme.amber)
                        .phosphor()

                    section("The emulator",
                            "DOSDeck runs DOSBox-X, which is free software under the "
                          + "GNU General Public License, version 2. It is used unmodified "
                          + "in every respect that matters to the emulation; the interface "
                          + "around it is ours.\n\ndosbox-x.com")

                    section("No just-in-time compilation",
                            "The CPU is an interpreter. The dynamic cores are not compiled "
                          + "into this build at all, so no memory is ever both writable and "
                          + "executable.")

                    section("No network",
                            "DOSDeck has no network features of any kind — no account, no "
                          + "sign-in, no analytics, no update check. DOSBox-X's IPX, "
                          + "Ethernet and serial-over-TCP support is configured off and the "
                          + "build fails if the finished library imports a socket call.")

                    section("Your software is yours",
                            "DOSDeck offers no catalogue, no store and no downloads, and it "
                          + "cannot obtain software. It runs the files you put in its folder "
                          + "in the Files app. It does not supply MS-DOS, Windows, or any "
                          + "game.")

                    section("What ships inside",
                            "A small demonstration program written for this project, and a "
                          + "FreeDOS 1.3 boot floppy image (GNU GPL v2, freedos.org), so "
                          + "that the app has something to run before you have added "
                          + "anything.")
                }
                .padding(24)
            }
            VStack {
                HStack {
                    Spacer()
                    Button { dismiss() } label: {
                        Image(systemName: "xmark")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundColor(Theme.amber)
                            .padding(12)
                    }
                }
                Spacer()
            }
        }
    }

    private func section(_ title: String, _ body: String) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title.uppercased())
                .font(Theme.small.weight(.semibold))
                .kerning(1.4)
                .foregroundColor(Theme.cursor)
            Text(body)
                .font(Theme.small)
                .foregroundColor(Theme.amber.opacity(0.8))
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
