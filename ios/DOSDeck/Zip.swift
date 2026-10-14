//
//  Zip.swift
//
//  Just enough ZIP to unpack a DOS game.
//
//  DOS software arrives as a .zip far more often than as a folder, and
//  DOSBox-X mounts a DIRECTORY by path -- it cannot read out of an archive. So
//  an archive has to become a folder before it can be run, and something has
//  to do that.
//
//  Written here rather than linked, because the alternative is a dependency
//  for two features of one format. Apple's Compression framework provides the
//  one hard part (raw DEFLATE); the rest is the central directory, which is a
//  few fixed-width fields. Entries are decompressed in bounded chunks, so a
//  600 MB archive costs 64 KB of memory rather than 600 MB.
//
//  Deliberately NOT supported: ZIP64, encryption, and compression methods
//  other than stored and deflate. Each is reported as itself rather than
//  producing half a game -- a title that will not start is a bug report, and a
//  title that starts with files missing is a mystery.
//

import Compression
import Foundation

enum ZipError: LocalizedError {
    case notAnArchive
    case zip64
    case encrypted(String)
    case unsupportedMethod(String, UInt16)
    case corrupt(String)

    var errorDescription: String? {
        switch self {
        case .notAnArchive:
            return "That file is not a ZIP archive."
        case .zip64:
            return "That archive is in ZIP64 format, which DOSDeck cannot read. "
                 + "Unpack it on a computer and copy the folder across instead."
        case .encrypted(let name):
            return "\(name) is password-protected, and DOSDeck cannot unpack it."
        case .unsupportedMethod(let name, let method):
            return "\(name) uses compression method \(method), which DOSDeck "
                 + "cannot read. Only stored and deflated entries are supported."
        case .corrupt(let name):
            return "\(name) is damaged and could not be unpacked."
        }
    }
}

enum Zip {

    private struct Entry {
        let name: String
        let method: UInt16
        let flags: UInt16
        let compressedSize: Int
        let uncompressedSize: Int
        let localHeaderOffset: Int
        var isDirectory: Bool { name.hasSuffix("/") }
    }

    /// Unpack `archive` into `destination`, which is created if needed.
    ///
    /// `progress` is called on an arbitrary thread with 0...1.
    static func extract(_ archive: URL, to destination: URL,
                        progress: @escaping (Double) -> Void = { _ in }) throws {
        // Mapped, not read: the archive may be larger than the memory the app
        // is allowed, and the kernel is better at paging it than we are.
        let data = try Data(contentsOf: archive, options: .mappedIfSafe)
        let entries = try centralDirectory(of: data)

        let fm = FileManager.default
        try fm.createDirectory(at: destination, withIntermediateDirectories: true)

        let total = max(1, entries.reduce(0) { $0 + $1.uncompressedSize })
        var done = 0

        for entry in entries {
            // An entry may not escape the destination. A .zip is a file from
            // the internet; "../../Library/Preferences/x" is a real shape of
            // archive and must land nowhere.
            guard let relative = safePath(entry.name) else { continue }
            let out = destination.appendingPathComponent(relative)

            if entry.isDirectory {
                try? fm.createDirectory(at: out, withIntermediateDirectories: true)
                continue
            }
            try? fm.createDirectory(at: out.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)

            // Bit 0 of the general-purpose flags is encryption.
            guard entry.flags & 1 == 0 else { throw ZipError.encrypted(entry.name) }

            try write(entry: entry, from: data, to: out)
            done += entry.uncompressedSize
            progress(min(1, Double(done) / Double(total)))
        }
        progress(1)
    }

    // MARK: - The central directory

    private static func centralDirectory(of data: Data) throws -> [Entry] {
        // The end-of-central-directory record is last, after a comment of up
        // to 65535 bytes, so it is found by scanning backwards for it.
        let eocdSignature: UInt32 = 0x0605_4b50
        let minEOCD = 22
        guard data.count >= minEOCD else { throw ZipError.notAnArchive }

        var eocd = -1
        let searchFrom = max(0, data.count - 65_557)
        var i = data.count - minEOCD
        while i >= searchFrom {
            if read32(data, i) == eocdSignature { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw ZipError.notAnArchive }

        let count  = Int(read16(data, eocd + 10))
        let offset = Int(read32(data, eocd + 16))
        // 0xFFFF / 0xFFFFFFFF here are ZIP64's "look elsewhere" markers.
        guard count != 0xFFFF, offset != 0xFFFF_FFFF else { throw ZipError.zip64 }
        guard offset >= 0, offset < data.count else { throw ZipError.notAnArchive }

        var entries: [Entry] = []
        var p = offset
        for _ in 0..<count {
            guard p + 46 <= data.count, read32(data, p) == 0x0201_4b50 else { break }
            let flags      = read16(data, p + 8)
            let method     = read16(data, p + 10)
            let compSize   = Int(read32(data, p + 20))
            let uncompSize = Int(read32(data, p + 24))
            let nameLen    = Int(read16(data, p + 28))
            let extraLen   = Int(read16(data, p + 30))
            let commentLen = Int(read16(data, p + 32))
            let localAt    = Int(read32(data, p + 42))

            guard p + 46 + nameLen <= data.count else { break }
            let name = String(decoding: data[(p + 46)..<(p + 46 + nameLen)], as: UTF8.self)

            if compSize == 0xFFFF_FFFF || uncompSize == 0xFFFF_FFFF || localAt == 0xFFFF_FFFF {
                throw ZipError.zip64
            }
            entries.append(Entry(name: name, method: method, flags: flags,
                                 compressedSize: compSize, uncompressedSize: uncompSize,
                                 localHeaderOffset: localAt))
            p += 46 + nameLen + extraLen + commentLen
        }
        return entries
    }

    // MARK: - One entry

    private static func write(entry: Entry, from data: Data, to out: URL) throws {
        // The local header repeats the name and extra fields, and its lengths
        // are the ones that count: some writers put a different extra field in
        // each copy, and trusting the central directory's length lands the
        // read a few bytes into the data.
        let h = entry.localHeaderOffset
        guard h + 30 <= data.count, read32(data, h) == 0x0403_4b50 else {
            throw ZipError.corrupt(entry.name)
        }
        let nameLen  = Int(read16(data, h + 26))
        let extraLen = Int(read16(data, h + 28))
        let start = h + 30 + nameLen + extraLen
        let end = start + entry.compressedSize
        guard start >= 0, end <= data.count else { throw ZipError.corrupt(entry.name) }

        FileManager.default.createFile(atPath: out.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: out) else {
            throw ZipError.corrupt(entry.name)
        }
        defer { try? handle.close() }

        switch entry.method {
        case 0:     // stored
            try handle.write(contentsOf: data[start..<end])
        case 8:     // deflate
            try inflate(data[start..<end], into: handle,
                        expecting: entry.uncompressedSize, name: entry.name)
        default:
            throw ZipError.unsupportedMethod(entry.name, entry.method)
        }
    }

    /// Raw DEFLATE through Apple's Compression framework.
    ///
    /// COMPRESSION_ZLIB is raw deflate here, not a zlib-wrapped stream, which
    /// is exactly what a ZIP entry holds -- the two-byte zlib header a .gz or
    /// an HTTP body would carry is absent.
    private static func inflate(_ input: Data, into handle: FileHandle,
                                expecting: Int, name: String) throws {
        let bufferSize = 64 * 1024
        let dst = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        defer { dst.deallocate() }

        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { stream.deallocate() }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE,
                                      COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw ZipError.corrupt(name)
        }
        defer { compression_stream_destroy(stream) }

        var written = 0
        try input.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else {
                throw ZipError.corrupt(name)
            }
            stream.pointee.src_ptr = base
            stream.pointee.src_size = input.count

            var status = COMPRESSION_STATUS_OK
            repeat {
                stream.pointee.dst_ptr = dst
                stream.pointee.dst_size = bufferSize
                // FINALIZE because the whole compressed entry is already in
                // front of the decoder -- there is no more input coming.
                status = compression_stream_process(stream, Int32(COMPRESSION_STREAM_FINALIZE.rawValue))
                let produced = bufferSize - stream.pointee.dst_size
                if produced > 0 {
                    handle.write(Data(bytes: dst, count: produced))
                    written += produced
                }
                if status == COMPRESSION_STATUS_ERROR { throw ZipError.corrupt(name) }
            } while status == COMPRESSION_STATUS_OK
        }

        // The size check stands in for a CRC: a truncated archive decompresses
        // without complaint right up to the point where it stops, and a game
        // missing its last file is a far more confusing failure than one that
        // refuses to unpack.
        guard written == expecting else { throw ZipError.corrupt(name) }
    }

    // MARK: - Helpers

    /// A relative path that cannot leave the destination, or nil to skip the
    /// entry entirely.
    private static func safePath(_ name: String) -> String? {
        let parts = name.split(separator: "/").map(String.init)
        guard !parts.isEmpty else { return nil }
        for part in parts where part == ".." || part == "." || part.isEmpty {
            return nil
        }
        if name.hasPrefix("/") { return nil }
        return parts.joined(separator: "/")
    }

    private static func read16(_ d: Data, _ at: Int) -> UInt16 {
        guard at + 2 <= d.count else { return 0 }
        return UInt16(d[d.startIndex + at]) | UInt16(d[d.startIndex + at + 1]) << 8
    }

    private static func read32(_ d: Data, _ at: Int) -> UInt32 {
        guard at + 4 <= d.count else { return 0 }
        let b = d.startIndex + at
        return UInt32(d[b]) | UInt32(d[b + 1]) << 8 | UInt32(d[b + 2]) << 16 | UInt32(d[b + 3]) << 24
    }
}
