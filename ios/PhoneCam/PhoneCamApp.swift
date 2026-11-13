import AVFoundation
import SwiftUI
import UIKit

@main
struct PhoneCamApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .preferredColorScheme(.dark)
                .onAppear { model.start() }
        }
    }
}

/// État de l'application, rafraîchi pour l'affichage.
final class AppModel: ObservableObject {
    let engine = CameraEngine()
    lazy var server = PhoneServer(engine: engine)

    @Published var cameraAllowed = true
    @Published var status = "En attente du PC"
    @Published var details = ""
    @Published var streaming = false
    @Published var connected = false
    @Published var obsOn = UserDefaults.standard.bool(forKey: "obsEnabled")
    @Published var front = false

    private var timer: Timer?
    private var lastBytes: Int64 = 0
    private var lastFrames: Int64 = 0
    private var lastTime = Date()

    func start() {
        AVCaptureDevice.requestAccess(for: .video) { ok in
            DispatchQueue.main.async {
                self.cameraAllowed = ok
                guard ok else { return }
                self.server.onStateChanged = { [weak self] in self?.refresh() }
                self.server.start()
                // Flux OBS : seulement s'il avait été activé la dernière fois.
                if self.obsOn { self.server.setObsEnabled(true) }
                self.timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
                self.refresh()
            }
        }
    }

    func toggleObs() {
        obsOn.toggle()
        UserDefaults.standard.set(obsOn, forKey: "obsEnabled")
        server.setObsEnabled(obsOn)
        refresh()
    }

    func switchCamera() { server.switchCamera() }

    var hasSeveralCameras: Bool { engine.cameras.count > 1 }

    /// IPv4 Wi-Fi du téléphone.
    var ipAddress: String? {
        var result: String?
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return nil }
        defer { freeifaddrs(ifaddr) }
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let i = ptr.pointee
            guard let addr = i.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
                  String(cString: i.ifa_name) == "en0" else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
            result = String(cString: host)
        }
        return result
    }

    var obsUrl: String { ipAddress.map { "http://\($0):\(ObsServer.port)/" } ?? "pas de réseau Wi-Fi" }

    func refresh() {
        let s = server
        streaming = s.cameraOn
        connected = s.clientName != nil
        front = engine.cameras.first { $0.id == engine.lastConfig?.cameraId }?.facing == "front"
        UIApplication.shared.isIdleTimerDisabled = streaming
        if streaming {
            status = connected ? "En direct vers le PC · Wi-Fi" : "En direct pour OBS"
            var d = ""
            if let c = engine.lastConfig {
                d = "\(c.width)×\(c.height) @ \(c.fps) i/s \(c.codec == "hevc" ? "HEVC" : "H.264")"
            }
            let now = Date()
            let dt = now.timeIntervalSince(lastTime)
            if dt > 0.5 {
                let mbps = Double(s.bytesSent - lastBytes) * 8 / dt / 1e6
                let fps = Double(s.framesSent - lastFrames) / dt
                d += String(format: "  ·  %.1f Mb/s  ·  %.0f i/s", mbps, fps)
                lastBytes = s.bytesSent
                lastFrames = s.framesSent
                lastTime = now
            }
            if obsOn {
                let n = s.obs.clientCount
                d += "\nOBS : \(obsUrl)" + (n > 0 ? "  ·  \(n) lecteur\(n > 1 ? "s" : "")" : "")
            }
            details = d
        } else {
            status = connected ? "Connecté au PC" : "En attente du PC"
            details = ""
        }
    }
}
