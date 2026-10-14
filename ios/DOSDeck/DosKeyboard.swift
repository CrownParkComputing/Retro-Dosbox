//
//  DosKeyboard.swift
//
//  A PC keyboard, because a DOS machine needs one and iOS does not have one.
//
//  Scancodes, not characters. DOS programs read the keyboard at the scancode
//  level: a character API cannot express a held arrow key, cannot tell the two
//  Alt keys apart, and has nothing to say about Ctrl-Break. So every key here
//  sends a press and a release of an SDL scancode, which is what the engine's
//  mapper takes, and the guest sees a normal key.
//
//  The modifiers LATCH rather than auto-release. Ctrl-Alt-Delete, Alt-X and
//  Ctrl-C are three-handed on a phone otherwise -- and a latched modifier is
//  also how you hold Shift while aiming with the other thumb.
//

import SwiftUI

struct DosKeyboard: View {
    /// (scancode, pressed)
    let send: (Int32, Bool) -> Void

    @State private var ctrl = false
    @State private var alt = false
    @State private var shift = false

    private struct Key: Identifiable {
        let id = UUID()
        let cap: String
        let code: Int32
        var width: CGFloat = 1
        /// Latching modifier rather than a key that is pressed and released.
        var modifier: Modifier? = nil
        enum Modifier { case ctrl, alt, shift }
    }

    private var rows: [[Key]] {
        [
            [
                Key(cap: "Esc", code: DD_SCAN_ESCAPE, width: 1.3),
                Key(cap: "F1", code: DD_SCAN_F1), Key(cap: "F2", code: DD_SCAN_F2),
                Key(cap: "F3", code: DD_SCAN_F3), Key(cap: "F4", code: DD_SCAN_F4),
                Key(cap: "F5", code: DD_SCAN_F5), Key(cap: "F6", code: DD_SCAN_F6),
                Key(cap: "F7", code: DD_SCAN_F7), Key(cap: "F8", code: DD_SCAN_F8),
                Key(cap: "F9", code: DD_SCAN_F9), Key(cap: "F10", code: DD_SCAN_F10),
                Key(cap: "F11", code: DD_SCAN_F11), Key(cap: "F12", code: DD_SCAN_F12),
            ],
            [
                Key(cap: "`", code: DD_SCAN_GRAVE),
                Key(cap: "1", code: DD_SCAN_1), Key(cap: "2", code: DD_SCAN_2),
                Key(cap: "3", code: DD_SCAN_3), Key(cap: "4", code: DD_SCAN_4),
                Key(cap: "5", code: DD_SCAN_5), Key(cap: "6", code: DD_SCAN_6),
                Key(cap: "7", code: DD_SCAN_7), Key(cap: "8", code: DD_SCAN_8),
                Key(cap: "9", code: DD_SCAN_9), Key(cap: "0", code: DD_SCAN_0),
                Key(cap: "-", code: DD_SCAN_MINUS), Key(cap: "=", code: DD_SCAN_EQUALS),
                Key(cap: "Bksp", code: DD_SCAN_BACKSPACE, width: 1.6),
            ],
            [
                Key(cap: "Tab", code: DD_SCAN_TAB, width: 1.4),
                Key(cap: "Q", code: DD_SCAN_Q), Key(cap: "W", code: DD_SCAN_W),
                Key(cap: "E", code: DD_SCAN_E), Key(cap: "R", code: DD_SCAN_R),
                Key(cap: "T", code: DD_SCAN_T), Key(cap: "Y", code: DD_SCAN_Y),
                Key(cap: "U", code: DD_SCAN_U), Key(cap: "I", code: DD_SCAN_I),
                Key(cap: "O", code: DD_SCAN_O), Key(cap: "P", code: DD_SCAN_P),
                Key(cap: "[", code: DD_SCAN_LEFTBRACKET),
                Key(cap: "]", code: DD_SCAN_RIGHTBRACKET),
                Key(cap: "\\", code: DD_SCAN_BACKSLASH),
            ],
            [
                Key(cap: "Caps", code: DD_SCAN_CAPSLOCK, width: 1.7),
                Key(cap: "A", code: DD_SCAN_A), Key(cap: "S", code: DD_SCAN_S),
                Key(cap: "D", code: DD_SCAN_D), Key(cap: "F", code: DD_SCAN_F),
                Key(cap: "G", code: DD_SCAN_G), Key(cap: "H", code: DD_SCAN_H),
                Key(cap: "J", code: DD_SCAN_J), Key(cap: "K", code: DD_SCAN_K),
                Key(cap: "L", code: DD_SCAN_L),
                Key(cap: ";", code: DD_SCAN_SEMICOLON),
                Key(cap: "'", code: DD_SCAN_APOSTROPHE),
                Key(cap: "Enter", code: DD_SCAN_RETURN, width: 2),
            ],
            [
                Key(cap: "Shift", code: DD_SCAN_LSHIFT, width: 2.1, modifier: .shift),
                Key(cap: "Z", code: DD_SCAN_Z), Key(cap: "X", code: DD_SCAN_X),
                Key(cap: "C", code: DD_SCAN_C), Key(cap: "V", code: DD_SCAN_V),
                Key(cap: "B", code: DD_SCAN_B), Key(cap: "N", code: DD_SCAN_N),
                Key(cap: "M", code: DD_SCAN_M),
                Key(cap: ",", code: DD_SCAN_COMMA), Key(cap: ".", code: DD_SCAN_PERIOD),
                Key(cap: "/", code: DD_SCAN_SLASH),
                Key(cap: "↑", code: DD_SCAN_UP),
                Key(cap: "Del", code: DD_SCAN_DELETE, width: 1.3),
            ],
            [
                Key(cap: "Ctrl", code: DD_SCAN_LCTRL, width: 1.6, modifier: .ctrl),
                Key(cap: "Alt", code: DD_SCAN_LALT, width: 1.5, modifier: .alt),
                Key(cap: "Space", code: DD_SCAN_SPACE, width: 5.4),
                Key(cap: "←", code: DD_SCAN_LEFT),
                Key(cap: "↓", code: DD_SCAN_DOWN),
                Key(cap: "→", code: DD_SCAN_RIGHT),
                Key(cap: "PgUp", code: DD_SCAN_PAGEUP, width: 1.2),
                Key(cap: "PgDn", code: DD_SCAN_PAGEDOWN, width: 1.2),
            ],
        ]
    }

    var body: some View {
        VStack(spacing: 3) {
            ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                GeometryReader { geo in
                    let units = row.reduce(0) { $0 + $1.width }
                    let gaps = CGFloat(row.count - 1) * 3
                    let unit = max(18, (geo.size.width - gaps) / units)
                    HStack(spacing: 3) {
                        ForEach(row) { key in
                            keyButton(key, width: unit * key.width)
                        }
                    }
                }
                .frame(height: 32)
            }
        }
        .padding(6)
        .background(Theme.panel.opacity(0.95))
        .overlay(Rectangle().stroke(Theme.rule, lineWidth: 1))
    }

    private func keyButton(_ key: Key, width: CGFloat) -> some View {
        let latched = isLatched(key)
        return Button {
            press(key)
        } label: {
            Text(key.cap)
                .font(Theme.mono(key.cap.count > 2 ? 10 : 12, .medium))
                .foregroundColor(latched ? Theme.screenTop : Theme.amber)
                .frame(width: width, height: 30)
                .background(latched ? Theme.amber : Theme.panelRaised)
                .overlay(Rectangle().stroke(latched ? Color.clear : Theme.rule, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }

    private func isLatched(_ key: Key) -> Bool {
        switch key.modifier {
        case .ctrl:  return ctrl
        case .alt:   return alt
        case .shift: return shift
        case nil:    return false
        }
    }

    private func press(_ key: Key) {
        // A modifier is held until it is tapped again, and the key itself is
        // pressed or released to match -- the guest is told exactly what a
        // real keyboard would tell it.
        if let modifier = key.modifier {
            let now: Bool
            switch modifier {
            case .ctrl:  ctrl.toggle();  now = ctrl
            case .alt:   alt.toggle();   now = alt
            case .shift: shift.toggle(); now = shift
            }
            send(key.code, now)
            return
        }

        send(key.code, true)
        // 40ms, not zero: a guest that polls the keyboard can miss a press and
        // release that happen in the same instant.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.04) {
            send(key.code, false)
            // Shift is a one-shot in practice -- it is held to type one
            // capital -- while Ctrl and Alt are usually wanted for a
            // combination and are left latched until tapped off.
            if shift {
                shift = false
                send(DD_SCAN_LSHIFT, false)
            }
        }
    }
}
