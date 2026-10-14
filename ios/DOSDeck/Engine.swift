//
//  Engine.swift
//
//  The session. One per launch, and that is not a simplification: DOSBox-X
//  initialises its globals once and tears them down asymmetrically, so a
//  second run in the same process boots straight into a triple fault. Android
//  works around it by restarting its own process. iOS cannot -- an app that
//  calls exit() is a crash report -- so DOSDeck is built so that it never
//  needs to.
//
//  That constraint is the reason the app is shaped the way it is, and it turns
//  out to be the honest shape anyway. You choose Windows or DOS at the front
//  door, exactly as you would choose which machine to switch on, and then
//  everything happens inside that machine: mount a folder and run something
//  else, change the disc in the drive, reset to move an installation to its
//  next step. None of those needs a second session, and all of them are what a
//  real machine would do.
//

import Combine
import Foundation
import SwiftUI

// MARK: - Fixed C char arrays
//
// A `char name[32]` in a C struct arrives in Swift as a 32-element tuple,
// which cannot be assigned from a String. These two do it bytewise instead,
// which keeps the bridge free of a setter per field.

enum CField {
    static func set<T>(_ value: String, into field: inout T) {
        withUnsafeMutableBytes(of: &field) { raw in
            guard let base = raw.baseAddress, raw.count > 0 else { return }
            memset(base, 0, raw.count)
            value.utf8CString.withUnsafeBufferPointer { src in
                guard let from = src.baseAddress else { return }
                // src.count includes the NUL; leave room for ours.
                memcpy(base, from, min(raw.count - 1, max(0, src.count - 1)))
            }
        }
    }

    static func get<T>(_ field: T) -> String {
        var copy = field
        return withUnsafeBytes(of: &copy) { raw in
            guard let base = raw.baseAddress else { return "" }
            return String(cString: base.assumingMemoryBound(to: CChar.self))
        }
    }
}

// MARK: -

final class Engine: ObservableObject {

    enum Mode { case idle, dos, windows }

    /// What the machine is. Decided once, at the front door.
    @Published private(set) var mode: Mode = .idle

    /// True from the moment a session is asked for. The front door disappears
    /// at this point and does not come back, because it cannot.
    @Published private(set) var started = false

    /// True while the mainloop is actually turning. There is a gap of a second
    /// or two after `started` while DOSBox-X boots the machine.
    @Published private(set) var running = false

    @Published private(set) var fps = 0
    @Published private(set) var program = ""
    @Published private(set) var paused = false

    /// Something the user needs to be told. Shown as a banner, not an alert:
    /// an alert over a running machine steals the keyboard.
    @Published var notice: String?

    /// Drive letters this app assigns, and what is on them.
    ///
    ///   C:  whatever the session was started with, for its whole life
    ///   D:  "the other game" -- remounted each time one is run from the
    ///       in-session list, so a title is always at the root of its drive
    ///   E:  a CD image the user attached
    ///   A:  a floppy image
    ///
    /// Fixed rather than allocated, because a DOS user has to be able to TYPE
    /// the letter, and a drive that moves between sessions cannot be typed.
    static let bootDrive: CChar = 67     // 'C'
    static let otherDrive: CChar = 68    // 'D'
    static let cdDrive: CChar = 69       // 'E'
    static let floppyDrive: CChar = 65   // 'A'

    private var statusTimer: Timer?
    private var otherMounted = false

    // MARK: - Starting

    /// Start a DOS session.
    ///
    /// `mount` becomes C:. The wizard passes the chosen title's own folder, so
    /// that the title sits at the root of C: the way its installer expected --
    /// or, when nothing is chosen and the user wants a bare prompt, the whole
    /// DOS folder, so everything they own is one CD away. Running something
    /// ELSE later never touches C:; it mounts D: instead.
    @discardableResult
    func startDOS(machine: DDMachine, mount: URL?, runCommand: String?,
                  cd: URL? = nil, bootFloppy: URL? = nil,
                  title: String) -> Bool {
        var m = machine
        let conf = Storage.work.appendingPathComponent("dos.conf")
        let rc = dd_write_dos_conf(&m, title, mount?.path ?? "",
                                   runCommand ?? "", cd?.path ?? "",
                                   bootFloppy?.path ?? "", conf.path)
        guard rc == 0 else {
            notice = "DOSDeck could not write the machine configuration."
            return false
        }
        return start(conf: conf, workdir: mount ?? Storage.work, mode: .dos)
    }

    /// Start the Windows machine. The conf's [autoexec] calls GO.BAT from the
    /// machine folder, and the app rewrites GO.BAT between phases -- see
    /// WindowsMachine.
    @discardableResult
    func startWindows(_ win: WindowsMachine) -> Bool {
        guard win.writeConf(), win.writePhaseBatch() else {
            notice = "DOSDeck could not write the Windows machine configuration."
            return false
        }
        return start(conf: win.confURL, workdir: Storage.windows, mode: .windows)
    }

    private func start(conf: URL, workdir: URL, mode: Mode) -> Bool {
        guard !started else { return false }
        guard dd_start(conf.path, workdir.path) else {
            notice = "A machine has already run in this session."
            return false
        }
        self.mode = mode
        started = true

        // 2 Hz. The status line is a status line, not an instrument: polling
        // it faster costs battery and changes nothing anybody can read.
        statusTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.pollStatus()
        }
        return true
    }

    private func pollStatus() {
        running = dd_is_running()
        fps = Int(dd_fps())
        var buf = [CChar](repeating: 0, count: 64)
        dd_running_program(&buf, Int32(buf.count))
        program = String(cString: buf)
    }

    // MARK: - While it runs

    func setPaused(_ value: Bool) {
        guard started else { return }
        paused = value
        dd_set_pause(value)
    }

    func reset(hard: Bool = true) { dd_reset(hard) }

    /// Type a line at the DOS prompt. Only meaningful while a DOS shell is
    /// what is running: once Windows has booted, this types into Windows.
    func type(_ line: String) { dd_type(line) }

    @discardableResult
    func mount(_ drive: CChar, _ url: URL) -> Bool {
        dd_mount_dir(drive, url.path) == 0
    }

    func unmount(_ drive: CChar) { dd_unmount(drive) }

    /// Put a disc in the drive, or take it out with a nil path. Unlike typing
    /// IMGMOUNT, this works after a guest OS has booted -- which is the whole
    /// point of the Windows session's disc list.
    @discardableResult
    func insertCD(_ url: URL?, drive: CChar = Engine.cdDrive) -> Bool {
        dd_insert_cd(drive, url?.path ?? "") == 0
    }

    @discardableResult
    func insertFloppy(_ url: URL?, drive: CChar = Engine.floppyDrive) -> Bool {
        dd_insert_floppy(drive, url?.path ?? "") == 0
    }

    // MARK: - Running something else, without a second session

    /// Run another title inside the session that is already up.
    ///
    /// D: is "the other game" and is remounted for each one, so the title is
    /// always at the root of its own drive and there is no directory to change
    /// into. C: is whatever the session started with and is never disturbed.
    ///
    /// The drive is parked on C: before D: is unmounted, because DOS will not
    /// let go of the drive you are standing on.
    func run(title: Storage.Title) {
        switch title.kind {
        case .folder:
            guard let url = title.url else { return }
            mountAndRun(Self.mountRoot(of: url), named: title.name)
        case .archive:
            guard let url = title.url else { return }
            unpack(url, named: title.name) { [weak self] folder in
                guard let folder else { return }   // notice already says why
                self?.mountAndRun(Self.mountRoot(of: folder), named: title.name)
            }
        case .prompt:
            break   // only means anything as a way to START a session
        }
    }

    private func mountAndRun(_ folder: URL, named name: String) {
        guard let program = Storage.runnable(in: folder) else {
            notice = "There is no program to run in \(name)."
            return
        }
        type("C:")
        // Only unmount a drive that is actually mounted: DOS answers an
        // unmount of nothing with an error line, and the first thing a user
        // runs would print one for no reason.
        if otherMounted { unmount(Engine.otherDrive) }
        otherMounted = true
        guard mount(Engine.otherDrive, folder) else {
            notice = "DOSDeck could not mount \(name)."
            return
        }
        type("D:")
        type(program)
        notice = nil
    }

    /// Unpack an archive, once, and hand back the folder -- or nil, which the
    /// caller needs as much as the folder: a wizard showing "Unpacking…" has
    /// to stop showing it whichever way this ends. Work happens off the main
    /// thread; the completion arrives on it.
    private func unpack(_ archive: URL, named name: String,
                        then done: @escaping (URL?) -> Void) {
        let dest = Storage.extracted.appendingPathComponent(name, isDirectory: true)
        if FileManager.default.fileExists(atPath: dest.path) { done(dest); return }

        notice = "Unpacking \(name)…"
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                try Zip.extract(archive, to: dest)
            } catch {
                // A half-unpacked folder would be offered again and fail in a
                // more confusing way the second time.
                try? FileManager.default.removeItem(at: dest)
                DispatchQueue.main.async {
                    self?.notice = error.localizedDescription
                    done(nil)
                }
                return
            }
            DispatchQueue.main.async {
                self?.notice = nil
                done(dest)
            }
        }
    }

    /// Unpack an archive for a session that has not started yet, so the wizard
    /// has a real folder to mount as C:.
    func prepareArchive(_ archive: URL, named name: String,
                        then done: @escaping (URL?) -> Void) {
        unpack(archive, named: name) { folder in
            done(folder.map { Self.mountRoot(of: $0) })
        }
    }

    /// Follow a folder down through wrappers.
    ///
    /// An archive of a DOS game very often holds one folder with the game
    /// inside it, sometimes two deep. Mounting the wrapper puts the user at a
    /// prompt with nothing in sight, so the wrapper is stepped through -- but
    /// only while there is exactly one directory and nothing else, which is
    /// what makes it a wrapper rather than a choice.
    static func mountRoot(of dir: URL, depth: Int = 0) -> URL {
        guard depth < 3 else { return dir }
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]) else { return dir }

        let visible = entries.filter { $0.lastPathComponent != "__MACOSX" }
        guard visible.count == 1,
              let only = visible.first,
              (try? only.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
        else { return dir }
        return mountRoot(of: only, depth: depth + 1)
    }

    // MARK: - Settings

    /// The DOS machine the wizard edits, persisted whole. Stored as plain
    /// key/value in UserDefaults rather than as an archive, so a setting that
    /// is added later reads as its default instead of failing to decode.
    static func loadMachine() -> DDMachine {
        var m = DDMachine()
        dd_machine_defaults(&m)
        let d = UserDefaults.standard
        guard d.object(forKey: "machine.saved") != nil else { return m }

        m.cycles_max    = Int32(d.integer(forKey: "machine.cycles_max"))
        m.cycles_fixed  = Int32(d.integer(forKey: "machine.cycles_fixed"))
        m.memsize       = Int32(d.integer(forKey: "machine.memsize"))
        m.vmemsize      = Int32(d.integer(forKey: "machine.vmemsize"))
        m.fpu           = Int32(d.integer(forKey: "machine.fpu"))
        m.ems           = Int32(d.integer(forKey: "machine.ems"))
        m.umb           = Int32(d.integer(forKey: "machine.umb"))
        m.pcspeaker     = Int32(d.integer(forKey: "machine.pcspeaker"))
        m.aspect_mode   = Int32(d.integer(forKey: "machine.aspect_mode"))
        m.integer_scale = Int32(d.integer(forKey: "machine.integer_scale"))
        m.joystick      = Int32(d.integer(forKey: "machine.joystick"))
        CField.set(d.string(forKey: "machine.machine") ?? "svga_s3", into: &m.machine)
        CField.set(d.string(forKey: "machine.cputype") ?? "auto", into: &m.cputype)
        CField.set(d.string(forKey: "machine.sbtype") ?? "sbpro2", into: &m.sbtype)
        CField.set(d.string(forKey: "machine.dos_ver") ?? "", into: &m.dos_ver)
        return m
    }

    static func save(machine m: DDMachine) {
        let d = UserDefaults.standard
        d.set(true, forKey: "machine.saved")
        d.set(Int(m.cycles_max), forKey: "machine.cycles_max")
        d.set(Int(m.cycles_fixed), forKey: "machine.cycles_fixed")
        d.set(Int(m.memsize), forKey: "machine.memsize")
        d.set(Int(m.vmemsize), forKey: "machine.vmemsize")
        d.set(Int(m.fpu), forKey: "machine.fpu")
        d.set(Int(m.ems), forKey: "machine.ems")
        d.set(Int(m.umb), forKey: "machine.umb")
        d.set(Int(m.pcspeaker), forKey: "machine.pcspeaker")
        d.set(Int(m.aspect_mode), forKey: "machine.aspect_mode")
        d.set(Int(m.integer_scale), forKey: "machine.integer_scale")
        d.set(Int(m.joystick), forKey: "machine.joystick")
        d.set(CField.get(m.machine), forKey: "machine.machine")
        d.set(CField.get(m.cputype), forKey: "machine.cputype")
        d.set(CField.get(m.sbtype), forKey: "machine.sbtype")
        d.set(CField.get(m.dos_ver), forKey: "machine.dos_ver")
    }
}
