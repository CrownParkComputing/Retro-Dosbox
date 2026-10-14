//
//  EmuSurface.swift
//
//  The picture. A Metal view that takes finished frames from the engine's
//  framebuffer tap and fits them to the screen.
//
//  The engine renders OFFSCREEN -- it has no window and presents nothing --
//  so this is the only thing that puts a DOS machine on screen. That
//  arrangement is what makes a native front end possible at all: the app owns
//  every pixel, including the ones around the picture.
//
//  The shaders are compiled at RUN TIME from the string below rather than
//  shipped as a .metal file. They are eleven lines, they will never be edited
//  by anyone tuning a shader, and compiling them here means the build does not
//  have to grow a metallib step -- one less thing to go wrong in a build this
//  project cannot run locally.
//

import Foundation
import Metal
import MetalKit
import SwiftUI

/// Where the picture ended up, in view points. The touch layer needs it: a
/// finger should move the pointer the distance it travelled ON THE GLASS,
/// which means knowing how much the 320x200 picture was scaled to get there.
final class EmuGeometry: ObservableObject {
    @Published var picture: CGRect = .zero
    @Published var frameSize: CGSize = .zero
    @Published var hasPicture = false
}

struct EmuSurface: UIViewRepresentable {
    /// 0 auto, 1 force 4:3, 2 force 16:9, 3 fill the screen.
    var aspectMode: Int32
    var integerScale: Bool
    @ObservedObject var geometry: EmuGeometry

    func makeCoordinator() -> Renderer { Renderer(geometry: geometry) }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.isOpaque = true
        view.preferredFramesPerSecond = 60
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        // The surround is the app's own screen colour, so a 4:3 picture on a
        // widescreen phone sits in DOSDeck rather than in a black void.
        view.clearColor = MTLClearColor(red: 16 / 255, green: 11 / 255, blue: 4 / 255, alpha: 1)
        view.delegate = context.coordinator
        context.coordinator.attach(to: view)
        return view
    }

    func updateUIView(_ view: MTKView, context: Context) {
        context.coordinator.aspectMode = aspectMode
        context.coordinator.integerScale = integerScale
    }

    // MARK: - Renderer

    final class Renderer: NSObject, MTKViewDelegate {
        var aspectMode: Int32 = 0
        var integerScale = false

        private let geometry: EmuGeometry
        private var device: MTLDevice?
        private var queue: MTLCommandQueue?
        private var pipeline: MTLRenderPipelineState?
        private var sampler: MTLSamplerState?
        private var texture: MTLTexture?
        private var vertices: MTLBuffer?

        /// One frame's worth of pixels, reused. A copy rather than a borrowed
        /// pointer because the emulator thread rewrites its buffer in place.
        private var pixels = [UInt32]()
        private var textureSize = CGSize.zero
        private var lastSerial: UInt64 = 0
        private var lastPicture = CGRect.zero

        init(geometry: EmuGeometry) {
            self.geometry = geometry
            super.init()
        }

        func attach(to view: MTKView) {
            guard let device = view.device else { return }
            self.device = device
            queue = device.makeCommandQueue()

            let source = """
            #include <metal_stdlib>
            using namespace metal;
            struct Vertex { float2 pos; float2 uv; };
            struct Fragment { float4 pos [[position]]; float2 uv; };
            vertex Fragment dd_vertex(uint vid [[vertex_id]],
                                      const device Vertex *verts [[buffer(0)]]) {
                Fragment out;
                out.pos = float4(verts[vid].pos, 0.0, 1.0);
                out.uv = verts[vid].uv;
                return out;
            }
            fragment float4 dd_fragment(Fragment in [[stage_in]],
                                        texture2d<float> tex [[texture(0)]],
                                        sampler smp [[sampler(0)]]) {
                return tex.sample(smp, in.uv);
            }
            """
            guard let library = try? device.makeLibrary(source: source, options: nil),
                  let vfn = library.makeFunction(name: "dd_vertex"),
                  let ffn = library.makeFunction(name: "dd_fragment") else { return }

            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = vfn
            descriptor.fragmentFunction = ffn
            descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
            pipeline = try? device.makeRenderPipelineState(descriptor: descriptor)

            // Nearest, not linear. A DOS pixel is a square and should look
            // like one; smoothing 320x200 up to a phone screen is the one
            // thing that makes an emulator look like a video of itself.
            let s = MTLSamplerDescriptor()
            s.minFilter = .nearest
            s.magFilter = .nearest
            s.sAddressMode = .clampToEdge
            s.tAddressMode = .clampToEdge
            sampler = device.makeSamplerState(descriptor: s)

            vertices = device.makeBuffer(length: 4 * 4 * MemoryLayout<Float>.size,
                                         options: .storageModeShared)
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard let device, let queue, let pipeline, let sampler,
                  let drawable = view.currentDrawable,
                  let pass = view.currentRenderPassDescriptor,
                  let buffer = queue.makeCommandBuffer() else { return }

            uploadFrameIfChanged(device: device)

            guard let texture, let vertices, textureSize != .zero else {
                // No picture yet: present the clear colour so the machine
                // appears to be warming up rather than to have failed.
                if let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) {
                    encoder.endEncoding()
                }
                buffer.present(drawable)
                buffer.commit()
                return
            }

            let viewSize = view.drawableSize
            let rect = fit(frame: textureSize, into: viewSize)
            writeQuad(rect, in: viewSize, to: vertices)
            publish(rect, viewSize: viewSize, scale: view.contentScaleFactor)

            if let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) {
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexBuffer(vertices, offset: 0, index: 0)
                encoder.setFragmentTexture(texture, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            buffer.present(drawable)
            buffer.commit()
        }

        // MARK: - Frames

        private func uploadFrameIfChanged(device: MTLDevice) {
            // Poll the serial first: DOS text mode can sit on an identical
            // picture for seconds, and re-uploading it is pure heat.
            let serial = dd_frame_serial()
            guard serial != lastSerial else { return }

            var w: Int32 = 0, h: Int32 = 0
            dd_frame_size(&w, &h)
            guard w > 0, h > 0 else { return }

            let count = Int(w) * Int(h)
            if pixels.count < count { pixels = [UInt32](repeating: 0, count: count) }

            var gotW: Int32 = 0, gotH: Int32 = 0, gotSerial: UInt64 = 0
            let ok = pixels.withUnsafeMutableBufferPointer { out -> Int32 in
                dd_copy_frame(out.baseAddress, Int32(out.count), &gotW, &gotH, &gotSerial)
            }
            guard ok == 1, gotW > 0, gotH > 0 else { return }
            lastSerial = gotSerial

            if texture == nil || Int(gotW) != Int(textureSize.width)
                              || Int(gotH) != Int(textureSize.height) {
                let d = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: .bgra8Unorm, width: Int(gotW), height: Int(gotH),
                    mipmapped: false)
                d.usage = .shaderRead
                texture = device.makeTexture(descriptor: d)
                textureSize = CGSize(width: Int(gotW), height: Int(gotH))
            }

            // 0xXXRRGGBB packed little-endian is B,G,R,X in memory, which is
            // what bgra8Unorm expects; no swizzle is needed.
            pixels.withUnsafeBufferPointer { src in
                guard let base = src.baseAddress else { return }
                texture?.replace(region: MTLRegionMake2D(0, 0, Int(gotW), Int(gotH)),
                                 mipmapLevel: 0, withBytes: base,
                                 bytesPerRow: Int(gotW) * 4)
            }
        }

        // MARK: - Geometry

        /// Where the picture goes, in drawable pixels.
        private func fit(frame: CGSize, into view: CGSize) -> CGRect {
            guard view.width > 0, view.height > 0, frame.width > 0, frame.height > 0 else {
                return .zero
            }
            if aspectMode == 3 { return CGRect(origin: .zero, size: view) }

            // DOS modes are frequently non-square-pixel: 320x200 is a 4:3
            // picture, not 16:10. The engine reports the ratio the current
            // mode should be DISPLAYED at, and ignoring it is the classic
            // emulator tell -- but "auto" is not always what somebody on a
            // widescreen handheld wants, which is why the other modes exist.
            var wanted: CGFloat
            switch aspectMode {
            case 1: wanted = 4.0 / 3.0
            case 2: wanted = 16.0 / 9.0
            default:
                let reported = dd_pixel_aspect_x1000()
                wanted = reported > 0
                    ? frame.width / frame.height * CGFloat(reported) / 1000.0
                    : frame.width / frame.height
            }
            guard wanted > 0 else { return CGRect(origin: .zero, size: view) }

            var w = view.width
            var h = w / wanted
            if h > view.height {
                h = view.height
                w = h * wanted
            }

            if integerScale {
                // A whole number of screen pixels per emulated SCANLINE,
                // which is where uneven scaling actually shows: a 200-line
                // picture stretched to 743 lines has some rows two pixels
                // tall and some three. The width then follows from the
                // display ratio, so the picture is still the right shape --
                // sharp rows matter, and a wrong aspect is a different bug.
                // Never below 1x: a picture scaled to nothing is worse than
                // an imperfect one.
                let factor = max(1, floor(h / frame.height))
                h = frame.height * factor
                w = h * wanted
            }
            return CGRect(x: ((view.width - w) / 2).rounded(),
                          y: ((view.height - h) / 2).rounded(),
                          width: w.rounded(), height: h.rounded())
        }

        /// A triangle strip in clip space: (-1,-1) bottom left to (1,1) top
        /// right, with V flipped because the framebuffer's first row is the
        /// top one and Metal's texture origin is not.
        private func writeQuad(_ rect: CGRect, in view: CGSize, to buffer: MTLBuffer) {
            func ndcX(_ x: CGFloat) -> Float { Float(x / view.width * 2 - 1) }
            func ndcY(_ y: CGFloat) -> Float { Float(1 - y / view.height * 2) }

            let l = ndcX(rect.minX), r = ndcX(rect.maxX)
            let t = ndcY(rect.minY), b = ndcY(rect.maxY)
            let quad: [Float] = [
                l, b, 0, 1,
                l, t, 0, 0,
                r, b, 1, 1,
                r, t, 1, 0,
            ]
            quad.withUnsafeBytes { src in
                guard let base = src.baseAddress else { return }
                memcpy(buffer.contents(), base, src.count)
            }
        }

        /// Tell the touch layer where the picture is, in POINTS, and only when
        /// it moves -- publishing every frame would redraw the overlay sixty
        /// times a second for nothing.
        private func publish(rect: CGRect, viewSize: CGSize, scale: CGFloat) {
            let points = CGRect(x: rect.minX / scale, y: rect.minY / scale,
                                width: rect.width / scale, height: rect.height / scale)
            guard points != lastPicture else { return }
            lastPicture = points
            let size = textureSize
            DispatchQueue.main.async { [geometry] in
                geometry.picture = points
                geometry.frameSize = size
                geometry.hasPicture = true
            }
        }
    }
}
