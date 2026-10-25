package org.midibottleneck.selftest;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Environment;
import android.os.PowerManager;
import android.os.Process;
import android.os.StatFs;
import android.util.DisplayMetrics;
import android.view.WindowManager;

import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/**
 * The Java-side capability report: device, Android version, processor as Android shows it, memory, power state and the
 * timer precision and thread priorities available to Java threads (the Java MIDI route runs on such threads). Measurements,
 * not pass/fail. The native part (libst_capability.so) reports the same for native threads.
 */
final class JavaCapability {
    private JavaCapability() {}

    /** One measured sleep series. */
    static final class Late {
        double median, p99, max;
        static Late of(double[] microseconds) {
            double[] sorted = microseconds.clone();
            Arrays.sort(sorted);
            Late late = new Late();
            late.median = sorted[sorted.length / 2];
            late.p99 = sorted[Math.min(sorted.length - 1, sorted.length * 99 / 100)];
            late.max = sorted[sorted.length - 1];
            return late;
        }
        @Override public String toString() {
            return String.format(Locale.US, "median %.0f us, 99%% %.0f us, max %.0f us", median, p99, max);
        }
    }

    /** Thread.sleep(millis) lateness in microseconds. */
    static Late sleepLate(long millis, int rounds) {
        double[] late = new double[rounds];
        for (int i = 0; i < rounds; i++) {
            long begin = System.nanoTime();
            try { Thread.sleep(millis); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            late[i] = (System.nanoTime() - begin - millis * 1_000_000L) / 1000.0;
        }
        return Late.of(late);
    }

    /** LockSupport.parkNanos lateness in microseconds. */
    static Late parkLate(long nanos, int rounds) {
        double[] late = new double[rounds];
        for (int i = 0; i < rounds; i++) {
            long begin = System.nanoTime();
            LockSupport.parkNanos(nanos);
            late[i] = (System.nanoTime() - begin - nanos) / 1000.0;
        }
        return Late.of(late);
    }

    static void run(Context context, Results results, boolean quick) {
        StringBuilder t = new StringBuilder("ANDROID CAPABILITY REPORT, Java part (measurements, not pass/fail)\n");
        t.append("\n1. Device and system\n");
        line(t, "manufacturer / model", Build.MANUFACTURER + " / " + Build.MODEL);
        line(t, "device / product / board / hardware", Build.DEVICE + " / " + Build.PRODUCT + " / " + Build.BOARD + " / " + Build.HARDWARE);
        line(t, "Android", Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), security patch " + (Build.VERSION.SDK_INT >= 23 ? Build.VERSION.SECURITY_PATCH : "?"));
        line(t, "build", Build.FINGERPRINT);
        line(t, "supported ABIs", Arrays.toString(Build.SUPPORTED_ABIS));
        if (Build.VERSION.SDK_INT >= 31) line(t, "SoC", Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL);
        line(t, "this app", context.getPackageName() + " (" + (Build.VERSION.SDK_INT >= 29 ? "main build" : "built for API 23+") + ")");
        PackageManager pm = context.getPackageManager();
        line(t, "feature MIDI", String.valueOf(pm.hasSystemFeature(PackageManager.FEATURE_MIDI)));
        line(t, "feature USB host", String.valueOf(pm.hasSystemFeature(PackageManager.FEATURE_USB_HOST)));
        line(t, "feature Bluetooth LE", String.valueOf(pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)));
        try {
            DisplayMetrics metrics = new DisplayMetrics();
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            @SuppressWarnings("deprecation") android.view.Display display = wm.getDefaultDisplay();
            display.getRealMetrics(metrics);
            line(t, "screen", metrics.widthPixels + " x " + metrics.heightPixels + " px, " + metrics.densityDpi + " dpi, "
                    + String.format(Locale.US, "%.1f Hz", display.getRefreshRate()));
        } catch (Throwable ignored) { }

        t.append("\n2. Processor\n");
        int cores = Runtime.getRuntime().availableProcessors();
        line(t, "logical processors (Java)", String.valueOf(cores));

        t.append("\n3. Memory and storage\n");
        ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        line(t, "total RAM", mb(mi.totalMem));
        line(t, "available RAM now", mb(mi.availMem) + (mi.lowMemory ? " (system says LOW memory)" : ""));
        line(t, "app memory class / large class", am.getMemoryClass() + " MB / " + am.getLargeMemoryClass() + " MB");
        line(t, "Java heap limit", mb(Runtime.getRuntime().maxMemory()));
        line(t, "low-RAM device", String.valueOf(am.isLowRamDevice()));
        try {
            StatFs fs = new StatFs(Environment.getExternalStorageDirectory().getPath());
            line(t, "free storage", mb(fs.getAvailableBytes()) + " of " + mb(fs.getTotalBytes()));
        } catch (Throwable ignored) { }

        t.append("\n4. Power state (affects timing)\n");
        PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        line(t, "battery saver on", String.valueOf(power.isPowerSaveMode()));
        if (Build.VERSION.SDK_INT >= 23) line(t, "app exempt from battery optimisation", String.valueOf(power.isIgnoringBatteryOptimizations(context.getPackageName())));
        if (Build.VERSION.SDK_INT >= 29) line(t, "thermal status (0 = none ... 6 = shutdown)", String.valueOf(power.getCurrentThermalStatus()));

        t.append("\n5. Clock and timer precision (Java threads, normal priority)\n");
        int rounds = quick ? 40 : 150;
        line(t, "System.nanoTime step", nanoStep() + " ns");
        line(t, "Thread.sleep(1 ms) late by", sleepLate(1, rounds).toString());
        line(t, "Thread.sleep(2 ms) late by", sleepLate(2, rounds).toString());
        line(t, "parkNanos(500 us) late by", parkLate(500_000, rounds * 2).toString());

        t.append("\n6. Thread priority from Java (a helper thread tries each level; Android thread priorities, lower = more urgent)\n");
        final StringBuilder priorityLines = new StringBuilder();
        final int priorityRounds = quick ? 30 : 100;
        Thread worker = new Thread(() -> priorities(priorityLines, priorityRounds), "selftest-priority");
        worker.start();
        try { worker.join(60_000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        t.append(priorityLines);

        results.write("capability-java.txt", t.toString());
        results.add("Capability report (Java part)", Results.Status.INFO, "written to capability-java.txt");
    }

    private static void priorities(StringBuilder out, int rounds) {
        int tid = Process.myTid();
        int original = Process.getThreadPriority(tid);
        line(out, "priority at start", String.valueOf(original));
        line(out, "at start: sleep 1 ms late by", sleepLate(1, rounds).toString());
        int[] levels = { Process.THREAD_PRIORITY_MORE_FAVORABLE, Process.THREAD_PRIORITY_FOREGROUND, Process.THREAD_PRIORITY_DISPLAY,
                Process.THREAD_PRIORITY_URGENT_DISPLAY, Process.THREAD_PRIORITY_AUDIO, Process.THREAD_PRIORITY_URGENT_AUDIO };
        String[] names = { "MORE_FAVORABLE (-1)", "FOREGROUND (-2)", "DISPLAY (-4)", "URGENT_DISPLAY (-8)", "AUDIO (-16)", "URGENT_AUDIO (-19)" };
        for (int i = 0; i < levels.length; i++) {
            try {
                Process.setThreadPriority(levels[i]);
                int now = Process.getThreadPriority(tid);
                if (now == levels[i]) {
                    line(out, names[i], "accepted; sleep 1 ms late by " + sleepLate(1, rounds) + "; parkNanos(500 us) late by " + parkLate(500_000, rounds));
                } else {
                    line(out, names[i], "no effect (priority reads " + now + ")");
                }
            } catch (Throwable t) {
                line(out, names[i], "refused (" + t.getClass().getSimpleName() + ")");
            }
        }
        try { Process.setThreadPriority(original); } catch (Throwable ignored) { }
        Thread.currentThread().setPriority(Thread.MAX_PRIORITY);
        line(out, "Thread.setPriority(MAX)", "Java priority reads " + Thread.currentThread().getPriority());
    }

    /** Best AUDIO-class priority a helper thread can take; used by the drift check. Returns the level that took, else 0. */
    static int raiseCurrentThread() {
        int[] levels = { Process.THREAD_PRIORITY_URGENT_AUDIO, Process.THREAD_PRIORITY_AUDIO, Process.THREAD_PRIORITY_URGENT_DISPLAY,
                Process.THREAD_PRIORITY_DISPLAY, Process.THREAD_PRIORITY_FOREGROUND };
        for (int level : levels) {
            try {
                Process.setThreadPriority(level);
                if (Process.getThreadPriority(Process.myTid()) == level) return level;
            } catch (Throwable ignored) { }
        }
        return 0;
    }

    private static long nanoStep() {
        long smallest = Long.MAX_VALUE;
        long last = System.nanoTime();
        for (int i = 0; i < 20000; i++) {
            long now = System.nanoTime();
            if (now != last) { smallest = Math.min(smallest, now - last); last = now; }
        }
        return smallest == Long.MAX_VALUE ? -1 : smallest;
    }

    private static void line(StringBuilder text, String name, String value) {
        text.append("  ").append(name).append(": ").append(value).append('\n');
    }

    private static String mb(long bytes) { return String.format(Locale.US, "%.0f MB", bytes / 1048576.0); }
}
