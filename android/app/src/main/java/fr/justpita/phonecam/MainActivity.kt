package fr.justpita.phonecam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.TextView
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var details: TextView
    private lateinit var caps: TextView
    private lateinit var toggle: Button
    private val handler = Handler(Looper.getMainLooper())
    private var lastBytes = 0L
    private var lastFrames = 0L
    private var lastTime = 0L

    private val tick = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.status)
        details = findViewById(R.id.details)
        caps = findViewById(R.id.caps)
        toggle = findViewById(R.id.toggle)

        toggle.setOnClickListener {
            if (StreamService.State.running) {
                stopService(Intent(this, StreamService::class.java))
            } else {
                startServer()
            }
            handler.postDelayed({ refresh() }, 300)
        }
        findViewById<CheckBox>(R.id.keepScreen).setOnCheckedChangeListener { _, on ->
            if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        val missing = listOf(Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS)
            .filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 1)
        } else {
            onPermissionsReady()
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            onPermissionsReady()
        } else {
            status.text = "L'accès à la caméra est nécessaire."
        }
    }

    private fun onPermissionsReady() {
        Thread {
            val text = try {
                Caps.summary(Caps.collect(this))
            } catch (e: Exception) {
                "Lecture des capacités impossible : ${e.message}"
            }
            runOnUiThread { caps.text = text }
        }.start()
        if (!StreamService.State.running) startServer()
    }

    private fun startServer() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return
        startForegroundService(Intent(this, StreamService::class.java))
    }

    override fun onResume() {
        super.onResume()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    private fun localIps(): List<String> = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .mapNotNull { it.hostAddress }
    } catch (e: Exception) {
        emptyList()
    }

    private fun refresh() {
        val st = StreamService.State
        toggle.text = if (st.running) "Arrêter" else "Démarrer"
        status.text = when {
            !st.running -> "Arrêté"
            st.streaming -> "Diffusion vers ${st.client}"
            st.client != null -> "Connecté à ${st.client}"
            else -> "En attente du PC"
        }

        val sb = StringBuilder()
        val ips = localIps()
        sb.append("Adresse : ")
            .append(if (ips.isEmpty()) "aucun réseau" else ips.joinToString(", ") { "$it:${StreamServer.PORT}" })
            .append('\n')
        val server = StreamService.instance
        if (server != null && st.streaming) {
            val now = System.nanoTime()
            val bytes = server.bytesSent.get()
            val frames = server.framesSent.get()
            if (lastTime != 0L) {
                val dt = (now - lastTime) / 1e9
                val mbps = (bytes - lastBytes) * 8 / dt / 1e6
                val fps = (frames - lastFrames) / dt
                sb.append("Format : ${server.currentFormat}\n")
                sb.append(String.format("Débit : %.1f Mb/s · %.0f i/s · %d images sautées\n", mbps, fps, server.framesDropped.get()))
            }
            lastTime = now
            lastBytes = bytes
            lastFrames = frames
        } else {
            lastTime = 0L
        }
        details.text = sb.toString().trimEnd()
    }
}
