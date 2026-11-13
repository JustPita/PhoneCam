package fr.justpita.phonecam

import android.util.Log
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * Flux réseau pour OBS : MPEG-TS sur HTTP (`http://IP:38510/`), à mettre dans une « Source média »
 * OBS (décocher « Fichier local »). Le flux H.264/HEVC du téléphone est seulement multiplexé, pas
 * réencodé. Plusieurs lecteurs peuvent se connecter ; chacun commence à une image clé.
 */
class ObsServer(private val listener: Listener) {

    interface Listener {
        /** Nombre de lecteurs changé (0 → 1 : il faut une image clé, et démarrer la caméra si besoin). */
        fun onObsClients(count: Int)
    }

    private class Client(val socket: Socket) {
        val queue = LinkedBlockingQueue<ByteArray>()
        @Volatile var synced = false
        @Volatile var queuedBytes = 0L
    }

    private val clients = CopyOnWriteArrayList<Client>()
    private var server: ServerSocket? = null
    @Volatile private var running = false
    private val muxer = TsMuxer()
    val clientCount: Int get() = clients.size

    fun start() {
        if (running) return
        running = true
        Thread({
            try {
                val ss = ServerSocket()
                ss.reuseAddress = true
                ss.bind(InetSocketAddress(PORT))
                server = ss
                while (running) {
                    val s = ss.accept()
                    Thread({ serve(s) }, "obs-client").start()
                }
            } catch (e: IOException) {
                if (running) Log.e(TAG, "accept", e)
            }
        }, "obs-accept").start()
    }

    fun stop() {
        running = false
        try { server?.close() } catch (_: IOException) {}
        server = null
        for (c in clients) try { c.socket.close() } catch (_: IOException) {}
        clients.clear()
        listener.onObsClients(0)
    }

    /** Codec du flux (« avc » ou « hevc ») ; un changement de codec coupe les lecteurs, OBS se reconnecte. */
    fun setCodec(codec: String) {
        if (muxer.setCodec(codec)) {
            for (c in clients) try { c.socket.close() } catch (_: IOException) {}
        }
    }

    fun pushFrame(data: ByteArray, ptsUs: Long, key: Boolean) {
        if (clients.isEmpty()) return
        val ts = muxer.mux(data, ptsUs, key)
        for (c in clients) {
            if (!c.synced) {
                if (!key) continue
                c.synced = true
            }
            // Lecteur trop lent : on le resynchronise sur la prochaine image clé plutôt que d'accumuler du retard.
            if (c.queuedBytes > MAX_QUEUED_BYTES) {
                c.queue.clear()
                c.queuedBytes = 0
                c.synced = false
                continue
            }
            c.queuedBytes += ts.size
            c.queue.offer(ts)
        }
    }

    private fun serve(s: Socket) {
        val c = Client(s)
        try {
            s.tcpNoDelay = true
            s.soTimeout = 5000
            // Lecture de la requête HTTP (on ignore le chemin : tout renvoie le flux).
            val input = s.getInputStream()
            val head = StringBuilder()
            while (!head.endsWith("\r\n\r\n") && head.length < 8192) {
                val b = input.read()
                if (b < 0) return
                head.append(b.toChar())
            }
            s.soTimeout = 0
            val out: OutputStream = s.getOutputStream()
            out.write(
                ("HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-cache\r\n" +
                    "Connection: close\r\n\r\n").toByteArray()
            )
            out.flush()
            clients += c
            listener.onObsClients(clients.size)
            while (running && !s.isClosed) {
                val chunk = c.queue.poll(1, TimeUnit.SECONDS) ?: continue
                c.queuedBytes -= chunk.size
                out.write(chunk)
                if (c.queue.isEmpty()) out.flush()
            }
        } catch (e: IOException) {
            // lecteur parti
        } catch (e: InterruptedException) {
            // arrêt
        } finally {
            clients -= c
            try { s.close() } catch (_: IOException) {}
            if (running) listener.onObsClients(clients.size)
        }
    }

    companion object {
        const val PORT = 38510
        private const val MAX_QUEUED_BYTES = 16L * 1024 * 1024
        private const val TAG = "PhoneCamObs"
    }
}

/** Multiplexeur MPEG-TS minimal : un programme, une piste vidéo H.264 ou HEVC, PTS à 90 kHz. */
class TsMuxer {
    private var streamType = 0x1B // H.264 ; 0x24 = HEVC
    private val cc = IntArray(0x2000)
    private var version = 0

    /** Renvoie true si le codec a changé. */
    fun setCodec(codec: String): Boolean {
        val t = if (codec == "hevc") 0x24 else 0x1B
        if (t == streamType) return false
        streamType = t
        version = (version + 1) and 0x1F
        return true
    }

    fun mux(frame: ByteArray, ptsUs: Long, key: Boolean): ByteArray {
        val out = java.io.ByteArrayOutputStream(frame.size + frame.size / 150 * 4 + 1024)
        if (key) {
            // PAT et PMT devant chaque image clé : un lecteur qui arrive peut démarrer là.
            writeSection(out, PID_PAT, pat())
            writeSection(out, PID_PMT, pmt())
        }
        val pcr90 = ptsUs * 9 / 100           // horloge 90 kHz
        val pts90 = pcr90 + 9000               // 100 ms d'avance sur le PCR
        val pes = java.io.ByteArrayOutputStream(frame.size + 32)
        pes.write(byteArrayOf(0, 0, 1, 0xE0.toByte(), 0, 0, 0x80.toByte(), 0x80.toByte(), 5))
        pes.write(ptsBytes(pts90))
        pes.write(frame)
        writePes(out, pes.toByteArray(), pcr90, key)
        return out.toByteArray()
    }

    private fun ptsBytes(pts: Long): ByteArray {
        val p = pts and 0x1FFFFFFFFL
        return byteArrayOf(
            (0x21 or ((p shr 29).toInt() and 0x0E)).toByte(),
            (p shr 22).toByte(),
            ((p shr 14).toInt() or 1).toByte(),
            (p shr 7).toByte(),
            ((p shl 1).toInt() or 1).toByte(),
        )
    }

    private fun writePes(out: java.io.ByteArrayOutputStream, pes: ByteArray, pcr90: Long, key: Boolean) {
        var pos = 0
        var first = true
        while (pos < pes.size) {
            val pkt = ByteArray(188)
            pkt[0] = 0x47
            pkt[1] = ((if (first) 0x40 else 0) or (PID_VIDEO shr 8)).toByte()
            pkt[2] = PID_VIDEO.toByte()
            var hdr = 4
            val remaining = pes.size - pos
            // Champ d'adaptation : PCR (+ accès aléatoire) sur le 1er paquet, bourrage sur le dernier.
            val needPcr = first
            val adaptMin = if (needPcr) 8 else 0
            val space = 184 - adaptMin
            val payload = minOf(remaining, space)
            val adaptLen = 184 - payload // octets d'adaptation (champ longueur compris)
            if (adaptLen > 0) {
                pkt[3] = (0x30 or (cc[PID_VIDEO] and 0x0F)).toByte()
                pkt[4] = (adaptLen - 1).toByte()
                if (adaptLen > 1) {
                    var flags = 0
                    if (needPcr) flags = flags or 0x10
                    if (needPcr && key) flags = flags or 0x40
                    pkt[5] = flags.toByte()
                    var i = 6
                    if (needPcr) {
                        val base = pcr90 and 0x1FFFFFFFFL
                        pkt[6] = (base shr 25).toByte()
                        pkt[7] = (base shr 17).toByte()
                        pkt[8] = (base shr 9).toByte()
                        pkt[9] = (base shr 1).toByte()
                        pkt[10] = (((base and 1) shl 7) or 0x7E).toByte()
                        pkt[11] = 0
                        i = 12
                    }
                    while (i < 4 + adaptLen) pkt[i++] = 0xFF.toByte()
                }
                hdr = 4 + adaptLen
            } else {
                pkt[3] = (0x10 or (cc[PID_VIDEO] and 0x0F)).toByte()
            }
            cc[PID_VIDEO]++
            System.arraycopy(pes, pos, pkt, hdr, payload)
            pos += payload
            first = false
            out.write(pkt)
        }
    }

    private fun writeSection(out: java.io.ByteArrayOutputStream, pid: Int, section: ByteArray) {
        val pkt = ByteArray(188) { 0xFF.toByte() }
        pkt[0] = 0x47
        pkt[1] = (0x40 or (pid shr 8)).toByte()
        pkt[2] = pid.toByte()
        pkt[3] = (0x10 or (cc[pid] and 0x0F)).toByte()
        cc[pid]++
        pkt[4] = 0 // pointer_field
        System.arraycopy(section, 0, pkt, 5, section.size)
        out.write(pkt)
    }

    private fun pat(): ByteArray = withCrc(
        byteArrayOf(
            0x00, (0xB0).toByte(), 13, 0x00, 0x01, (0xC1 or (version shl 1)).toByte(), 0, 0,
            0x00, 0x01, (0xE0 or (PID_PMT shr 8)).toByte(), PID_PMT.toByte(),
        )
    )

    private fun pmt(): ByteArray = withCrc(
        byteArrayOf(
            0x02, (0xB0).toByte(), 18, 0x00, 0x01, (0xC1 or (version shl 1)).toByte(), 0, 0,
            (0xE0 or (PID_VIDEO shr 8)).toByte(), PID_VIDEO.toByte(), 0xF0.toByte(), 0,
            streamType.toByte(), (0xE0 or (PID_VIDEO shr 8)).toByte(), PID_VIDEO.toByte(), 0xF0.toByte(), 0,
        )
    )

    private fun withCrc(b: ByteArray): ByteArray {
        var crc = -1
        for (x in b) {
            crc = crc xor ((x.toInt() and 0xFF) shl 24)
            repeat(8) { crc = if (crc and 0x80000000.toInt() != 0) (crc shl 1) xor 0x04C11DB7 else crc shl 1 }
        }
        return b + byteArrayOf((crc ushr 24).toByte(), (crc ushr 16).toByte(), (crc ushr 8).toByte(), crc.toByte())
    }

    companion object {
        const val PID_PAT = 0x0000
        const val PID_PMT = 0x1000
        const val PID_VIDEO = 0x0100
    }
}
