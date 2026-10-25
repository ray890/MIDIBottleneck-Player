package org.midibottleneck.player;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

/**
 * Foreground service that keeps playback alive with the screen off (required from API 29 for a long-running audio task;
 * it also holds a partial wake lock while a song plays or is paused, so the processor keeps running at the song's pace).
 * Started by {@link PlayerHost} when playback starts, stopped when it ends. Nothing else lives here: the player itself
 * is in the process-wide {@link PlayerHost}.
 */
public class PlayerService extends Service {
    private static final String CHANNEL = "playback";
    private static final int NOTIFICATION_ID = 1;
    static final String ACTION_STOP = "org.midibottleneck.player.STOP";

    private static PlayerService current;
    private PowerManager.WakeLock wakeLock;
    private String title = "", text = "";

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            PlayerHost.get(this).stopFromService();
            return START_NOT_STICKY;
        }
        current = this;
        if (wakeLock == null) {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MIDIBottleneck:playback");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
        }
        Notification n = build();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        wakeLock = null;
        if (current == this) current = null;
        super.onDestroy();
    }

    /** Refreshes the notification text (main thread). */
    static void update(String fileName, String state) {
        PlayerService s = current;
        if (s == null) return;
        if (fileName.equals(s.title) && state.equals(s.text)) return;
        s.title = fileName;
        s.text = state;
        NotificationManager nm = (NotificationManager) s.getSystemService(Context.NOTIFICATION_SERVICE);
        nm.notify(NOTIFICATION_ID, s.build());
    }

    private Notification build() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.channel_name),
                    NotificationManager.IMPORTANCE_LOW));
        }
        int immutable = PendingIntent.FLAG_IMMUTABLE;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class)
                .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP), immutable);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, PlayerService.class).setAction(ACTION_STOP), immutable);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        b.setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle(title.isEmpty() ? getString(R.string.app_name) : title)
                .setContentText(text.isEmpty() ? getString(R.string.app_name) : text)
                .setContentIntent(open)
                .setOngoing(true)
                .addAction(android.R.drawable.ic_menu_close_clear_cancel, getString(R.string.stop), stop);
        return b.build();
    }
}
