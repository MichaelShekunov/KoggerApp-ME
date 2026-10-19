package org.kogger.koggerapp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;
import java.lang.reflect.Method;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.net.wifi.WifiManager;
import android.provider.Settings;
import android.util.Log;
import android.view.WindowManager;
import android.app.Activity;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.core.content.FileProvider;

import org.qtproject.qt.android.bindings.QtActivity;

public class KoggerActivity extends QtActivity {
    private static final String TAG = KoggerActivity.class.getSimpleName();
    private static final String SCREEN_BRIGHT_WAKE_LOCK_TAG = "KoggerApp";
    private static final String MULTICAST_LOCK_TAG = "KoggerApp";
    private static final int STORAGE_PERMISSION_REQUEST_CODE = 1;

    private static KoggerActivity m_instance = null;
    private static boolean m_storagePermissionPending = false;

    private PowerManager.WakeLock m_wakeLock;
    private WifiManager.MulticastLock m_wifiMulticastLock;

    public KoggerActivity() {
        m_instance = this;
    }

    /**
     * Returns the singleton instance of KoggerActivity.
     *
     * @return The current instance of KoggerActivity.
     */
    public static KoggerActivity getInstance() {
        return m_instance;
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        nativeInit();
        checkStoragePermissions();
        acquireWakeLock();
        keepScreenOn();
        setupMulticastLock();

        KoggerUsbSerialManager.initialize(this);

        // A restored activity carries the intent it was first started with: that one was handled.
        if (savedInstanceState == null) {
            handleOpenIntent(getIntent());
        }
    }

    /**
     * singleTask: a file opened from another app while KoggerApp runs arrives here, handed on by
     * KoggerOpenActivity.
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleOpenIntent(intent);
    }

    /**
     * Passes a file another app opened with KoggerApp (VIEW) or shared to it (SEND) to the native
     * side, which keeps it if it is a KOGGER map: docs/map-export.md. A task brought back from
     * Recents is relaunched with the intent it began with; that one was handled already.
     */
    private void handleOpenIntent(final Intent intent) {
        if (intent == null || (intent.getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0) {
            return;
        }

        final String action = intent.getAction();
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(action)) {
            uri = intent.getData();
        } else if (Intent.ACTION_SEND.equals(action)) {
            uri = streamOf(intent);
        }
        if (uri == null) {
            return;
        }

        Log.i(TAG, "Open " + action + " " + intent.getType() + " " + uri);
        try {
            koggerOpenFile(uri.toString());
        } catch (final UnsatisfiedLinkError e) {
            Log.e(TAG, "Native open-file callback is not registered", e);
        }
    }

    @SuppressWarnings("deprecation")
    private static Uri streamOf(final Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri.class);
        }
        return intent.getParcelableExtra(Intent.EXTRA_STREAM);
    }

    /**
     * Offers a file to other apps through the system share sheet (a KOGGER map to a messenger).
     * The file goes out through the app's FileProvider; one in a folder the provider does not
     * cover (a removable card) is copied into the cache first.
     *
     * @param path         the file.
     * @param mimeType     the type the receiving apps see.
     * @param chooserTitle the share sheet's title.
     * @return false when the file cannot be offered.
     */
    public static boolean shareFile(final String path, final String mimeType, final String chooserTitle) {
        if (m_instance == null) {
            Log.w(TAG, "shareFile: activity instance is null");
            return false;
        }

        try {
            final File file = new File(path);
            if (!file.isFile()) {
                Log.w(TAG, "shareFile: not a file: " + path);
                return false;
            }

            final String authority = m_instance.getPackageName() + ".qtprovider";
            Uri uri;
            try {
                uri = FileProvider.getUriForFile(m_instance, authority, file);
            } catch (final IllegalArgumentException e) {
                uri = FileProvider.getUriForFile(m_instance, authority, copyToShareCache(file));
            }

            final Intent send = new Intent(Intent.ACTION_SEND);
            send.setType(mimeType);
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.setClipData(ClipData.newRawUri(file.getName(), uri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            final Intent chooser = Intent.createChooser(send, chooserTitle);
            m_instance.runOnUiThread(() -> {
                try {
                    m_instance.startActivity(chooser);
                } catch (final Exception e) {
                    Log.e(TAG, "shareFile: the share sheet did not open", e);
                }
            });
            return true;
        } catch (final Exception e) {
            Log.e(TAG, "shareFile failed: " + path, e);
            return false;
        }
    }

    /**
     * Copies a file into getCacheDir()/share (res/xml/qtprovider_paths.xml), replacing what an
     * earlier share left there.
     */
    private static File copyToShareCache(final File file) throws IOException {
        final File dir = new File(m_instance.getCacheDir(), "share");
        final File[] old = dir.listFiles();
        if (old != null) {
            for (final File f : old) {
                f.delete();
            }
        }
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("cannot make " + dir);
        }

        final File copy = new File(dir, file.getName());
        try (InputStream in = new FileInputStream(file); OutputStream out = new FileOutputStream(copy)) {
            final byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
        }
        return copy;
    }

    /**
     * Delivers the outcome of a runtime storage permission request (API 23-29).
     */
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode != STORAGE_PERMISSION_REQUEST_CODE) {
            return;
        }

        boolean granted = grantResults.length > 0;
        for (int result : grantResults) {
            if (result != PackageManager.PERMISSION_GRANTED) {
                granted = false;
                break;
            }
        }

        m_storagePermissionPending = false;
        notifyStoragePermissionResult(granted);
    }

    /**
     * MANAGE_EXTERNAL_STORAGE (API 30+) is granted in system settings, not in a
     * runtime dialog, so its outcome is only observable when the user comes back.
     */
    @Override
    protected void onResume() {
        super.onResume();

        if (!m_storagePermissionPending || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;
        }

        final boolean granted = Environment.isExternalStorageManager();
        if (granted) {
            m_storagePermissionPending = false;
            notifyStoragePermissionResult(true);
        }
    }

    private void notifyStoragePermissionResult(boolean granted) {
        Log.i(TAG, "Storage permission result: " + granted);

        try {
            koggerStoragePermissionResult(granted);
        } catch (final UnsatisfiedLinkError e) {
            Log.e(TAG, "Native storage permission callback is not registered", e);
        }
    }

    @Override
    protected void onDestroy() {
        try {
            stopService(new Intent(this, KoggerLinkService.class));
            releaseMulticastLock();
            releaseWakeLock();
            KoggerUsbSerialManager.cleanup(this);
        } catch (final Exception e) {
            Log.e(TAG, "Exception onDestroy()", e);
        }

        if (isFinishing()) {
            startExitWatchdog();
        }
        super.onDestroy();
    }

    /**
     * Makes leaving for good certain. Qt's own onDestroy ends the process with
     * System.exit(0), but only after QtNative.terminateQt() returns, and on a
     * tablet it did not: the app, kept alive overnight by KoggerLinkService, was
     * swiped out of Recents, Android logged "Activity destroy timeout", and the
     * process stayed with its main thread stuck - the next launch showed nothing
     * but the splash window. If the process is still here 3 s after the activity
     * is gone, this ends it, and the next launch is a clean start.
     */
    private static void startExitWatchdog() {
        final Thread watchdog = new Thread(() -> {
            try {
                Thread.sleep(3000);
            } catch (final InterruptedException e) {
                return;
            }
            Log.w(TAG, "Qt did not exit 3 s after the activity was destroyed - ending the process");
            android.os.Process.killProcess(android.os.Process.myPid());
        }, "KoggerExitWatchdog");
        watchdog.setDaemon(true);
        watchdog.start();
    }

    /**
     * Keeps the screen on by adding the appropriate window flag.
     */
    private void keepScreenOn() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /**
     * Acquires a wake lock to keep the CPU running.
     */
    private void acquireWakeLock() {
        final PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        m_wakeLock = pm.newWakeLock(PowerManager.SCREEN_BRIGHT_WAKE_LOCK, SCREEN_BRIGHT_WAKE_LOCK_TAG);
        if (m_wakeLock != null) {
            m_wakeLock.acquire();
        } else {
            Log.w(TAG, "SCREEN_BRIGHT_WAKE_LOCK not acquired!");
        }
    }

    /**
     * Releases the wake lock if held.
     */
    private void releaseWakeLock() {
        if (m_wakeLock != null && m_wakeLock.isHeld()) {
            m_wakeLock.release();
        }
    }

    /**
     * Sets up a multicast lock to allow multicast packets.
     */
    private void setupMulticastLock() {
        if (m_wifiMulticastLock == null) {
            final WifiManager wifi = (WifiManager) getSystemService(Context.WIFI_SERVICE);
            m_wifiMulticastLock = wifi.createMulticastLock(MULTICAST_LOCK_TAG);
            m_wifiMulticastLock.setReferenceCounted(true);
        }

        m_wifiMulticastLock.acquire();
        Log.d(TAG, "Multicast lock: " + m_wifiMulticastLock.toString());
    }

    /**
     * Releases the multicast lock if held.
     */
    private void releaseMulticastLock() {
        if (m_wifiMulticastLock != null && m_wifiMulticastLock.isHeld()) {
            m_wifiMulticastLock.release();
            Log.d(TAG, "Multicast lock released.");
        }
    }

    /**
     * Starts or stops KoggerLinkService: on while at least one device link is
     * open, so the link outlives the app leaving the screen.
     *
     * @param active true when a link is open.
     */
    public static void setLinkServiceActive(final boolean active) {
        if (m_instance == null) {
            Log.w(TAG, "setLinkServiceActive: activity instance is null");
            return;
        }
        m_instance.runOnUiThread(() -> {
            final Intent intent = new Intent(m_instance, KoggerLinkService.class);
            try {
                if (active) {
                    ContextCompat.startForegroundService(m_instance, intent);
                } else {
                    m_instance.stopService(intent);
                }
            } catch (final Exception e) {
                // Android 12+ refuses to start one from the background; the next
                // link opened in the foreground starts it.
                Log.e(TAG, "link service " + (active ? "start" : "stop") + " failed", e);
            }
        });
    }

    /**
     * Moves the app task to background (same UX as Home button).
     */
    public static void moveTaskToBackApp() {
        if (m_instance == null) {
            Log.w(TAG, "moveTaskToBackApp: activity instance is null");
            return;
        }

        m_instance.runOnUiThread(() -> m_instance.moveTaskToBack(true));
    }

    public static String getSDCardPath() {
        StorageManager storageManager = (StorageManager)m_instance.getSystemService(Activity.STORAGE_SERVICE);
        List<StorageVolume> volumes = storageManager.getStorageVolumes();
        
        for (StorageVolume vol : volumes) {
            if (!vol.isRemovable()) {
                continue;
            }
            
            String path = null;
            
            // For Android 11+ (API 30+), use the proper getDirectory() method
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                File directory = vol.getDirectory();
                if (directory != null) {
                    path = directory.getAbsolutePath();
                }
            } else {
                // For older versions, use reflection to get the path
                try {
                    Method mMethodGetPath = vol.getClass().getMethod("getPath");
                    path = (String) mMethodGetPath.invoke(vol);
                } catch (Exception e) {
                    Log.e(TAG, "Failed to get path via reflection", e);
                    continue;
                }
            }
            
            if (path != null && !path.isEmpty()) {
                Log.i(TAG, "removable sd card mounted at " + path);
                return path;
            }
        }
        
        Log.w(TAG, "No removable SD card found");
        return "";
    }

    /**
     * Checks and requests storage permissions for SD card access.
     * For Android 11+ (API 30+), this requires MANAGE_EXTERNAL_STORAGE permission.
     *
     * @return true if permissions are granted, false otherwise
     */
    public static boolean checkStoragePermissions() {
        if (m_instance == null) {
            Log.e(TAG, "Activity instance is null");
            return false;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android 11+ (API 30+) requires MANAGE_EXTERNAL_STORAGE for full SD card access
            if (!Environment.isExternalStorageManager()) {
                Log.i(TAG, "MANAGE_EXTERNAL_STORAGE not granted, requesting...");
                m_storagePermissionPending = true;
                try {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    intent.setData(Uri.parse("package:" + m_instance.getPackageName()));
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    m_instance.startActivity(intent);
                } catch (Exception e) {
                    Log.e(TAG, "Failed to open storage permission settings", e);
                    // Fallback to general settings
                    Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    m_instance.startActivity(intent);
                }
                return false;
            }
            Log.i(TAG, "MANAGE_EXTERNAL_STORAGE already granted");
            return true;
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            // Android 6.0+ (API 23+) requires runtime permissions
            String[] permissions = {
                android.Manifest.permission.READ_EXTERNAL_STORAGE,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE
            };

            boolean allGranted = true;
            for (String permission : permissions) {
                if (ContextCompat.checkSelfPermission(m_instance, permission) != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false;
                    break;
                }
            }

            if (!allGranted) {
                Log.i(TAG, "Storage permissions not granted, requesting...");
                m_storagePermissionPending = true;
                ActivityCompat.requestPermissions(m_instance, permissions, STORAGE_PERMISSION_REQUEST_CODE);
                return false;
            }

            Log.i(TAG, "Storage permissions already granted");
            return true;
        } else {
            // Below Android 6.0, permissions are granted at install time
            return true;
        }
    }

    // Native C++ functions
    public native boolean nativeInit();
    public native void koggerLogDebug(final String message);
    public native void koggerLogWarning(final String message);
    public native void koggerStoragePermissionResult(final boolean granted);
    public native void koggerOpenFile(final String uri);
}
