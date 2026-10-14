//
//  Shot.swift
//
//  Opening on a named screen, for the store captures.
//
//  simctl has no tap API, so there is no way to drive the interface from the
//  capture script: the app has to be told which screen to open on. RETRODOS_SHOT
//  is how, and SIMCTL_CHILD_RETRODOS_SHOT is how the script sets it.
//
//  Inert in the shipped app, and not by politeness: nothing can set an
//  environment variable for an App Store app on a device. It is also read
//  exactly once, at the first screen, and can do nothing a user could not do
//  by tapping -- the "session" shot starts the bundled demo, which is two taps
//  away in any case.
//
import Foundation

enum Shot {
    enum Screen: String {
        case front      // the Windows-or-DOS front door
        case dos        // the DOS wizard, choosing software
        case machine    // the DOS wizard, the hardware options
        case windows    // the Windows wizard, the installation steps
        case about      // credits and the licence
        case session    // a DOS machine actually running the bundled demo
    }

    static var requested: Screen? {
        guard let raw = ProcessInfo.processInfo.environment["RETRODOS_SHOT"] else {
            return nil
        }
        return Screen(rawValue: raw)
    }
}
