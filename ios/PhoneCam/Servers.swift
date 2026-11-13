import Foundation
import Network
import UIKit

/// Serveur du protocole PhoneCam (port 38500, voir PROTOCOL.md) + annonce Bonjour `_phonecam._tcp`.
/// Un seul PC à la fois. La caméra tourne tant que le PC ou un lecteur OBS regarde.
final class PhoneServer {
    static let port: UInt16 = 38500

    var onStateChanged: (() -> Void)?

    private let engine: CameraEngine
    let obs: ObsServer
    private let queue = DispatchQueue(label: "phonecam.server")
    private var listener: NWListener?
    private var connection: NWConnection?
    private var pendingVideo = 0
    private var waitKey = false

    private(set) var clientName: String?
    private(set) var pcStreaming = false
    private(set) var cameraOn = false
    private(set) var bytesSent: Int64 = 0
    private(set) var framesSent: Int64 = 0

    init(engine: CameraEngine) {
        self.engine = engine
        self.obs = ObsServer()
        engine.onFormat = { [weak self] text in
            guard let self = self else { return }
            self.queue.async {
                if let r = text.range(of: "codec=") {
                    let codec = text[r.upperBound...].split(separator: " ").first.map(String.init) ?? "avc"
                    self.obs.setCodec(codec)
                }
                self.send("F", Data(text.utf8))
            }
        }
        engine.onFrame = { [weak self] data, pts, key in
            self?.queue.async { self?.handleFrame(data, pts: pts, key: key) }
        }
        engine.onError = { [weak self] msg in
            self?.queue.async {
                self?.send("E", Data(msg.utf8))
                self?.cameraOn = false
                self?.notify()
            }
        }
        obs.onClientsChanged = { [weak self] count in
            self?.queue.async { self?.obsClientsChanged(count) }
        }
    }

    func start() {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.port)!) else { return }
        l.service = NWListener.Service(name: UIDevice.current.name, type: "_phonecam._tcp")
        l.newConnectionHandler = { [weak self] c in self?.queue.async { self?.accept(c) } }
        l.start(queue: queue)
        listener = l
    }

    private func notify() {
        DispatchQueue.main.async { self.onStateChanged?() }
    }

    // MARK: - PC

    private func accept(_ c: NWConnection) {
        connection?.cancel()
        connection = c
        pendingVideo = 0
        waitKey = false
        if case let .hostPort(host, _) = c.endpoint { clientName = "\(host)" } else { clientName = "PC" }
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self = self, let c = c else { return }
            switch state {
            case .failed, .cancelled: self.queue.async { self.closed(c) }
            default: break
            }
        }
        c.start(queue: queue)
        send("H", Data(engine.helloText().utf8))
        receiveHeader(c)
        notify()
    }

    private func closed(_ c: NWConnection) {
        guard connection === c else { return }
        connection = nil
        clientName = nil
        pcStopped()
        notify()
    }

    private func receiveHeader(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 5, maximumLength: 5) { [weak self] data, _, _, error in
            guard let self = self else { return }
            guard error == nil, let d = data, d.count == 5 else {
                c.cancel()
                return
            }
            let b = [UInt8](d)
            let len = Int(b[1]) << 24 | Int(b[2]) << 16 | Int(b[3]) << 8 | Int(b[4])
            guard len >= 0, len <= 1 << 20 else {
                c.cancel()
                return
            }
            if len == 0 {
                self.handle(b[0], Data())
                self.receiveHeader(c)
                return
            }
            c.receive(minimumIncompleteLength: len, maximumLength: len) { payload, _, _, error in
                guard error == nil, let p = payload, p.count == len else {
                    c.cancel()
                    return
                }
                self.handle(b[0], p)
                self.receiveHeader(c)
            }
        }
    }

    private func handle(_ type: UInt8, _ payload: Data) {
        switch Character(UnicodeScalar(type)) {
        case "S":
            pcStreaming = true
            cameraOn = true
            pendingVideo = 0
            engine.start(CameraEngine.Config.parse(String(decoding: payload, as: UTF8.self)))
            notify()
        case "K":
            engine.requestKeyFrame()
        case "X":
            pcStopped()
        default:
            break
        }
    }

    private func pcStopped() {
        pcStreaming = false
        if obs.clientCount == 0 && cameraOn {
            engine.stop()
            cameraOn = false
        }
        notify()
    }

    private func send(_ type: Character, _ payload: Data, video: Bool = false) {
        guard let c = connection else { return }
        var msg = Data([type.asciiValue ?? 0])
        var len = UInt32(payload.count).bigEndian
        withUnsafeBytes(of: &len) { msg.append(contentsOf: $0) }
        msg.append(payload)
        if video { pendingVideo += 1 }
        bytesSent += Int64(msg.count)
        c.send(content: msg, completion: .contentProcessed { [weak self] _ in
            guard video, let self = self else { return }
            self.queue.async { self.pendingVideo -= 1 }
        })
    }

    private func handleFrame(_ data: Data, pts: Int64, key: Bool) {
        obs.push(data, ptsUs: pts, key: key)
        guard connection != nil else { return }
        // Réseau trop lent : on saute jusqu'à la prochaine image clé plutôt que d'accumuler du retard.
        if key { waitKey = false }
        if waitKey || (!key && pendingVideo >= 6) {
            if !waitKey {
                waitKey = true
                engine.requestKeyFrame()
            }
            return
        }
        var payload = Data([key ? 1 : 0])
        var p = pts.bigEndian
        withUnsafeBytes(of: &p) { payload.append(contentsOf: $0) }
        payload.append(data)
        framesSent += 1
        send("V", payload, video: true)
    }

    // MARK: - OBS

    func setObsEnabled(_ on: Bool) {
        queue.async {
            if on { self.obs.start() } else { self.obs.stop() }
            self.notify()
        }
    }

    private func obsClientsChanged(_ count: Int) {
        if count > 0 {
            if !cameraOn {
                // OBS seul : dernier réglage, ou 1080p30 H.264.
                engine.start(engine.lastConfig ?? engine.defaultConfig())
                cameraOn = true
            } else {
                engine.requestKeyFrame()
            }
        } else if !pcStreaming && cameraOn {
            engine.stop()
            cameraOn = false
        }
        notify()
    }

    /// Changement de caméra depuis le téléphone : même codec, même taille si possible.
    func switchCamera() {
        queue.async {
            guard let cfg = self.engine.lastConfig, self.engine.cameras.count > 1 else { return }
            let ids = self.engine.cameras.map { $0.id }
            let next = ids[((ids.firstIndex(of: cfg.cameraId) ?? 0) + 1) % ids.count]
            let modes = self.engine.modes.filter { $0.cameraId == next && $0.codec == cfg.codec }
            guard !modes.isEmpty else { return }
            let same = modes.first { $0.width == cfg.width && $0.height == cfg.height }
            let chosen = same ?? modes.filter { $0.width * $0.height <= cfg.width * cfg.height }
                .max { $0.width * $0.height < $1.width * $1.height } ?? modes[0]
            var n = cfg
            n.cameraId = next
            n.width = chosen.width
            n.height = chosen.height
            n.fps = min(cfg.fps, chosen.fps)
            if same == nil { n.bitrate = 0 }
            self.engine.start(n)
        }
    }
}

/// Flux réseau pour OBS : MPEG-TS sur HTTP, port 38510 (même format que l'app Android et l'app PC).
final class ObsServer {
    static let port: UInt16 = 38510

    var onClientsChanged: ((Int) -> Void)?
    private let queue = DispatchQueue(label: "phonecam.obs")
    private var listener: NWListener?
    private var clients: [ObjectIdentifier: Client] = [:]
    private let muxer = TsMuxer()
    private let lock = NSLock()
    private var count = 0

    var clientCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }

    var running: Bool { listener != nil }

    private final class Client {
        let connection: NWConnection
        var ready = false
        var synced = false
        var pending = 0
        init(_ c: NWConnection) { connection = c }
    }

    func start() {
        guard listener == nil else { return }
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.port)!) else { return }
        l.newConnectionHandler = { [weak self] c in self?.queue.async { self?.accept(c) } }
        l.start(queue: queue)
        listener = l
    }

    func stop() {
        queue.sync {
            listener?.cancel()
            listener = nil
            for c in clients.values { c.connection.cancel() }
            clients.removeAll()
            updateCount()
        }
    }

    func setCodec(_ codec: String) {
        queue.async {
            if self.muxer.setCodec(codec) {
                for c in self.clients.values { c.connection.cancel() } // OBS se reconnecte
            }
        }
    }

    func push(_ frame: Data, ptsUs: Int64, key: Bool) {
        queue.async {
            guard self.clients.values.contains(where: { $0.ready }) else { return }
            let ts = self.muxer.mux(frame, ptsUs: ptsUs, key: key)
            for c in self.clients.values where c.ready {
                if !c.synced {
                    if !key { continue }
                    c.synced = true
                }
                if c.pending > 16 * 1024 * 1024 {
                    c.synced = false // trop lent : on repart à la prochaine image clé
                    continue
                }
                c.pending += ts.count
                c.connection.send(content: ts, completion: .contentProcessed { [weak self] _ in
                    self?.queue.async { c.pending -= ts.count }
                })
            }
        }
    }

    private func accept(_ c: NWConnection) {
        let client = Client(c)
        let id = ObjectIdentifier(client)
        clients[id] = client
        c.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled:
                self?.queue.async {
                    self?.clients[id] = nil
                    self?.updateCount()
                }
            default: break
            }
        }
        c.start(queue: queue)
        readRequest(client, id: id, buffer: Data())
    }

    private func readRequest(_ client: Client, id: ObjectIdentifier, buffer: Data) {
        client.connection.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, _, error in
            guard let self = self else { return }
            guard error == nil, let d = data, !d.isEmpty else {
                client.connection.cancel()
                return
            }
            var all = buffer
            all.append(d)
            if all.range(of: Data("\r\n\r\n".utf8)) == nil {
                if all.count > 8192 { client.connection.cancel() } else { self.readRequest(client, id: id, buffer: all) }
                return
            }
            let head = "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"
            client.connection.send(content: Data(head.utf8), completion: .contentProcessed { _ in })
            client.ready = true
            self.updateCount()
        }
    }

    private func updateCount() {
        let n = clients.values.filter { $0.ready }.count
        lock.lock()
        let changed = n != count
        count = n
        lock.unlock()
        if changed { onClientsChanged?(n) }
    }
}

/// Multiplexeur MPEG-TS minimal : un programme, une piste vidéo H.264 ou HEVC, PTS à 90 kHz.
final class TsMuxer {
    private var streamType: UInt8 = 0x1B
    private var version: UInt8 = 0
    private var ccPat: UInt8 = 0, ccPmt: UInt8 = 0, ccVideo: UInt8 = 0
    private let pidPmt = 0x1000, pidVideo = 0x0100

    /// Renvoie true si le codec a changé.
    func setCodec(_ codec: String) -> Bool {
        let t: UInt8 = codec == "hevc" ? 0x24 : 0x1B
        guard t != streamType else { return false }
        streamType = t
        version = (version + 1) & 0x1F
        return true
    }

    func mux(_ frame: Data, ptsUs: Int64, key: Bool) -> Data {
        var out = Data(capacity: frame.count + frame.count / 184 * 4 + 1024)
        if key {
            out.append(section(pid: 0, cc: &ccPat, body: pat()))
            out.append(section(pid: pidPmt, cc: &ccPmt, body: pmt()))
        }
        let pcr90 = UInt64(max(0, ptsUs)) * 9 / 100
        let pts = (pcr90 + 9000) & 0x1FFFFFFFF
        var pes: [UInt8] = [0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5]
        pes.append(UInt8(0x21 | ((pts >> 29) & 0x0E)))
        pes.append(UInt8((pts >> 22) & 0xFF))
        pes.append(UInt8(((pts >> 14) & 0xFF) | 1))
        pes.append(UInt8((pts >> 7) & 0xFF))
        pes.append(UInt8(((pts << 1) & 0xFF) | 1))
        pes.append(contentsOf: frame)

        var pos = 0
        var first = true
        while pos < pes.count {
            var pkt = [UInt8](repeating: 0xFF, count: 188)
            pkt[0] = 0x47
            pkt[1] = UInt8((first ? 0x40 : 0) | (pidVideo >> 8))
            pkt[2] = UInt8(pidVideo & 0xFF)
            let adaptMin = first ? 8 : 0
            let payload = min(pes.count - pos, 184 - adaptMin)
            let adaptLen = 184 - payload
            var hdr = 4
            if adaptLen > 0 {
                pkt[3] = 0x30 | (ccVideo & 0x0F)
                pkt[4] = UInt8(adaptLen - 1)
                if adaptLen > 1 {
                    var flags: UInt8 = 0
                    if first { flags |= 0x10 | (key ? 0x40 : 0) }
                    pkt[5] = flags
                    if first {
                        let base = pcr90 & 0x1FFFFFFFF
                        pkt[6] = UInt8((base >> 25) & 0xFF)
                        pkt[7] = UInt8((base >> 17) & 0xFF)
                        pkt[8] = UInt8((base >> 9) & 0xFF)
                        pkt[9] = UInt8((base >> 1) & 0xFF)
                        pkt[10] = UInt8(((base & 1) << 7) | 0x7E)
                        pkt[11] = 0
                    }
                }
                hdr = 4 + adaptLen
            } else {
                pkt[3] = 0x10 | (ccVideo & 0x0F)
            }
            ccVideo = ccVideo &+ 1
            pkt.replaceSubrange(hdr..<(hdr + payload), with: pes[pos..<(pos + payload)])
            pos += payload
            first = false
            out.append(contentsOf: pkt)
        }
        return out
    }

    private func section(pid: Int, cc: inout UInt8, body: [UInt8]) -> Data {
        var pkt = [UInt8](repeating: 0xFF, count: 188)
        pkt[0] = 0x47
        pkt[1] = UInt8(0x40 | (pid >> 8))
        pkt[2] = UInt8(pid & 0xFF)
        pkt[3] = 0x10 | (cc & 0x0F)
        cc = cc &+ 1
        pkt[4] = 0
        pkt.replaceSubrange(5..<(5 + body.count), with: body)
        return Data(pkt)
    }

    private func pat() -> [UInt8] {
        withCrc([0x00, 0xB0, 13, 0x00, 0x01, 0xC1 | (version << 1), 0, 0,
                 0x00, 0x01, UInt8(0xE0 | (pidPmt >> 8)), UInt8(pidPmt & 0xFF)])
    }

    private func pmt() -> [UInt8] {
        withCrc([0x02, 0xB0, 18, 0x00, 0x01, 0xC1 | (version << 1), 0, 0,
                 UInt8(0xE0 | (pidVideo >> 8)), UInt8(pidVideo & 0xFF), 0xF0, 0,
                 streamType, UInt8(0xE0 | (pidVideo >> 8)), UInt8(pidVideo & 0xFF), 0xF0, 0])
    }

    private func withCrc(_ b: [UInt8]) -> [UInt8] {
        var crc: UInt32 = 0xFFFF_FFFF
        for x in b {
            crc ^= UInt32(x) << 24
            for _ in 0..<8 { crc = (crc & 0x8000_0000) != 0 ? (crc << 1) ^ 0x04C1_1DB7 : crc << 1 }
        }
        return b + [UInt8(crc >> 24), UInt8((crc >> 16) & 0xFF), UInt8((crc >> 8) & 0xFF), UInt8(crc & 0xFF)]
    }
}
