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

    private val streamer = CameraStreamer(ctx, object : CameraStreamer.Sink {
        override fun onFormat(text: String) = enqueue(Msg('F', text.toByteArray()))
        override fun onCodecConfig(data: ByteArray) = enqueue(Msg('C', data))
        override fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) = enqueueVideo(data, ptsUs, keyFrame)
        override fun onError(message: String) {
            enqueue(Msg('E', message.toByteArray()))
            listener.onStreaming(false)
        }
    })

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

    private fun handleClient(s: Socket) {
        streamer.stop()
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
                        listener.onStreaming(true)
                    }
                    'K' -> streamer.requestKeyFrame()
                    'X' -> {
                        streamer.stop()
                        listener.onStreaming(false)
                    }
                }
            }
        } catch (e: IOException) {
            // client parti
        } finally {
            if (client === s) {
                client = null
                streamer.stop()
                listener.onStreaming(false)
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
