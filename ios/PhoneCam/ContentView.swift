import AVFoundation
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            if model.streaming {
                // Retour écran : ce que filme la caméra (iOS met lui-même la caméra avant en miroir).
                CameraPreview(session: model.engine.session)
                    .ignoresSafeArea()
            } else {
                waitingCard
            }

            VStack(spacing: 0) {
                topBar
                Spacer()
                if model.streaming { bottomButtons }
            }
        }
    }

    private var topBar: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 10) {
                Circle()
                    .fill(model.streaming ? Color.red : (model.connected ? Color.green : Color.yellow))
                    .frame(width: 10, height: 10)
                Text(model.status).font(.headline).foregroundColor(.white)
                Spacer()
                Button(action: model.toggleObs) {
                    Text(model.obsOn ? "OBS ●" : "OBS")
                        .font(.footnote.bold())
                        .padding(.horizontal, 14)
                        .padding(.vertical, 7)
                        .foregroundColor(model.obsOn ? Color(red: 0.3, green: 0.76, blue: 1) : .white)
                        .background(Capsule().fill(Color.black.opacity(0.4)))
                        .overlay(Capsule().stroke(Color.white.opacity(0.35)))
                }
            }
            if !model.details.isEmpty {
                Text(model.details).font(.caption).foregroundColor(.white.opacity(0.85)).padding(.leading, 20)
            }
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
        .padding(.bottom, 24)
        .background(LinearGradient(colors: [.black.opacity(0.7), .clear], startPoint: .top, endPoint: .bottom))
    }

    private var bottomButtons: some View {
        HStack(spacing: 28) {
            if model.hasSeveralCameras {
                Button(action: model.switchCamera) {
                    Image(systemName: "arrow.triangle.2.circlepath.camera")
                        .font(.system(size: 26))
                        .foregroundColor(.white)
                        .frame(width: 72, height: 72)
                        .background(Circle().fill(Color.black.opacity(0.4)))
                        .overlay(Circle().stroke(Color.white.opacity(0.2)))
                }
            }
        }
        .padding(.bottom, 32)
    }

    private var waitingCard: some View {
        VStack(spacing: 14) {
            Image(systemName: "video.fill")
                .font(.system(size: 34))
                .foregroundColor(.white)
                .frame(width: 72, height: 72)
                .background(RoundedRectangle(cornerRadius: 16).fill(Color(red: 0.12, green: 0.53, blue: 0.9)))
            Text(model.cameraAllowed ? (model.connected ? "Connecté" : "En attente du PC") : "Caméra refusée")
                .font(.title2.bold())
                .foregroundColor(.white)
            Text(model.cameraAllowed
                 ? "Ouvrez PhoneCam sur le PC et indiquez l'adresse ci-dessous (même Wi-Fi).\nGardez l'application ouverte pendant la diffusion."
                 : "Autorisez l'accès à la caméra dans Réglages › PhoneCam.")
                .font(.subheadline)
                .multilineTextAlignment(.center)
                .foregroundColor(Color(white: 0.8))
            if let ip = model.ipAddress {
                Text("IP du téléphone : \(ip)").font(.footnote.monospaced()).foregroundColor(Color(white: 0.6))
            }
            if model.obsOn {
                Text("OBS : \(model.obsUrl)").font(.footnote.monospaced()).foregroundColor(Color(white: 0.6))
            }
        }
        .padding(28)
        .background(RoundedRectangle(cornerRadius: 24).fill(Color(red: 0.12, green: 0.13, blue: 0.15)))
        .padding(24)
    }
}

/// Aperçu de la session de capture (AVCaptureVideoPreviewLayer), sans déformation.
struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let v = PreviewView()
        v.previewLayer.session = session
        v.previewLayer.videoGravity = .resizeAspect
        v.backgroundColor = .black
        return v
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {}
}
