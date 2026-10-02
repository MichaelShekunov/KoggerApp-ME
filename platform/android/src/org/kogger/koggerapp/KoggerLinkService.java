package org.kogger.koggerapp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import androidx.core.app.NotificationCompat;

/**
 * Keeps a device link alive while the app is not on screen.
 *
 * Android 16 blocks the network of an app in the background
 * (netpolicy blocked=APP_BACKGROUND): on a test tablet the link to the boat went
 * silent the moment the app left the screen and came back 209.8 s later, when it
 * returned. A foreground service lifts that block, and while it runs this also
 * holds the Wi-Fi radio and the CPU awake, so a recording survives the screen
 * turning off. KoggerActivity starts it when the first link opens and stops it
 * when the last one closes.
 */
public class KoggerLinkService extends Service {
    private static final String TAG = KoggerLinkService.class.getSimpleName();
    private static final String CHANNEL_ID = "kogger_link";
    private static final int NOTIFICATION_ID = 0x4B47;
    private static final String LOCK_TAG = "KoggerApp:link";

    private WifiManager.WifiLock m_wifiLock;
    private PowerManager.WakeLock m_cpuLock;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        createChannel();

        final Intent open = new Intent(this, KoggerActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        final PendingIntent tap = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        final Notification notification = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("Connected to a device")
                .setSmallIcon(getApplicationInfo().icon)
                .setContentIntent(tap)
                .setOngoing(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        acquireLocks();
        Log.i(TAG, "link service: foreground");
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        releaseLocks();
        Log.i(TAG, "link service: stopped");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        final NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL_ID, "Device link",
                    NotificationManager.IMPORTANCE_LOW));
        }
    }

    /**
     * Holds the Wi-Fi radio at full power and the CPU running while the service
     * runs; both are released when it stops.
     */
    private void acquireLocks() {
        if (m_wifiLock == null) {
            final WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                m_wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, LOCK_TAG);
                m_wifiLock.setReferenceCounted(false);
            }
        }
        if (m_wifiLock != null && !m_wifiLock.isHeld()) {
            m_wifiLock.acquire();
        }
        if (m_cpuLock == null) {
            final PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                m_cpuLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, LOCK_TAG);
                m_cpuLock.setReferenceCounted(false);
            }
        }
        if (m_cpuLock != null && !m_cpuLock.isHeld()) {
            m_cpuLock.acquire();
        }
    }

    private void releaseLocks() {
        if (m_wifiLock != null && m_wifiLock.isHeld()) {
            m_wifiLock.release();
        }
        if (m_cpuLock != null && m_cpuLock.isHeld()) {
            m_cpuLock.release();
        }
    }
}
