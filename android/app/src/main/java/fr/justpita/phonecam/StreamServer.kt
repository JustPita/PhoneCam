package fr.justpita.phonecam

import android.content.Context
import android.os.Build
import android.util.Log
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Serveur TCP (un seul client à la fois) + balise UDP de découverte.
 * Voir PROTOCOL.md à la racine du dépôt.
 */
class StreamServer(private val ctx: Context, private val listener: Listener) {

    interface Listener {
        fun onClientChanged(address: String?)
        fun onStreaming(active: Boolean)
    }

    private class Msg(val type: Char, val payload: ByteArray, val video: Boolean = false, val key: Boolean = false, val pts: Long = 0)

    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null
    private var beaconThread: Thread? = null

    @Volatile private var client: Socket? = null
    @Volatile private var queue = LinkedBlockingDeque<Msg>()
    private val queuedVideo = AtomicInteger(0)
    @Volatile private var waitKeyFrame = false

    val bytesSent = AtomicLong(0)
    val framesSent = AtomicLong(0)
    val framesDropped = AtomicLong(0)

    private val streamer: CameraStreamer = CameraStreamer(ctx, object : CameraStreamer.Sink {
        override fun onFormat(text: String) {
            Regex("codec=(\\w+)").find(text)?.let { obs.setCodec(it.groupValues[1]) }
            enqueue(Msg('F', text.toByteArray()))
        }
        override fun onCodecConfig(data: ByteArray) = enqueue(Msg('C', data))
        override fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) {
            obs.pushFrame(data, ptsUs, keyFrame)
            enqueueVideo(data, ptsUs, keyFrame)
        }
        override fun onError(message: String) {
            enqueue(Msg('E', message.toByteArray()))
            cameraOn = false
            listener.onStreaming(false)
        }
    })

    // --- Flux réseau pour OBS ------------------------------------------------------------------
    /** Le PC a demandé la capture (message S) et ne l'a pas arrêtée. */
    @Volatile private var pcStreaming = false
    @Volatile private var cameraOn = false

    private val obs: ObsServer = ObsServer(object : ObsServer.Listener {
        override fun onObsClients(count: Int) {
            if (count > 0) {
                if (!cameraOn) {
                    // OBS seul : on démarre la caméra avec le dernier réglage, ou en 1080p30.
                    streamer.start(streamer.lastConfig ?: defaultConfig())
                    cameraOn = true
                    listener.onStreaming(true)
                } else {
                    streamer.requestKeyFrame() // le nouveau lecteur démarre sur une image clé
                }
            } else if (!pcStreaming && cameraOn) {
                streamer.stop()
                cameraOn = false
                listener.onStreaming(false)
            }
        }
    })

    val obsEnabled: Boolean get() = obsOn
    val obsClients: Int get() = obs.clientCount
    @Volatile private var obsOn = false

    fun setObsEnabled(on: Boolean) {
        if (on == obsOn) return
        obsOn = on
        if (on) obs.start() else obs.stop()
    }

    private fun defaultConfig(): CameraStreamer.Config {
        val back = caps.cams.firstOrNull { it.facing == "back" } ?: caps.cams.firstOrNull()
        val codec = "avc" // le plus compatible avec OBS
        val modes = caps.modes.filter { it.cameraId == back?.id && it.codec == codec && !it.highSpeed }
        val m = modes.firstOrNull { it.width == 1920 && it.height == 1080 }
            ?: modes.filter { it.width * 9 == it.height * 16 && it.height <= 1080 }.maxByOrNull { it.width * it.height }
            ?: modes.maxByOrNull { it.width * it.height }
        return CameraStreamer.Config(back?.id ?: "0", m?.width ?: 1920, m?.height ?: 1080, minOf(30, m?.fps ?: 30), codec, 0)
    }

    val currentFormat: String get() = streamer.current
    val streamInfo: CameraStreamer.Info? get() = streamer.info

    private val caps: Caps.All by lazy { Caps.collect(ctx) }
    private val capsText: String by lazy { Caps.helloText(caps) }

    /** Aperçu à l'écran (null pour le retirer). */
    fun setPreview(texture: android.graphics.SurfaceTexture?) = streamer.setPreview(texture)

    /** Plusieurs caméras ? (pour afficher le bouton de changement) */
    val cameraCount: Int get() = caps.cams.size

    /**
     * Passe à la caméra suivante depuis le téléphone, en gardant le codec et, si possible,
     * la taille et la fréquence. Le PC est prévenu par le message de format habituel.
     */
    fun switchCamera() {
        val cfg = streamer.lastConfig ?: return
        val ids = caps.cams.map { it.id }
        if (ids.size < 2) return
        val next = ids[(ids.indexOf(cfg.cameraId) + 1).mod(ids.size)]
        val modes = caps.modes.filter { it.cameraId == next && it.codec == cfg.codec }
        if (modes.isEmpty()) return
        val same = modes.filter { it.width == cfg.width && it.height == cfg.height }
        val chosen = if (same.isNotEmpty()) {
            same.maxBy { it.fps }
        } else {
            val area = cfg.width.toLong() * cfg.height
            val is169 = { m: Caps.Mode -> m.width * 9 == m.height * 16 }
            modes.filter { it.width.toLong() * it.height <= area }
                .sortedWith(compareBy<Caps.Mode>({ !is169(it) }, { -(it.width.toLong() * it.height) }, { -it.fps }))
                .firstOrNull() ?: modes.minBy { it.width.toLong() * it.height }
        }
        // Fréquence : celle demandée si possible (en normal, toute valeur ≤ max ; en haute vitesse, exacte).
        val fps = when {
            same.any { !it.highSpeed && it.fps >= cfg.fps } -> cfg.fps
            same.any { it.highSpeed && it.fps == cfg.fps } -> cfg.fps
            else -> minOf(cfg.fps, chosen.fps)
        }
        val keepBitrate = chosen.width == cfg.width && chosen.height == cfg.height && fps == cfg.fps
        streamer.start(
            cfg.copy(
                cameraId = next, width = chosen.width, height = chosen.height, fps = fps,
                bitrate = if (keepBitrate) cfg.bitrate else 0,
            )
        )
    }

    fun start() {
        if (running) return
        running = true
        acceptThread = Thread(::acceptLoop, "phonecam-accept").apply { start() }
        beaconThread = Thread(::beaconLoop, "phonecam-beacon").apply { start() }
    }

    fun stop() {
        running = false
        try { serverSocket?.close() } catch (_: IOException) {}
        try { client?.close() } catch (_: IOException) {}
        obs.stop()
        streamer.shutdown()
        acceptThread?.interrupt()
        beaconThread?.interrupt()
    }

    private fun enqueue(m: Msg) {
        if (client != null) queue.offer(m)
    }

    private fun enqueueVideo(data: ByteArray, pts: Long, key: Boolean) {
        if (client == null) return
        // Si le réseau ne suit pas, on jette les images jusqu'à la prochaine image clé
        // plutôt que d'accumuler de la latence.
        if (key) waitKeyFrame = false
        if (waitKeyFrame || (!key && queuedVideo.get() >= MAX_QUEUED_FRAMES)) {
            if (!waitKeyFrame) {
                waitKeyFrame = true
                streamer.requestKeyFrame()
            }
            framesDropped.incrementAndGet()
            return
        }
        queuedVideo.incrementAndGet()
        queue.offer(Msg('V', data, video = true, key = key, pts = pts))
    }

    private fun acceptLoop() {
        try {
            val ss = ServerSocket()
            ss.reuseAddress = true
            ss.bind(InetSocketAddress(PORT))
            serverSocket = ss
            while (running) {
                val s = ss.accept()
                // Nouvelle connexion : elle remplace l'ancienne.
                client?.let { old -> try { old.close() } catch (_: IOException) {} }
                Thread({ handleClient(s) }, "phonecam-client").start()
            }
        } catch (e: IOException) {
            if (running) Log.e(TAG, "accept", e)
        }
    }

    /** Le PC n'en veut plus : on n'arrête la caméra que si OBS ne regarde pas. */
    private fun pcStopped() {
        pcStreaming = false
        if (obs.clientCount == 0) {
            streamer.stop()
            cameraOn = false
            listener.onStreaming(false)
        }
    }

    private fun handleClient(s: Socket) {
        if (obs.clientCount == 0) {
            streamer.stop()
            cameraOn = false
        }
        val q = LinkedBlockingDeque<Msg>()
        queue = q
        queuedVideo.set(0)
        waitKeyFrame = false
        client = s
        val addr = s.inetAddress.hostAddress ?: "?"
        listener.onClientChanged(if (s.inetAddress.isLoopbackAddress) "USB (adb)" else addr)

        s.tcpNoDelay = true
        s.sendBufferSize = 4 * 1024 * 1024
        val out = DataOutputStream(BufferedOutputStream(s.getOutputStream(), 256 * 1024))
        val input = DataInputStream(s.getInputStream())

        val sender = Thread({ senderLoop(s, out, q) }, "phonecam-send").apply { start() }
        q.offer(Msg('H', capsText.toByteArray()))

        try {
            while (running && !s.isClosed) {
                val type = input.readUnsignedByte().toChar()
                val len = input.readInt()
                if (len < 0 || len > 1 shl 20) throw IOException("message trop long")
                val payload = ByteArray(len)
                input.readFully(payload)
                when (type) {
                    'S' -> {
                        q.removeIf { it.video }
                        queuedVideo.set(0)
                        streamer.start(CameraStreamer.Config.parse(String(payload)))
                        pcStreaming = true
                        cameraOn = true
                        listener.onStreaming(true)
                    }
                    'K' -> streamer.requestKeyFrame()
                    'X' -> pcStopped()
                }
            }
        } catch (e: IOException) {
            // client parti
        } finally {
            if (client === s) {
                client = null
                pcStopped()
                listener.onClientChanged(null)
            }
            try { s.close() } catch (_: IOException) {}
            sender.interrupt()
        }
    }

    private fun senderLoop(s: Socket, out: DataOutputStream, q: LinkedBlockingDeque<Msg>) {
        try {
            while (!s.isClosed) {
                val m = q.poll(500, TimeUnit.MILLISECONDS) ?: continue
                if (client !== s) break
                if (m.video) {
                    queuedVideo.decrementAndGet()
                    out.writeByte('V'.code)
                    out.writeInt(m.payload.size + 9)
                    out.writeByte(if (m.key) 1 else 0)
                    out.writeLong(m.pts)
                    out.write(m.payload)
                    framesSent.incrementAndGet()
                } else {
                    out.writeByte(m.type.code)
                    out.writeInt(m.payload.size)
                    out.write(m.payload)
                }
                if (q.isEmpty()) out.flush()
                bytesSent.addAndGet(m.payload.size + 5L)
            }
        } catch (e: IOException) {
            try { s.close() } catch (_: IOException) {}
        } catch (e: InterruptedException) {
            // fin
        }
    }

    private fun beaconLoop() {
        val text = "PHONECAM name=${Build.MANUFACTURER} ${Build.MODEL} port=$PORT".replace('\n', ' ').toByteArray()
        try {
            DatagramSocket().use { sock ->
                sock.broadcast = true
                val target = InetAddress.getByName("255.255.255.255")
                while (running) {
                    try {
                        sock.send(DatagramPacket(text, text.size, target, BEACON_PORT))
                    } catch (e: IOException) {
                        // pas de réseau pour l'instant
                    }
                    Thread.sleep(1000)
                }
            }
        } catch (e: InterruptedException) {
            // arrêt
        } catch (e: IOException) {
            Log.e(TAG, "beacon", e)
        }
    }

    companion object {
        const val PORT = 38500
        const val BEACON_PORT = 38501
        private const val MAX_QUEUED_FRAMES = 6
        private const val TAG = "PhoneCam"
    }
}
