package fr.justpita.phonecam

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Size
import kotlin.math.min
import kotlin.math.roundToInt

/** Lit ce que la caméra ET l'encodeur matériel savent faire, pour le proposer au PC. */
object Caps {
    /** Nom court -> type MIME, dans l'ordre de préférence. */
    val CODECS = linkedMapOf(
        "hevc" to MediaFormat.MIMETYPE_VIDEO_HEVC,
        "avc" to MediaFormat.MIMETYPE_VIDEO_AVC,
    )

    data class Mode(
        val cameraId: String,
        val width: Int,
        val height: Int,
        val fps: Int,
        val highSpeed: Boolean,
        val codec: String,
    )

    data class Cam(val id: String, val facing: String, val orientation: Int)

    data class All(val cams: List<Cam>, val codecs: List<String>, val modes: List<Mode>)

    /** Encodeur à utiliser pour un type MIME : matériel en priorité. */
    fun findEncoder(mime: String): MediaCodecInfo? {
        val list = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            info.isEncoder && info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
        }
        return list.firstOrNull { it.isHardwareAccelerated && !it.isAlias }
            ?: list.firstOrNull { !it.isSoftwareOnly }
            ?: list.firstOrNull()
    }

    private fun encoderMaxFps(vc: MediaCodecInfo.VideoCapabilities, w: Int, h: Int): Int {
        if (!vc.isSizeSupported(w, h)) return 0
        return try {
            vc.getSupportedFrameRatesFor(w, h).upper.toInt()
        } catch (e: IllegalArgumentException) {
            0
        }
    }

    fun facingName(f: Int?): String = when (f) {
        CameraMetadata.LENS_FACING_FRONT -> "front"
        CameraMetadata.LENS_FACING_BACK -> "back"
        else -> "external"
    }

    /** Plus grand fps « normal » (session classique) réellement atteignable pour une taille donnée. */
    fun regularMaxFps(chars: CameraCharacteristics, size: Size): Int {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return 0
        val ae = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return 30
        val dur = try {
            map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        } catch (e: IllegalArgumentException) {
            0L
        }
        val byDuration = if (dur > 0) (1e9 / dur).roundToInt() else Int.MAX_VALUE
        return ae.map { it.upper }.filter { it <= byDuration }.maxOrNull() ?: 0
    }

    /** fps fixes disponibles en mode haute vitesse pour cette taille (vide si non pris en charge). */
    fun highSpeedFps(chars: CameraCharacteristics, size: Size): List<Int> {
        val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: return emptyList()
        if (CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO !in caps) return emptyList()
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        if (map.highSpeedVideoSizes.none { it == size }) return emptyList()
        return map.getHighSpeedVideoFpsRangesFor(size)
            .filter { it.lower == it.upper }
            .map { it.upper }
            .distinct()
            .sorted()
    }

    fun collect(ctx: Context): All {
        val cm = ctx.getSystemService(CameraManager::class.java)
        val cams = mutableListOf<Cam>()
        val modes = mutableListOf<Mode>()

        val encoders = CODECS.mapNotNull { (name, mime) ->
            findEncoder(mime)?.let { name to it.getCapabilitiesForType(mime).videoCapabilities }
        }

        for (id in cm.cameraIdList) {
            val chars = cm.getCameraCharacteristics(id)
            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: continue
            cams += Cam(
                id,
                facingName(chars.get(CameraCharacteristics.LENS_FACING)),
                chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
            )

            val sizes = (map.getOutputSizes(MediaCodec::class.java)?.toList() ?: emptyList())
                .filter { it.width >= 320 && it.height >= 240 && it.width >= it.height }
                .distinct()

            for (size in sizes) {
                val camFps = regularMaxFps(chars, size)
                val hsList = highSpeedFps(chars, size)
                for ((codec, vc) in encoders) {
                    val encFps = encoderMaxFps(vc, size.width, size.height)
                    if (encFps <= 0) continue
                    val regular = min(camFps, encFps)
                    if (regular >= 10) {
                        modes += Mode(id, size.width, size.height, regular, false, codec)
                    }
                    // Haute vitesse : chaque fréquence fixe proposée (60, 120, 240…), au-delà du mode normal.
                    for (hs in hsList) {
                        if (hs > regular && hs <= encFps) {
                            modes += Mode(id, size.width, size.height, hs, true, codec)
                        }
                    }
                }
            }
        }
        return All(cams, encoders.map { it.first }, modes)
    }

    fun helloText(all: All): String = buildString {
        appendLine("device ${Build.MANUFACTURER} ${Build.MODEL}".replace('\n', ' '))
        all.codecs.forEach { appendLine("codec $it") }
        all.cams.forEach { appendLine("camera ${it.id} ${it.facing} ${it.orientation}") }
        all.modes.forEach {
            appendLine("mode ${it.cameraId} ${it.width} ${it.height} ${it.fps} ${if (it.highSpeed) 1 else 0} ${it.codec}")
        }
    }

    fun summary(all: All): String = buildString {
        for (cam in all.cams) {
            val m = all.modes.filter { it.cameraId == cam.id }
            if (m.isEmpty()) continue
            val best = m.maxWith(compareBy<Mode>({ it.width * it.height }, { it.fps }))
            val fastest = m.maxWith(compareBy<Mode>({ it.fps }, { it.width * it.height }))
            appendLine(
                "Caméra ${cam.id} (${if (cam.facing == "back") "arrière" else if (cam.facing == "front") "avant" else "externe"}) : " +
                    "max ${best.width}×${best.height} @ ${best.fps} i/s, " +
                    "le plus rapide ${fastest.width}×${fastest.height} @ ${fastest.fps} i/s"
            )
        }
        append("Encodeurs : ${all.codecs.joinToString(", ") { it.uppercase() }}")
    }
}
