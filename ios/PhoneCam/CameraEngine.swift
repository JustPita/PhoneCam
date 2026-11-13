import AVFoundation
import Foundation
import VideoToolbox

/// Capture AVFoundation -> encodeur matériel VideoToolbox -> images Annex-B (même format que l'app Android).
final class CameraEngine: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {

    struct Config: Equatable {
        var cameraId: String
        var width: Int
        var height: Int
        var fps: Int
        var codec: String // "avc" ou "hevc"
        var bitrate: Int

        static func parse(_ text: String) -> Config {
            var kv: [String: String] = [:]
            for token in text.split(separator: " ") {
                let parts = token.split(separator: "=", maxSplits: 1)
                if parts.count == 2 { kv[String(parts[0])] = String(parts[1]) }
            }
            return Config(
                cameraId: kv["camera"] ?? "0",
                width: Int(kv["width"] ?? "") ?? 1920,
                height: Int(kv["height"] ?? "") ?? 1080,
                fps: Int(kv["fps"] ?? "") ?? 30,
                codec: kv["codec"] ?? "avc",
                bitrate: Int(kv["bitrate"] ?? "") ?? 0
            )
        }
    }

    struct Mode {
        let cameraId: String
        let width: Int
        let height: Int
        let fps: Int
        let codec: String
    }

    struct Camera {
        let id: String
        let device: AVCaptureDevice
        var facing: String { device.position == .front ? "front" : "back" }
    }

    // Sorties
    var onFormat: ((String) -> Void)?
    var onFrame: ((Data, Int64, Bool) -> Void)?
    var onError: ((String) -> Void)?

    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "phonecam.camera")
    private var compression: VTCompressionSession?
    private var forceKeyFrame = false
    private(set) var lastConfig: Config?
    private(set) var running = false
    private var formatSent = false

    // MARK: - Capacités

    lazy var cameras: [Camera] = {
        let types: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera]
        let devices = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified).devices
        // Arrière d'abord, puis avant.
        let sorted = devices.sorted { ($0.position == .back ? 0 : 1) < ($1.position == .back ? 0 : 1) }
        return sorted.enumerated().map { Camera(id: String($0.offset), device: $0.element) }
    }()

    lazy var modes: [Mode] = {
        var result: [Mode] = []
        for cam in cameras {
            var best: [String: Int] = [:] // "LxH" -> fps max
            for format in cam.device.formats {
                let d = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
                guard d.width >= 320, d.width >= d.height else { continue }
                let fps = Int((format.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? 30).rounded(.down))
                let key = "\(d.width)x\(d.height)"
                best[key] = max(best[key] ?? 0, fps)
            }
            for (key, fps) in best {
                let p = key.split(separator: "x")
                guard let w = Int(p[0]), let h = Int(p[1]) else { continue }
                for codec in ["hevc", "avc"] {
                    // H.264 matériel : 4K max ; au-delà, HEVC seulement.
                    if codec == "avc" && w * h > 4096 * 2304 { continue }
                    result.append(Mode(cameraId: cam.id, width: w, height: h, fps: fps, codec: codec))
                }
            }
        }
        return result
    }()

    func helloText() -> String {
        var s = "device Apple \(deviceModel())\n"
        s += "codec hevc\ncodec avc\n"
        for c in cameras { s += "camera \(c.id) \(c.facing) 90\n" }
        for m in modes { s += "mode \(m.cameraId) \(m.width) \(m.height) \(m.fps) 0 \(m.codec)\n" }
        return s
    }

    private func deviceModel() -> String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    /// Réglage par défaut (OBS sans PC) : caméra arrière en 1080p30 H.264.
    func defaultConfig() -> Config {
        let back = cameras.first { $0.facing == "back" } ?? cameras.first
        let id = back?.id ?? "0"
        let has1080 = modes.contains { $0.cameraId == id && $0.width == 1920 && $0.height == 1080 }
        return Config(cameraId: id, width: has1080 ? 1920 : 1280, height: has1080 ? 1080 : 720, fps: 30, codec: "avc", bitrate: 0)
    }

    // MARK: - Démarrage / arrêt

    func start(_ requested: Config) {
        queue.async { self.startOnQueue(requested) }
    }

    func stop() {
        queue.sync { self.stopOnQueue() }
    }

    func requestKeyFrame() {
        queue.async { self.forceKeyFrame = true }
    }

    private func stopOnQueue() {
        if session.isRunning { session.stopRunning() }
        session.beginConfiguration()
        session.inputs.forEach { session.removeInput($0) }
        session.outputs.forEach { session.removeOutput($0) }
        session.commitConfiguration()
        if let c = compression {
            VTCompressionSessionCompleteFrames(c, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(c)
        }
        compression = nil
        running = false
    }

    private func startOnQueue(_ requested: Config) {
        stopOnQueue()
        guard let cam = cameras.first(where: { $0.id == requested.cameraId }) ?? cameras.first else {
            onError?("Aucune caméra")
            return
        }
        let device = cam.device

        // Format : taille demandée, avec la plage de fps qui contient la fréquence voulue.
        let candidates = device.formats.filter {
            let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription)
            return Int(d.width) == requested.width && Int(d.height) == requested.height
        }
        let format = candidates.first { f in
            f.videoSupportedFrameRateRanges.contains { Int($0.maxFrameRate) >= requested.fps && Int($0.minFrameRate) <= requested.fps }
        } ?? candidates.first
        guard let chosen = format else {
            onError?("Taille \(requested.width)×\(requested.height) non disponible")
            return
        }
        let maxFps = Int(chosen.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? 30)
        var cfg = requested
        cfg.cameraId = cam.id
        cfg.fps = min(requested.fps, maxFps)
        if cfg.bitrate <= 0 {
            let bpp = cfg.codec == "hevc" ? 0.06 : 0.1
            cfg.bitrate = max(2_000_000, min(100_000_000, Int(Double(cfg.width * cfg.height * cfg.fps) * bpp)))
        }

        do {
            session.beginConfiguration()
            session.sessionPreset = .inputPriority
            let input = try AVCaptureDeviceInput(device: device)
            guard session.canAddInput(input) else { throw NSError(domain: "PhoneCam", code: 1) }
            session.addInput(input)

            let output = AVCaptureVideoDataOutput()
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: queue)
            guard session.canAddOutput(output) else { throw NSError(domain: "PhoneCam", code: 2) }
            session.addOutput(output)

            try device.lockForConfiguration()
            device.activeFormat = chosen
            let duration = CMTime(value: 1, timescale: CMTimeScale(cfg.fps))
            device.activeVideoMinFrameDuration = duration
            device.activeVideoMaxFrameDuration = duration
            if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
            device.unlockForConfiguration()

            // Image « capteur » en paysage, sans stabilisation (latence), sans miroir.
            if let conn = output.connection(with: .video) {
                if conn.isVideoStabilizationSupported { conn.preferredVideoStabilizationMode = .off }
                if conn.isVideoMirroringSupported {
                    conn.automaticallyAdjustsVideoMirroring = false
                    conn.isVideoMirrored = false
                }
                if #available(iOS 17.0, *) {
                    if conn.isVideoRotationAngleSupported(0) { conn.videoRotationAngle = 0 }
                } else if conn.isVideoOrientationSupported {
                    conn.videoOrientation = .landscapeRight
                }
            }
            session.commitConfiguration()
        } catch {
            session.commitConfiguration()
            onError?("Caméra indisponible : \(error.localizedDescription)")
            return
        }

        guard makeEncoder(cfg) else {
            onError?("Encodeur \(cfg.codec.uppercased()) refusé pour \(cfg.width)×\(cfg.height) @ \(cfg.fps)")
            return
        }
        lastConfig = cfg
        formatSent = false
        running = true
        session.startRunning()
        onFormat?(
            "codec=\(cfg.codec) width=\(cfg.width) height=\(cfg.height) fps=\(cfg.fps) bitrate=\(cfg.bitrate) " +
                "camera=\(cfg.cameraId) facing=\(cam.facing) orientation=90 highspeed=0"
        )
        forceKeyFrame = true
    }

    // MARK: - Encodeur

    private func makeEncoder(_ cfg: Config) -> Bool {
        let codecType = cfg.codec == "hevc" ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
        var spec: [String: Any] = [:]
        if cfg.codec == "avc" { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String] = true }
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault, width: Int32(cfg.width), height: Int32(cfg.height),
            codecType: codecType, encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        guard status == noErr, let s = session else { return false }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: cfg.codec == "hevc" ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: cfg.bitrate as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: cfg.fps as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: (cfg.fps * 2) as CFNumber)
        VTCompressionSessionPrepareToEncodeFrames(s)
        compression = s
        return true
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard running, let s = compression, let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        var props: CFDictionary?
        if forceKeyFrame {
            forceKeyFrame = false
            props = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
        }
        VTCompressionSessionEncodeFrame(s, imageBuffer: image, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: props, infoFlagsOut: nil) { [weak self] status, _, buffer in
            guard status == noErr, let buffer = buffer, let self = self else { return }
            self.emit(buffer)
        }
    }

    /// AVCC (longueurs de 4 octets) -> Annex-B (codes de début), en-têtes devant chaque image clé.
    private func emit(_ buffer: CMSampleBuffer) {
        guard let block = CMSampleBufferGetDataBuffer(buffer) else { return }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, createIfNecessary: false) as? [[CFString: Any]]
        let key = !(attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)
        var out = Data()
        let startCode: [UInt8] = [0, 0, 0, 1]
        if key, let desc = CMSampleBufferGetFormatDescription(buffer) {
            for ps in parameterSets(desc) {
                out.append(contentsOf: startCode)
                out.append(ps)
            }
        }
        var length = 0
        var pointer: UnsafeMutablePointer<CChar>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length,
                                          dataPointerOut: &pointer) == noErr, let base = pointer else { return }
        var offset = 0
        while offset + 4 <= length {
            var nalLen: UInt32 = 0
            memcpy(&nalLen, base + offset, 4)
            nalLen = CFSwapInt32BigToHost(nalLen)
            offset += 4
            guard offset + Int(nalLen) <= length else { break }
            out.append(contentsOf: startCode)
            base.advanced(by: offset).withMemoryRebound(to: UInt8.self, capacity: Int(nalLen)) {
                out.append($0, count: Int(nalLen))
            }
            offset += Int(nalLen)
        }
        let pts = CMSampleBufferGetPresentationTimeStamp(buffer)
        let us = Int64(CMTimeGetSeconds(pts) * 1_000_000)
        onFrame?(out, us, key)
    }

    private func parameterSets(_ desc: CMFormatDescription) -> [Data] {
        var result: [Data] = []
        let hevc = CMFormatDescriptionGetMediaSubType(desc) == kCMVideoCodecType_HEVC
        var count = 0
        if hevc {
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(desc, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                               parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                               nalUnitHeaderLengthOut: nil)
        } else {
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(desc, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                               parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                               nalUnitHeaderLengthOut: nil)
        }
        for i in 0..<count {
            var ptr: UnsafePointer<UInt8>?
            var size = 0
            let st = hevc
                ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(desc, parameterSetIndex: i, parameterSetPointerOut: &ptr,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                     nalUnitHeaderLengthOut: nil)
                : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(desc, parameterSetIndex: i, parameterSetPointerOut: &ptr,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                     nalUnitHeaderLengthOut: nil)
            if st == noErr, let p = ptr { result.append(Data(bytes: p, count: size)) }
        }
        return result
    }
}
