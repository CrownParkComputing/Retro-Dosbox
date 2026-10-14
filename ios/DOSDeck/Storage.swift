//
//  Storage.swift
//
//  Where the user's software lives, and how the app finds it.
//
//  Three folders under Documents, which Info.plist publishes to the Files app
//  (UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace). That is the
//  whole acquisition story: the app has no catalogue, no store, no downloads
//  and no links to obtain software, and it never has. It runs files the user
//  already has and put there themselves.
//
//  Each folder gets a README on first launch. It is not decoration -- on a
//  fresh install every list in the app is legitimately empty, and an empty
//  list with no explanation is how an emulator comes to look broken to
//  somebody who has never used one.
//

import Foundation

enum Storage {

    // MARK: - Layout

    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    /// Game folders and .zip archives.
    static var dos: URL { documents.appendingPathComponent("DOS", isDirectory: true) }

    /// CD and floppy images: the Windows disc, a game's CD, a FreeDOS ISO.
    static var discs: URL { documents.appendingPathComponent("Discs", isDirectory: true) }

    /// The Windows machine -- its disk image and its state. One machine, not a
    /// library of them: a Windows 98 disk image is several gigabytes and a
    /// phone holds one of those, not a collection.
    static var windows: URL { documents.appendingPathComponent("Windows", isDirectory: true) }

    /// Generated confs and anything else the app writes for itself. Out of
    /// Documents on purpose: it is not the user's and should not clutter the
    /// folder they browse.
    static var work: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask)[0]
        return base.appendingPathComponent("DOSDeck", isDirectory: true)
    }

    /// Where a .zip is unpacked before it is mounted. DOSBox-X mounts a
    /// DIRECTORY by path; it cannot read out of an archive, so an archive has
    /// to become a folder somewhere first.
    ///
    /// Application Support rather than Caches, deliberately: iOS may delete
    /// Caches at any moment, and doing so while the drive is mounted would
    /// pull the disk out from under a running game.
    static var extracted: URL { work.appendingPathComponent("Extracted", isDirectory: true) }

    // MARK: - First run

    static func prepare() {
        let fm = FileManager.default
        for url in [dos, discs, windows, work, extracted] {
            try? fm.createDirectory(at: url, withIntermediateDirectories: true)
        }
        installBundledContent()
        // Not backed up: these are large, reproducible, and iCloud has no
        // business carrying an unpacked copy of something already on the
        // device as a .zip.
        exclude(from: work)

        write(readme: """
        DOSDeck — DOS software
        ======================

        Put your own DOS games and programs in this folder.

        A FOLDER of DOS files appears in DOSDeck as something to run. So does a
        .ZIP of one; DOSDeck unpacks it the first time you run it.

        DOSDeck comes with no software of its own beyond a small demonstration
        program and a FreeDOS boot floppy, and it cannot download any. What you
        run here is what you put here.
        """, into: dos)

        write(readme: """
        DOSDeck — discs
        ===============

        CD images go here: .ISO, .CUE with its .BIN, .IMG or .CHD.

        This is also where DOSDeck looks for your Windows 98 installation CD.
        You supply that disc yourself. DOSDeck does not include Windows, cannot
        obtain it, and has nowhere to obtain it from.

        While Windows is running, DOSDeck lists everything in this folder so you
        can put a disc in the drive or change it, the way you would on a real
        machine.
        """, into: discs)

        write(readme: """
        DOSDeck — the Windows machine
        =============================

        DOSDeck keeps one Windows machine, and this is its folder.

        HDD.IMG is its hard disk: DOSDeck makes it, Windows Setup formats and
        fills it, and deleting it starts the installation over. The other files
        here are DOSDeck's note of how far the installation has got.

        Nothing in this folder is supplied by DOSDeck except the empty disk.
        """, into: windows)
    }

    private static func write(readme: String, into dir: URL) {
        let url = dir.appendingPathComponent("README.txt")
        guard !FileManager.default.fileExists(atPath: url.path) else { return }
        try? readme.write(to: url, atomically: true, encoding: .utf8)
    }

    private static func exclude(from url: URL) {
        var u = url
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? u.setResourceValues(values)
    }

    // MARK: - Reading the folders

    /// One thing the user can run.
    struct Title: Identifiable, Hashable {
        enum Kind { case folder, archive, prompt }

        let id: String
        let name: String
        let kind: Kind
        /// The folder to mount, or the archive to unpack. Empty for the two
        /// entries that are not files at all.
        let url: URL?
        /// Size on disk, for the subtitle. 0 when it was not worth counting.
        let bytes: Int64

        var icon: String {
            switch kind {
            case .folder:  return "folder"
            case .archive: return "doc.zipper"
            case .prompt:  return "chevron.right"
            }
        }
    }

    struct Disc: Identifiable, Hashable {
        let id: String
        let name: String
        let url: URL
        let bytes: Int64
    }

    private static let discExtensions: Set<String> = ["iso", "cue", "bin", "img", "chd"]

    /// Everything in the DOS folder that can be run, newest arrangement first:
    /// folders and archives, sorted by name, with the two bundled entries and
    /// the bare prompt offered separately by the wizard.
    static func titles() -> [Title] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: dos, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey],
            options: [.skipsHiddenFiles]) else { return [] }

        var out: [Title] = []
        for url in entries {
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
            if isDir {
                out.append(Title(id: url.path, name: url.lastPathComponent,
                                 kind: .folder, url: url, bytes: 0))
            } else if url.pathExtension.lowercased() == "zip" {
                let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
                out.append(Title(id: url.path,
                                 name: url.deletingPathExtension().lastPathComponent,
                                 kind: .archive, url: url, bytes: Int64(size)))
            }
        }
        // A folder and a .zip of the same game are one game, and the unpacked
        // folder is the one that can actually run. This is the same rule the
        // other builds use, for the same reason.
        let folders = Set(out.filter { $0.kind == .folder }.map { $0.name.lowercased() })
        out.removeAll { $0.kind == .archive && folders.contains($0.name.lowercased()) }

        return out.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    static func discImages(in dir: URL? = nil) -> [Disc] {
        let fm = FileManager.default
        let root = dir ?? discs
        guard let entries = try? fm.contentsOfDirectory(
            at: root, includingPropertiesForKeys: [.fileSizeKey],
            options: [.skipsHiddenFiles]) else { return [] }

        return entries
            .filter { discExtensions.contains($0.pathExtension.lowercased()) }
            // A .cue and its .bin are one disc; the .cue is the one to mount.
            .filter { url in
                guard url.pathExtension.lowercased() == "bin" else { return true }
                let cue = url.deletingPathExtension().appendingPathExtension("cue")
                return !fm.fileExists(atPath: cue.path)
            }
            .map { url in
                let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
                return Disc(id: url.path, name: url.lastPathComponent,
                            url: url, bytes: Int64(size))
            }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    // MARK: - What to run inside a folder

    /// Programs that are a last resort rather than a first choice. Some titles
    /// ship nothing else, so they are offered -- but only once everything that
    /// looks like the game itself has been ruled out.
    private static let supportNames = ["install", "setup", "uninstall", "network",
                                       "config", "readme", "modem", "serial"]

    /// The program to run in a mounted folder, or nil when there is none.
    ///
    /// .BAT first, and that order matters: installers habitually leave a
    /// one-line batch file that sets up the environment the .EXE expects, and
    /// running the .EXE directly then fails in ways that read as emulation
    /// bugs. Only the top level is considered -- the name goes into a DOS
    /// command line, where a nested path would simply not run.
    static func runnable(in dir: URL) -> String? {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return nil }

        func isSupport(_ n: String) -> Bool {
            let low = n.lowercased()
            return supportNames.contains { low.contains($0) }
        }

        var bat: String?, com: String?, exe: String?
        var sBat: String?, sCom: String?, sExe: String?

        for name in names.sorted(by: { $0.localizedCaseInsensitiveCompare($1) == .orderedAscending }) {
            var isDir: ObjCBool = false
            fm.fileExists(atPath: dir.appendingPathComponent(name).path, isDirectory: &isDir)
            if isDir.boolValue { continue }

            let ext = (name as NSString).pathExtension.lowercased()
            let support = isSupport(name)
            switch ext {
            case "bat":
                if support { sBat = sBat ?? name }
                else {
                    // RUN.BAT / START.BAT / PLAY.BAT beat an alphabetically
                    // earlier helper that happens to sort first.
                    let low = name.lowercased()
                    let preferred = low.contains("run") || low.contains("start") || low.contains("play")
                    if bat == nil || preferred { bat = name }
                }
            case "com": if support { sCom = sCom ?? name } else { com = com ?? name }
            case "exe": if support { sExe = sExe ?? name } else { exe = exe ?? name }
            default: break
            }
        }
        return bat ?? com ?? exe ?? sBat ?? sCom ?? sExe
    }

    // MARK: - Bundled content
    //
    // Two items ship inside the app: a 354-byte demonstration program written
    // for this project, and a FreeDOS 1.3 boot floppy image (GNU GPL v2,
    // freedos.org). Neither is offered as a download, because neither is a
    // download -- they are in the bundle, and on first launch they are copied
    // into the user's own folders where they become ordinary content.
    //
    // Copied rather than listed as special cases, for two reasons. DOSBox-X
    // mounts a directory BY PATH and cannot read out of an app bundle, so a
    // copy has to happen somewhere regardless. And once copied they are just a
    // folder and a disc like any other, which means every screen in the app
    // has something real in it on a fresh install -- a reviewer with no DOS
    // software of their own can start the app and run something immediately.
    //
    // Once, tracked by a flag: somebody who deletes the demo has deleted it on
    // purpose and should not find it back next launch.

    private static let installedKey = "bundledContentInstalled"

    static func installBundledContent() {
        let d = UserDefaults.standard
        guard !d.bool(forKey: installedKey) else { return }
        d.set(true, forKey: installedKey)

        let fm = FileManager.default
        if let src = Bundle.main.url(forResource: "DEMO", withExtension: "COM",
                                     subdirectory: "demo") {
            let folder = dos.appendingPathComponent("Demo", isDirectory: true)
            try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
            try? fm.copyItem(at: src, to: folder.appendingPathComponent("DEMO.COM"))
        }
        if let src = Bundle.main.url(forResource: "FREEDOS", withExtension: "IMG",
                                     subdirectory: "demo") {
            try? fm.copyItem(at: src, to: discs.appendingPathComponent("FREEDOS.IMG"))
        }
    }

    // MARK: - Odds and ends

    static func describe(bytes: Int64) -> String {
        guard bytes > 0 else { return "" }
        let f = ByteCountFormatter()
        f.countStyle = .file
        f.allowedUnits = [.useKB, .useMB, .useGB]
        return f.string(fromByteCount: bytes)
    }
}
