//
//  WindowsMachine.swift
//
//  The Windows machine, and the walkthrough that installs it.
//
//  DOSDeck supplies no operating system. It makes an empty disk, boots the CD
//  the user already owns, and stays out of the way while Microsoft's installer
//  does the rest -- which is exactly the position the DOSBox-X guide this
//  follows takes: "Getting this image file is outside the scope of this
//  guide." There is no disc here, no link to one, and no way for the app to
//  obtain one.
//
//  WHERE THE PHASES LIVE
//
//  An installation reboots the machine several times, and each boot has to
//  start something different: the CD while Windows is not on the disk yet, the
//  disk once it is. On a desktop each of those is a separate DOSBox-X session.
//  iOS gets one session per launch, so the decision lives in GO.BAT instead,
//  where it is re-made at every boot -- including the reboots Setup asks for
//  itself. See dd_write_phase_batch().
//
//  What is left here is the part a person has to be told: which step they are
//  on and what to do on it.
//

import Foundation

final class WindowsMachine: ObservableObject {

    @Published private(set) var raw = DDWinMachine()

    /// Re-read from the folder every time it is asked for, because the truth
    /// about an installation is on disk and not in this object: Setup writes
    /// to the disk image, and the app finds out by looking.
    @Published private(set) var phase: Int32 = DD_PHASE_CREATE

    var confURL: URL { Storage.windows.appendingPathComponent("machine.conf") }
    var batchURL: URL { Storage.windows.appendingPathComponent("GO.BAT") }
    var diskURL: URL { Storage.windows.appendingPathComponent(CField.get(raw.hdd)) }

    init() { load() }

    func load() {
        var m = DDWinMachine()
        if !dd_win_load(Storage.windows.path, &m) {
            dd_win_defaults(&m, Storage.windows.path, DD_OS_WIN98)
        }
        raw = m
        refresh()
    }

    func refresh() {
        var m = raw
        phase = dd_win_true_phase(&m)
    }

    @discardableResult
    func save() -> Bool {
        var m = raw
        let ok = dd_win_save(&m)
        refresh()
        return ok
    }

    // MARK: - Settings

    var iso: URL? {
        get {
            let p = CField.get(raw.iso)
            return p.isEmpty ? nil : URL(fileURLWithPath: p)
        }
        set {
            CField.set(newValue?.path ?? "", into: &raw.iso)
            save()
        }
    }

    /// 0 means the 8GB template from the guide, which is what almost everyone
    /// should use: with a reported DOS version of 7.1 anything over 512MB is
    /// FAT32, and Windows 98's own IDE driver cannot handle a volume over
    /// 128GB whatever the filesystem allows.
    var sizeMB: Int32 {
        get { raw.size_mb }
        set { raw.size_mb = newValue; save() }
    }

    /// The guide's default is 128. Windows 98 does not boot reliably above
    /// 512MB without a patch, and nothing it runs needs more.
    var memsize: Int32 {
        get { raw.memsize }
        set { raw.memsize = newValue; save() }
    }

    /// A 3dfx Voodoo on the PCI bus. On by default: without one, the 3D games
    /// of Windows 98's own era cannot run at all -- they look for Glide or a
    /// Direct3D device and find neither, because the S3 Trio DOSBox-X gives
    /// the guest is a 2D card. It is a setting because the emulated Voodoo is
    /// a software rasteriser, and on a handheld it is the difference between a
    /// game that is slow and a game that does not move.
    var voodoo: Bool {
        get { raw.voodoo != 0 }
        set { raw.voodoo = newValue ? 1 : 0; save() }
    }

    // MARK: - State

    var diskExists: Bool { FileManager.default.fileExists(atPath: diskURL.path) }

    var installed: Bool {
        var m = raw
        return dd_win_installed(&m)
    }

    /// What the user still has to supply, or nil when the machine can start.
    var blocker: String? {
        var m = raw
        var buf = [CChar](repeating: 0, count: 512)
        dd_win_blocker(&m, &buf, Int32(buf.count))
        let s = String(cString: buf)
        return s.isEmpty ? nil : s
    }

    var osName: String { String(cString: dd_os_name(raw.os)) }

    // MARK: - Writing it out

    @discardableResult
    func writeConf() -> Bool {
        var m = raw
        return dd_write_win_conf(&m, confURL.path) == 0
    }

    @discardableResult
    func writePhaseBatch() -> Bool {
        var m = raw
        return dd_write_phase_batch(&m, batchURL.path) == 0
    }

    /// Back to the beginning, disk image and all. A half-installed Windows is
    /// not something Setup can be pointed at a second time, so "start again"
    /// has to mean a new disk. The chosen CD is kept.
    func startOver() {
        var m = raw
        dd_win_reset(&m, true)
        raw = m
        save()
        writePhaseBatch()
    }

    // MARK: - The walkthrough
    //
    // One entry per step, in the order they happen. The text is what somebody
    // who has never installed Windows 98 needs, and no more: the guide's own
    // sequence, in the words a person at a phone would use.

    struct Step: Identifiable {
        let id: Int32
        let title: String
        let detail: String
        /// What the user does next, inside the guest. Empty while DOSDeck is
        /// the one doing the work.
        let action: String?
    }

    var steps: [Step] {
        [
            Step(id: DD_PHASE_CREATE,
                 title: "Make the disk",
                 detail: "DOSDeck makes an empty hard disk for the machine. "
                       + "Nothing is installed on it yet and it takes a moment.",
                 action: nil),
            Step(id: DD_PHASE_INSTALL,
                 title: "Install from the CD",
                 detail: "The machine boots your Windows 98 CD. Setup runs twice: "
                       + "the first time you partition the disk and it restarts, "
                       + "the second time you format it and choose a Full installation.",
                 action: "First run: choose to partition, answer Yes to large disk support, "
                       + "then restart.\nSecond run: format the drive, then run Setup and "
                       + "install Windows."),
            Step(id: DD_PHASE_CONTINUE,
                 title: "Let Setup finish",
                 detail: "Windows is on the disk now, so the machine boots the disk and "
                       + "Setup carries on. It restarts itself several times — each one is "
                       + "normal, and DOSDeck boots the right thing every time.",
                 action: "Answer Setup's questions when it asks. Keep the CD in the drive: "
                       + "Windows asks for it until it has copied what it needs."),
            Step(id: DD_PHASE_RUN,
                 title: "Windows is installed",
                 detail: "The machine boots Windows from its own disk. Use the disc list "
                       + "to put a CD in the drive or change it while Windows is running.",
                 action: nil),
        ]
    }

    var currentStep: Step { steps.first { $0.id == phase } ?? steps[0] }
}
