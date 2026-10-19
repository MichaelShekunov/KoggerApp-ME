package org.kogger.koggerapp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/**
 * Takes a file another app opens with KoggerApp or shares to it (a KOGGER map from a messenger,
 * docs/map-export.md) and hands it on to KoggerActivity in the app's own task, then closes.
 *
 * Not KoggerActivity itself: a messenger opens a file with startActivityForResult (Telegram does),
 * and Android then puts even a singleTask activity into the messenger's task. That second
 * KoggerActivity makes Qt restart the whole app (QtActivityBase.restartApplication) and the file is
 * lost -- seen on a phone on 2026-10-18. Started from here, with no result asked for, KoggerActivity
 * is found in its own task and gets the file in onNewIntent, or the app starts with it.
 */
public class KoggerOpenActivity extends Activity {
    private static final String TAG = KoggerOpenActivity.class.getSimpleName();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        final Intent received = getIntent();
        if (received != null) {
            // The action, data, type, extras and ClipData go on as they came; the read grant is
            // passed on with them, so it outlives this activity.
            final Intent forward = new Intent(received);
            forward.setComponent(new ComponentName(this, KoggerActivity.class));
            forward.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TOP
                    | Intent.FLAG_ACTIVITY_SINGLE_TOP
                    | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                startActivity(forward);
            } catch (final Exception e) {
                Log.e(TAG, "Could not hand the file to KoggerActivity", e);
            }
        }

        finish();
    }
}
