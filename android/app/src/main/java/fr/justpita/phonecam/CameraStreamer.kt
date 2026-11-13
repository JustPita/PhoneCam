package fr.justpita.phonecam

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * Capture Camera2 -> surface d'entrée de l'encodeur matériel -> paquets H.264/HEVC.
 * Toutes les opérations caméra se font sur [camHandler] ; les sorties de l'encodeur sur [encHandler].
 */
class CameraStreamer(private val ctx: Context, private val sink: Sink) {

    interface Sink {
        fun onFormat(text: String)
        fun onCodecConfig(data: ByteArray)
        fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean)
        fun onError(message: String)
    }

    data class Config(
        val cameraId: String,
        val width: Int,
        val height: Int,
        val fps: Int,
        val codec: String,
        val bitrate: Int,
    ) {
        companion object {
            fun parse(text: String): Config {
                val kv = text.trim().split(Regex("\\s+")).mapNotNull {
                    val i = it.indexOf('=')
                    if (i > 0) it.substring(0, i) to it.substring(i + 1) else null
                }.toMap()
                return Config(
                    cameraId = kv["camera"] ?: "0",
                    width = kv["width"]?.toIntOrNull() ?: 1920,
                    height = kv["height"]?.toIntOrNull() ?: 1080,
                    fps = kv["fps"]?.toIntOrNull() ?: 30,
                    codec = kv["codec"] ?: "avc",
                    bitrate = kv["bitrate"]?.toIntOrNull() ?: 0,
                )
            }
        }
    }

    private val camThread = HandlerThread("phonecam-camera").apply { start() }
    private val camHandler = Handler(camThread.looper)
    private val camExecutor = Executor { camHandler.post(it) }
    private val encThread = HandlerThread("phonecam-encoder").apply { start() }
    private val encHandler = Handler(encThread.looper)

    private var generation = 0
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null

    @Volatile
    var current: String = ""
        private set

    /** Dernière configuration réellement appliquée (fps ajusté, débit calculé). */
    @Volatile
    var lastConfig: Config? = null
        private set

    /** Infos pour l'aperçu à l'écran : taille du flux, caméra avant ou non. */
    data class Info(val width: Int, val height: Int, val front: Boolean, val highSpeed: Boolean, val sensorOrientation: Int)

    @Volatile
    var info: Info? = null
        private set

    /** Session en cours : ce qu'il faut pour la reconstruire (ajout/retrait de l'aperçu). */
    private class Active(
        val gen: Int,
        val cam: CameraDevice,
        val encSurface: Surface,
        val highSpeed: Boolean,
        val aeRange: Range<Int>,
        val chars: CameraCharacteristics,
        val cfg: Config,
    ) {
        var formatSent = false
    }

    private var active: Active? = null
    private var previewTexture: SurfaceTexture? = null
    private var previewSurface: Surface? = null

    fun start(cfg: Config) {
        camHandler.post { startOnCamThread(cfg) }
    }

    /**
     * Branche (ou débranche avec null) l'aperçu à l'écran. Si une capture tourne, la session est
     * reconstruite : l'encodeur continue, le PC voit au plus une courte pause.
     * Attend la fin de l'opération pour que l'appelant puisse libérer la texture ensuite.
     */
    fun setPreview(texture: SurfaceTexture?) {
        val done = CountDownLatch(1)
        camHandler.post {
            if (previewTexture !== texture) {
                previewTexture = texture
                active?.let { if (it.gen == generation) createSession(it) }
            }
            done.countDown()
        }
        done.await(2, TimeUnit.SECONDS)
    }

    /** Arrête la capture et attend que tout soit libéré. */
    fun stop() {
        val done = CountDownLatch(1)
        camHandler.post {
            releaseOnCamThread()
            done.countDown()
        }
        done.await(3, TimeUnit.SECONDS)
    }

    fun requestKeyFrame() {
        encHandler.post {
            try {
                encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
            } catch (e: Exception) {
                Log.w(TAG, "request sync frame", e)
            }
        }
    }

    fun shutdown() {
        stop()
        camThread.quitSafely()
        encThread.quitSafely()
    }

    private fun releaseOnCamThread() {
        generation++
        active = null
        info = null
        try { session?.close() } catch (_: Exception) {}
        try { device?.close() } catch (_: Exception) {}
        try { previewSurface?.release() } catch (_: Exception) {}
        previewSurface = null
        try { encoder?.stop() } catch (_: Exception) {}
        try { encoder?.release() } catch (_: Exception) {}
        try { inputSurface?.release() } catch (_: Exception) {}
        session = null
        device = null
        encoder = null
        inputSurface = null
        current = ""
    }

    private fun fail(message: String, e: Throwable? = null) {
        Log.e(TAG, message, e)
        releaseOnCamThread()
        sink.onError(if (e != null) "$message : ${e.message}" else message)
    }

    private fun autoBitrate(cfg: Config): Int {
        // bits par pixel et par image : HEVC est ~40 % plus efficace que H.264.
        val bpp = if (cfg.codec == "hevc") 0.06 else 0.1
        return (cfg.width.toLong() * cfg.height * cfg.fps * bpp).toInt().coerceIn(2_000_000, 100_000_000)
    }

    @SuppressLint("MissingPermission")
    private fun startOnCamThread(requested: Config) {
        releaseOnCamThread()
        val gen = generation
        val cm = ctx.getSystemService(CameraManager::class.java)

        val chars = try {
            cm.getCameraCharacteristics(requested.cameraId)
        } catch (e: Exception) {
            fail("Caméra ${requested.cameraId} introuvable", e)
            return
        }
        val size = Size(requested.width, requested.height)

        // Choix du mode : session normale si elle atteint le fps demandé, sinon haute vitesse.
        val regularMax = Caps.regularMaxFps(chars, size)
        val hsList = Caps.highSpeedFps(chars, size)
        val highSpeed = requested.fps > regularMax && requested.fps in hsList
        val fps = when {
            highSpeed -> requested.fps
            requested.fps <= regularMax -> requested.fps
            else -> regularMax.coerceAtLeast(15)
        }
        val aeRange = if (highSpeed) {
            Range(fps, fps)
        } else {
            val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: emptyArray()
            ranges.filter { it.upper == fps }.maxByOrNull { it.lower }
                ?: ranges.filter { it.upper >= fps }.minByOrNull { it.upper }
                ?: Range(fps, fps)
        }

        val cfg = requested.copy(fps = fps).let {
            if (it.bitrate <= 0) it.copy(bitrate = autoBitrate(it)) else it
        }
        val mime = Caps.CODECS[cfg.codec] ?: MediaFormat.MIMETYPE_VIDEO_AVC
        val info = Caps.findEncoder(mime) ?: run {
            fail("Pas d'encodeur pour ${cfg.codec}")
            return
        }

        // --- Encodeur ---
        val codec = try {
            MediaCodec.createByCodecName(info.name)
        } catch (e: Exception) {
            fail("Impossible de créer l'encodeur ${info.name}", e)
            return
        }
        encoder = codec
        val caps = info.getCapabilitiesForType(mime)
        val format = MediaFormat.createVideoFormat(mime, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, caps.videoCapabilities.bitrateRange.clamp(cfg.bitrate))
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // temps réel
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            if (android.os.Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
            if (caps.encoderCapabilities.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            }
            val wantedProfile = if (cfg.codec == "hevc") {
                MediaCodecInfo.CodecProfileLevel.HEVCProfileMain
            } else {
                MediaCodecInfo.CodecProfileLevel.AVCProfileHigh
            }
            caps.profileLevels.filter { it.profile == wantedProfile }.maxByOrNull { it.level }?.let {
                setInteger(MediaFormat.KEY_PROFILE, it.profile)
                setInteger(MediaFormat.KEY_LEVEL, it.level)
            }
        }

        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(c: MediaCodec, index: Int) {}

            override fun onOutputBufferAvailable(c: MediaCodec, index: Int, bi: MediaCodec.BufferInfo) {
                if (gen != generation) return
                try {
                    val buf = c.getOutputBuffer(index)
                    if (buf != null && bi.size > 0) {
                        buf.position(bi.offset)
                        buf.limit(bi.offset + bi.size)
                        val data = ByteArray(bi.size)
                        buf.get(data)
                        if (bi.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            sink.onCodecConfig(data)
                        } else {
                            sink.onFrame(data, bi.presentationTimeUs, bi.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
                        }
                    }
                    c.releaseOutputBuffer(index, false)
                } catch (e: IllegalStateException) {
                    // encodeur arrêté pendant le traitement : sans importance
                }
            }

            override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
                camHandler.post { if (gen == generation) fail("Erreur encodeur", e) }
            }

            override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) {}
        }, encHandler)

        val surface: Surface
        try {
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            surface = codec.createInputSurface()
            codec.start()
        } catch (e: Exception) {
            fail("Configuration de l'encodeur refusée (${cfg.width}×${cfg.height} @ $fps)", e)
            return
        }
        inputSurface = surface

        // --- Caméra ---
        try {
            cm.openCamera(cfg.cameraId, camExecutor, object : CameraDevice.StateCallback() {
                override fun onOpened(cam: CameraDevice) {
                    if (gen != generation) {
                        cam.close()
                        return
                    }
                    device = cam
                    val a = Active(gen, cam, surface, highSpeed, aeRange, chars, cfg)
                    active = a
                    createSession(a)
                }

                override fun onDisconnected(cam: CameraDevice) {
                    cam.close()
                    if (gen == generation) fail("Caméra déconnectée (utilisée par une autre application ?)")
                }

                override fun onError(cam: CameraDevice, error: Int) {
                    cam.close()
                    if (gen == generation) fail("Erreur caméra $error")
                }
            })
        } catch (e: Exception) {
            fail("Ouverture de la caméra impossible", e)
        }
    }

    /** Aperçu refusé par la caméra pour ce mode (on diffuse alors sans aperçu). */
    private var previewRejectedFor: Config? = null

    private fun createSession(a: Active) {
        val gen = a.gen
        val cam = a.cam
        val surface = a.encSurface
        val highSpeed = a.highSpeed
        val aeRange = a.aeRange
        val chars = a.chars
        val cfg = a.cfg

        try { session?.close() } catch (_: Exception) {}
        session = null
        try { previewSurface?.release() } catch (_: Exception) {}
        previewSurface = null

        // Aperçu : même taille que le flux (obligatoire en haute vitesse, et sans mise à l'échelle sinon).
        val texture = previewTexture
        val preview = if (texture != null && previewRejectedFor != cfg) {
            texture.setDefaultBufferSize(cfg.width, cfg.height)
            Surface(texture).also { previewSurface = it }
        } else {
            null
        }
        val outputs = mutableListOf(OutputConfiguration(surface))
        if (preview != null) outputs += OutputConfiguration(preview)

        val type = if (highSpeed) SessionConfiguration.SESSION_HIGH_SPEED else SessionConfiguration.SESSION_REGULAR
        val sc = SessionConfiguration(type, outputs, camExecutor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (gen != generation || active !== a) {
                        s.close()
                        return
                    }
                    session = s
                    try {
                        val req = cam.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(surface)
                            if (preview != null) addTarget(preview)
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeRange)
                            val afModes = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
                            if (CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes) {
                                set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                            }
                            // La stabilisation ajoute de la latence et recadre l'image : désactivée.
                            set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                        }.build()
                        if (highSpeed) {
                            val hs = s as CameraConstrainedHighSpeedCaptureSession
                            s.setRepeatingBurst(hs.createHighSpeedRequestList(req), null, camHandler)
                        } else {
                            s.setRepeatingRequest(req, null, camHandler)
                        }
                    } catch (e: Exception) {
                        fail("Démarrage de la capture impossible", e)
                        return
                    }
                    val facing = Caps.facingName(chars.get(CameraCharacteristics.LENS_FACING))
                    val orientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
                    info = Info(cfg.width, cfg.height, facing == "front", highSpeed, orientation)
                    if (!a.formatSent) {
                        // Le PC n'est prévenu qu'au premier démarrage, pas quand on ajoute/retire l'aperçu.
                        a.formatSent = true
                        lastConfig = cfg
                        current = "${cfg.width}×${cfg.height} @ ${cfg.fps} i/s ${cfg.codec.uppercase()} " +
                            "${cfg.bitrate / 1_000_000} Mb/s${if (highSpeed) " (haute vitesse)" else ""}"
                        sink.onFormat(
                            "codec=${cfg.codec} width=${cfg.width} height=${cfg.height} fps=${cfg.fps} " +
                                "bitrate=${cfg.bitrate} camera=${cfg.cameraId} facing=$facing " +
                                "orientation=$orientation highspeed=${if (highSpeed) 1 else 0}"
                        )
                    } else {
                        requestKeyFrame()
                    }
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    if (gen != generation || active !== a) return
                    if (preview != null) {
                        // Certains modes n'acceptent pas de 2e sortie : on diffuse sans aperçu.
                        Log.w(TAG, "aperçu refusé pour ${cfg.width}×${cfg.height} @ ${cfg.fps}")
                        previewRejectedFor = cfg
                        createSession(a)
                    } else {
                        fail("Session caméra refusée pour ${cfg.width}×${cfg.height} @ ${cfg.fps}")
                    }
                }
            })
        try {
            cam.createCaptureSession(sc)
        } catch (e: Exception) {
            fail("Création de la session caméra impossible", e)
        }
    }

    companion object {
        private const val TAG = "PhoneCam"
    }
}
