package fr.justpita.phonecam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.IBinder
import android.os.PowerManager

/** Service de premier plan (type caméra) : garde le serveur actif écran éteint. */
class StreamService : Service(), StreamServer.Listener {

    private var server: StreamServer? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "PhoneCam", NotificationManager.IMPORTANCE_LOW)
        )
        startForeground(NOTIF_ID, notification("En attente du PC"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        server = StreamServer(this, this).also {
            it.start()
            instance = it
        }
        State.running = true
        State.client = null
        State.streaming = false
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY

    override fun onDestroy() {
        server?.stop()
        server = null
        instance = null
        releaseLocks()
        State.running = false
        State.client = null
        State.streaming = false
        super.onDestroy()
    }

    override fun onClientChanged(address: String?) {
        State.client = address
        updateNotification()
    }

    override fun onStreaming(active: Boolean) {
        State.streaming = active
        if (active) acquireLocks() else releaseLocks()
        updateNotification()
    }

    private fun acquireLocks() {
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhoneCam:stream")
                .apply { acquire() }
        }
        if (wifiLock == null) {
            wifiLock = getSystemService(WifiManager::class.java)
                .createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "PhoneCam:wifi")
                .apply { acquire() }
        }
    }

    private fun releaseLocks() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    private fun updateNotification() {
        val text = when {
            State.streaming -> "Diffusion vers ${State.client ?: "le PC"}"
            State.client != null -> "Connecté à ${State.client}"
            else -> "En attente du PC"
        }
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, notification(text))
    }

    private fun notification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_phonecam)
            .setContentTitle("PhoneCam")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    /** État partagé, lu par l'écran principal. */
    object State {
        @Volatile var running = false
        @Volatile var client: String? = null
        @Volatile var streaming = false
    }

    companion object {
        private const val CHANNEL = "phonecam"
        private const val NOTIF_ID = 1
        @Volatile var instance: StreamServer? = null
    }
}
