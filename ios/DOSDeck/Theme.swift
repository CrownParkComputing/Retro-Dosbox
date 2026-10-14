//
//  Theme.swift
//
//  What DOSDeck looks like.
//
//  The palette is the app's own icon, read off tool/make_dosdeck_art.py: an
//  amber phosphor screen, nearly black with a warm cast, and type that glows
//  rather than casts a shadow. That is not decoration -- it is the answer to
//  the rejection. The Retro-* family shares one look on purpose (blue chrome,
//  the Retro script, a left rail beside a list of titles) and Apple read the
//  family as one app submitted many times under guideline 4.3. So DOSDeck
//  shares none of it: different colours, different type, different shape of
//  screen, and no rail anywhere in the app.
//
//  Everything here is deliberately small and literal. There is no design
//  system to learn; a screen that needs a box uses Panel, and one that needs a
//  heading uses ScreenTitle.
//

import SwiftUI

enum Theme {

    // MARK: - Colour

    /// The screen itself: a vertical gradient from a warm near-black to an
    /// almost pure one, which is what an amber CRT looks like with the beam
    /// off at the bottom of the tube.
    static let screenTop    = Color(red: 16 / 255, green: 11 / 255, blue: 4 / 255)
    static let screenBottom = Color(red: 6 / 255, green: 4 / 255, blue: 2 / 255)

    /// P3-era amber phosphor. `amber` is text you are meant to read, `amberDim`
    /// is text that is present but not being asked for, and `cursor` is the one
    /// thing on screen that is brighter than the rest.
    static let amber    = Color(red: 255 / 255, green: 176 / 255, blue: 0 / 255)
    static let amberDim = Color(red: 156 / 255, green: 104 / 255, blue: 8 / 255)
    static let cursor   = Color(red: 255 / 255, green: 208 / 255, blue: 96 / 255)

    /// A panel is a lighter rectangle of the same screen, not a card floating
    /// over it -- a CRT has no depth and the interface should not pretend to.
    static let panel       = Color(red: 28 / 255, green: 20 / 255, blue: 8 / 255)
    static let panelRaised = Color(red: 40 / 255, green: 29 / 255, blue: 11 / 255)
    static let rule        = Color(red: 92 / 255, green: 62 / 255, blue: 10 / 255)

    /// The one non-amber colour in the app, for the rare thing that is wrong.
    /// Red on amber is legible; a second accent hue would not be.
    static let alarm = Color(red: 255 / 255, green: 96 / 255, blue: 72 / 255)

    static var background: LinearGradient {
        LinearGradient(colors: [screenTop, screenBottom],
                       startPoint: .top, endPoint: .bottom)
    }

    // MARK: - Type
    //
    // Monospaced throughout, because this is a DOS machine and because it is
    // the clearest possible break from the family's proportional UI. SF Mono
    // comes free with `.monospaced`, so no font ships in the bundle.

    static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }

    static let title   = mono(30, .bold)
    static let heading = mono(19, .semibold)
    static let body    = mono(14)
    static let small   = mono(12)
    static let tiny    = mono(10)
}

// MARK: - The glow
//
// One modifier, used sparingly. A phosphor glow is the type blurred behind
// itself; SwiftUI's shadow with zero offset is exactly that, and two passes
// read as light rather than as a drop shadow.

extension View {
    func phosphor(_ colour: Color = Theme.amber, strength: CGFloat = 1) -> some View {
        self.shadow(color: colour.opacity(0.55 * strength), radius: 3 * strength)
            .shadow(color: colour.opacity(0.30 * strength), radius: 9 * strength)
    }
}

// MARK: - Panels

/// A bordered region of screen. The border is a hairline rule rather than a
/// rounded card: DOS interfaces were drawn with box characters and this is the
/// nearest honest equivalent that is still a native control.
struct Panel<Content: View>: View {
    var title: String? = nil
    var raised = false
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let title {
                Text(title.uppercased())
                    .font(Theme.small.weight(.semibold))
                    .kerning(1.5)
                    .foregroundColor(Theme.amberDim)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                Rectangle().fill(Theme.rule).frame(height: 1)
            }
            content
        }
        .background(raised ? Theme.panelRaised : Theme.panel)
        .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
    }
}

/// The heading at the top of a screen, with the running C:\> prompt that gives
/// the app its identity. `step` draws the wizard's position without a stepper
/// control, which on a 390-point-tall landscape phone would cost more height
/// than it is worth.
struct ScreenTitle: View {
    let text: String
    var step: String? = nil
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("C:\\>")
                .font(Theme.heading)
                .foregroundColor(Theme.amberDim)
            Text(text)
                .font(Theme.heading)
                .foregroundColor(Theme.amber)
                .phosphor()
            Spacer()
            if let step {
                Text(step)
                    .font(Theme.small)
                    .foregroundColor(Theme.amberDim)
            }
        }
    }
}

// MARK: - Controls

/// The app's button. Filled when it is the thing to press, outlined otherwise.
struct DeckButton: View {
    let title: String
    var icon: String? = nil
    var prominent = false
    var enabled = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if let icon { Image(systemName: icon) }
                Text(title)
            }
            .font(Theme.body.weight(.semibold))
            .foregroundColor(prominent ? Theme.screenTop : Theme.amber)
            .padding(.horizontal, 18)
            .padding(.vertical, 11)
            .frame(maxWidth: .infinity)
            .background(prominent ? Theme.amber : Theme.panelRaised)
            .overlay(Rectangle().stroke(prominent ? Color.clear : Theme.rule, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .opacity(enabled ? 1 : 0.35)
        .disabled(!enabled)
    }
}

/// A row that is also a choice: the list idiom used for software, discs and
/// drives. Selection is a filled left edge rather than a checkmark, so a row
/// reads at a glance from across a table.
struct ChoiceRow<Trailing: View>: View {
    let title: String
    var subtitle: String? = nil
    var icon: String = "folder"
    var selected = false
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(spacing: 12) {
            Rectangle()
                .fill(selected ? Theme.amber : Color.clear)
                .frame(width: 3)
            Image(systemName: icon)
                .font(.system(size: 15))
                .foregroundColor(selected ? Theme.amber : Theme.amberDim)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(Theme.body.weight(selected ? .semibold : .regular))
                    .foregroundColor(selected ? Theme.amber : Theme.amber.opacity(0.85))
                    .lineLimit(1)
                if let subtitle {
                    Text(subtitle)
                        .font(Theme.tiny)
                        .foregroundColor(Theme.amberDim)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            trailing
        }
        .padding(.trailing, 12)
        .padding(.vertical, 9)
        .background(selected ? Theme.panelRaised : Color.clear)
        .contentShape(Rectangle())
    }
}

extension ChoiceRow where Trailing == EmptyView {
    init(title: String, subtitle: String? = nil, icon: String = "folder", selected: Bool = false) {
        self.init(title: title, subtitle: subtitle, icon: icon, selected: selected) { EmptyView() }
    }
}

/// A labelled setting. Used by the machine options, which is the one screen in
/// the app that is a dense list of controls.
struct OptionRow<Control: View>: View {
    let label: String
    var note: String? = nil
    @ViewBuilder var control: Control

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 1) {
                Text(label)
                    .font(Theme.body)
                    .foregroundColor(Theme.amber.opacity(0.9))
                if let note {
                    Text(note)
                        .font(Theme.tiny)
                        .foregroundColor(Theme.amberDim)
                        .lineLimit(2)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            control
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
    }
}

/// A menu-backed picker drawn as amber text, because the system picker is grey
/// chrome and would be the only non-DOSDeck-looking thing on the screen.
struct DeckPicker: View {
    let options: [(value: String, label: String)]
    @Binding var selection: String

    var body: some View {
        Menu {
            ForEach(options, id: \.value) { option in
                Button(option.label) { selection = option.value }
            }
        } label: {
            HStack(spacing: 6) {
                Text(options.first { $0.value == selection }?.label ?? selection)
                    .font(Theme.small.weight(.semibold))
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 9))
            }
            .foregroundColor(Theme.amber)
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .background(Theme.panelRaised)
            .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
        }
    }
}

/// A switch in the app's own colours. SwiftUI's Toggle tint is the only part
/// of it that can be themed, so the label is drawn by OptionRow instead.
struct DeckToggle: View {
    @Binding var on: Bool
    var body: some View {
        Toggle("", isOn: $on)
            .labelsHidden()
            .tint(Theme.amber)
    }
}

/// An empty list says what to do about being empty. Every list in this app can
/// legitimately be empty on a fresh install, and "nothing here" with no next
/// step is how an emulator comes to look broken to a reviewer.
struct EmptyNote: View {
    let title: String
    let detail: String
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(Theme.body.weight(.semibold))
                .foregroundColor(Theme.amber)
            Text(detail)
                .font(Theme.small)
                .foregroundColor(Theme.amberDim)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
    }
}
