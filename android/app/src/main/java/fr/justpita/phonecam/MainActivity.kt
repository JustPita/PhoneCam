package fr.justpita.phonecam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * Écran principal : aperçu plein écran pendant la diffusion (« retour écran »), état en haut,
 * boutons en bas (écran allumé, changer de caméra, arrêter), carte d'attente sinon.
 */
class MainActivity : Activity(), TextureView.SurfaceTextureListener {

    private lateinit var preview: TextureView
    private lateinit var waiting: LinearLayout
    private lateinit var waitTitle: TextView
    private lateinit var waitText: TextView
    private lateinit var address: TextView
    private lateinit var startButton: Button
    private lateinit var status: TextView
    private lateinit var statusDot: ImageView
    private lateinit var details: TextView
    private lateinit var switchButton: ImageButton
    private lateinit var screenButton: ImageButton
    private lateinit var powerButton: ImageButton
    private lateinit var toast: TextView
    private lateinit var obsButton: TextView

    private val handler = Handler(Looper.getMainLooper())
    private var resumed = false
    private var attachedServer: StreamServer? = null
    private var transformKey = ""
    private var keepScreenOn = false
    private var lastBytes = 0L
    private var lastFrames = 0L
    private var lastTime = 0L

    private val tick = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        preview = findViewById(R.id.preview)
        waiting = findViewById(R.id.waiting)
        waitTitle = findViewById(R.id.waitTitle)
        waitText = findViewById(R.id.waitText)
        address = findViewById(R.id.address)
        startButton = findViewById(R.id.startButton)
        status = findViewById(R.id.status)
        statusDot = findViewById(R.id.statusDot)
        details = findViewById(R.id.details)
        switchButton = findViewById(R.id.switchButton)
        screenButton = findViewById(R.id.screenButton)
        powerButton = findViewById(R.id.powerButton)
        toast = findViewById(R.id.toast)
        obsButton = findViewById(R.id.obsButton)
        obsButton.setOnClickListener {
            val server = StreamService.instance ?: return@setOnClickListener
            val on = !server.obsEnabled
            server.setObsEnabled(on)
            getSharedPreferences(StreamService.PREFS, MODE_PRIVATE).edit().putBoolean(StreamService.PREF_OBS, on).apply()
            showToast(
                if (on) "Flux OBS activé : ${obsUrl() ?: "pas de réseau"}\n(Source média OBS, « Fichier local » décoché)"
                else "Flux OBS désactivé"
            )
            refresh()
        }

        preview.surfaceTextureListener = this
        preview.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> transformKey = "" }

        keepScreenOn = getPreferences(MODE_PRIVATE).getBoolean("keepScreenOn", true)
        applyKeepScreenOn()

        switchButton.setOnClickListener {
            StreamService.instance?.let {
                Thread { it.switchCamera() }.start()
                showToast("Changement de caméra…")
            }
        }
        screenButton.setOnClickListener {
            keepScreenOn = !keepScreenOn
            getPreferences(MODE_PRIVATE).edit().putBoolean("keepScreenOn", keepScreenOn).apply()
            applyKeepScreenOn()
            showToast(if (keepScreenOn) "L'écran reste allumé pendant la diffusion" else "L'écran peut s'éteindre (la diffusion continue)")
        }
        powerButton.setOnClickListener {
            detachPreview()
            stopService(Intent(this, StreamService::class.java))
            showToast("PhoneCam est arrêté")
            handler.postDelayed({ refresh() }, 300)
        }
        startButton.setOnClickListener { startServer() }

        val missing = listOf(Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), 1) else startServer()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) startServer()
        else refresh()
    }

    private fun startServer() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
            return
        }
        if (!StreamService.State.running) startForegroundService(Intent(this, StreamService::class.java))
        handler.postDelayed({ refresh() }, 300)
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        handler.post(tick)
    }

    override fun onPause() {
        resumed = false
        handler.removeCallbacks(tick)
        // Plus d'aperçu en arrière-plan : la diffusion continue, sans dépenser de batterie pour l'écran.
        detachPreview()
        super.onPause()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        transformKey = ""
    }

    // --- Aperçu -----------------------------------------------------------------------------

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        transformKey = ""
        refresh()
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
        transformKey = ""
    }

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        detachPreview()
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    private fun attachPreviewIfNeeded(server: StreamServer?) {
        val st = preview.surfaceTexture
        if (!resumed || server == null || st == null || !StreamService.State.streaming) {
            if (!StreamService.State.streaming) attachedServer = null
            return
        }
        if (attachedServer === server) return
        attachedServer = server
        server.setPreview(st)
    }

    private fun detachPreview() {
        attachedServer?.setPreview(null)
        attachedServer = null
    }

    /**
     * Le flux arrive tel que le capteur le produit (paysage, non tourné) et le TextureView l'étire
     * à sa taille. On annule l'étirement, on tourne de (orientation capteur − rotation écran),
     * on ajuste sans déformation, puis miroir pour la caméra avant (comme un selfie).
     */
    private fun updateTransform(info: CameraStreamer.Info) {
        val vw = preview.width.toFloat()
        val vh = preview.height.toFloat()
        if (vw <= 0 || vh <= 0) return
        @Suppress("DEPRECATION")
        val displayDeg = when (windowManager.defaultDisplay.rotation) {
            Surface.ROTATION_90 -> 90
            Surface.ROTATION_180 -> 180
            Surface.ROTATION_270 -> 270
            else -> 0
        }
        val angle = (info.sensorOrientation - displayDeg + 360) % 360
        val key = "${info.width}x${info.height}-${info.front}-$angle-${vw}x$vh"
        if (key == transformKey) return
        transformKey = key

        val bw = info.width.toFloat()
        val bh = info.height.toFloat()
        val cx = vw / 2
        val cy = vh / 2
        val matrix = Matrix()
        // 1. Taille réelle de l'image au lieu de l'étirement à la vue
        matrix.setScale(bw / vw, bh / vh, cx, cy)
        // 2. Rotation
        matrix.postRotate(angle.toFloat(), cx, cy)
        // 3. Ajustement dans la vue
        val rw = if (angle % 180 == 0) bw else bh
        val rh = if (angle % 180 == 0) bh else bw
        val fit = minOf(vw / rw, vh / rh)
        matrix.postScale(fit, fit, cx, cy)
        // 4. Miroir pour la caméra avant
        if (info.front) matrix.postScale(-1f, 1f, cx, cy)
        preview.setTransform(matrix)
    }

    // --- Affichage -------------------------------------------------------------------------

    private fun applyKeepScreenOn() {
        screenButton.alpha = if (keepScreenOn) 1f else 0.45f
        val on = keepScreenOn && StreamService.State.streaming
        if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun showToast(text: String) {
        toast.text = text
        toast.visibility = View.VISIBLE
        handler.removeCallbacks(hideToast)
        handler.postDelayed(hideToast, 2200)
    }

    private val hideToast = Runnable { toast.visibility = View.GONE }

    private fun localIps(): List<String> = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .sortedBy { if (it.name.startsWith("wlan")) 0 else 1 } // Wi-Fi d'abord (pas l'IP mobile)
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .mapNotNull { it.hostAddress }
    } catch (e: Exception) {
        emptyList()
    }

    /** Adresse à coller dans OBS (IP Wi-Fi du téléphone). */
    private fun obsUrl(): String? = localIps().firstOrNull()?.let { "http://$it:${ObsServer.PORT}/" }

    private fun refresh() {
        val st = StreamService.State
        val server = StreamService.instance
        val streaming = st.running && st.streaming
        applyKeepScreenOn()

        val obsOn = server?.obsEnabled == true
        obsButton.visibility = if (st.running) View.VISIBLE else View.GONE
        obsButton.alpha = if (obsOn) 1f else 0.5f
        obsButton.setTextColor(if (obsOn) 0xFF4CC2FF.toInt() else 0xFFFFFFFF.toInt())
        obsButton.text = if (obsOn) "OBS ●" else "OBS"

        preview.visibility = if (streaming) View.VISIBLE else View.INVISIBLE
        waiting.visibility = if (streaming) View.GONE else View.VISIBLE
        switchButton.visibility = if (streaming && (server?.cameraCount ?: 0) > 1) View.VISIBLE else View.INVISIBLE
        screenButton.visibility = if (st.running) View.VISIBLE else View.INVISIBLE
        powerButton.visibility = if (st.running) View.VISIBLE else View.INVISIBLE

        val usb = st.client == "USB (adb)"
        when {
            !st.running -> {
                setStatus("Arrêté", 0xFF9AA0A6.toInt())
                waitTitle.text = "PhoneCam est arrêté"
                waitText.text = "Le PC ne peut pas utiliser la caméra tant que PhoneCam est arrêté."
                startButton.visibility = View.VISIBLE
                address.text = ""
            }
            streaming -> {
                setStatus(
                    when {
                        st.client == null -> "En direct pour OBS"
                        usb -> "En direct vers le PC · USB"
                        else -> "En direct vers le PC · Wi-Fi"
                    },
                    0xFFE53935.toInt(),
                )
                startButton.visibility = View.GONE
            }
            st.client != null -> {
                setStatus("Connecté au PC", 0xFF6CCB5F.toInt())
                waitTitle.text = "Connecté"
                waitText.text = "Le PC prépare la caméra…"
                startButton.visibility = View.GONE
            }
            else -> {
                setStatus("En attente du PC", 0xFFFCB900.toInt())
                waitTitle.text = "En attente du PC"
                waitText.text = "Ouvrez PhoneCam sur le PC : il se connecte tout seul,\n" +
                    "en USB ou sur le même Wi-Fi.\nLa diffusion continue écran éteint."
                startButton.visibility = View.GONE
            }
        }
        if (st.running && !streaming) {
            val ips = localIps()
            var text = if (ips.isEmpty()) "Aucun réseau Wi-Fi" else ips.joinToString("\n") { "IP du téléphone : $it" }
            if (obsOn) text += "\nOBS : ${obsUrl() ?: "pas de réseau"}"
            address.text = text
        }

        // Détails (format, débit) et aperçu
        if (streaming && server != null) {
            val info = server.streamInfo
            val sb = StringBuilder(server.currentFormat)
            val now = System.nanoTime()
            val bytes = server.bytesSent.get()
            val frames = server.framesSent.get()
            if (lastTime != 0L) {
                val dt = (now - lastTime) / 1e9
                sb.append(String.format("  ·  %.1f Mb/s  ·  %.0f i/s", (bytes - lastBytes) * 8 / dt / 1e6, (frames - lastFrames) / dt))
            }
            lastTime = now
            lastBytes = bytes
            lastFrames = frames
            if (obsOn) {
                val n = server.obsClients
                sb.append("\nOBS : ${obsUrl() ?: "pas de réseau"}")
                if (n > 0) sb.append("  ·  $n lecteur${if (n > 1) "s" else ""}")
            }
            details.text = sb.toString()
            attachPreviewIfNeeded(server)
            if (info != null) updateTransform(info)
        } else {
            details.text = ""
            lastTime = 0L
            attachedServer = null
        }
    }

    private fun setStatus(text: String, color: Int) {
        status.text = text
        statusDot.setColorFilter(color)
    }
}
