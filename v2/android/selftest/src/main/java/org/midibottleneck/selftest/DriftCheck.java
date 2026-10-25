package org.midibottleneck.selftest;

import android.os.SystemClock;

import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/**
 * Long-run timing check: a Java thread ticks every 2 ms against absolute deadlines (sleep, then spin for the last
 * 200 microseconds, the way the player's scheduler waits) and records how late each tick is. Run once at the normal
 * priority and once raised. Also compares the monotonic clock with the elapsed-real-time clock (clock drift) and counts
 * how often the app went to the background during the run (timing is not representative then).
 */
final class DriftCheck {
    private DriftCheck() {}

    interface BackgroundCounter { int count(); }

    static void run(Results results, int secondsEach, BackgroundCounter background) {
        StringBuilder t = new StringBuilder("LONG-RUN TIMING (a tick every 2 ms against absolute deadlines)\n");
        boolean ok = true;
        final int pausesBefore = background.count();
        for (int pass = 0; pass < 2; pass++) {
            final boolean raise = pass == 1;
            final String[] outcome = new String[1];
            Thread thread = new Thread(() -> outcome[0] = measure(raise, secondsEach), "selftest-drift");
            thread.start();
            try { thread.join((secondsEach + 30) * 1000L); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            if (outcome[0] == null) { ok = false; outcome[0] = "did not finish in time\n"; }
            t.append('\n').append(raise ? "Raised priority\n" : "Normal priority\n").append(outcome[0]);
        }
        int pauses = background.count() - pausesBefore;
        t.append("\nThe app went to the background ").append(pauses).append(" time(s) during this check")
                .append(pauses > 0 ? " (timing numbers above may be worse than normal)" : "").append(".\n");
        results.write("timing-longrun.txt", t.toString());
        results.add("Long-run timing", ok ? Results.Status.PASS : Results.Status.FAIL,
                ok ? "2 x " + secondsEach + " s; see timing-longrun.txt" : "a pass did not finish; see timing-longrun.txt");
    }

    private static String measure(boolean raise, int seconds) {
        StringBuilder t = new StringBuilder();
        int level = 0;
        if (raise) {
            level = JavaCapability.raiseCurrentThread();
            t.append("  priority taken: ").append(level == 0 ? "none (all refused; running at the normal level)" : String.valueOf(level)).append('\n');
        }
        final long periodNs = 2_000_000L;
        final int ticks = (int) (seconds * 1_000_000_000L / periodNs);
        double[] late = new double[ticks];
        long startMono = System.nanoTime(), startReal = SystemClock.elapsedRealtimeNanos();
        for (int i = 0; i < ticks; i++) {
            long deadline = startMono + (i + 1) * periodNs;
            long rest = deadline - System.nanoTime() - 200_000L;
            if (rest > 0) LockSupport.parkNanos(rest);
            while (System.nanoTime() < deadline) { /* spin the last part */ }
            late[i] = (System.nanoTime() - deadline) / 1000.0;
        }
        long endMono = System.nanoTime(), endReal = SystemClock.elapsedRealtimeNanos();
        double[] sorted = late.clone();
        Arrays.sort(sorted);
        int over1 = 0, over2 = 0, over5 = 0, over10 = 0;
        for (double v : late) { if (v > 1000) over1++; if (v > 2000) over2++; if (v > 5000) over5++; if (v > 10000) over10++; }
        t.append(String.format(Locale.US, "  ticks: %d in %.2f s%n", ticks, (endMono - startMono) / 1e9));
        t.append(String.format(Locale.US, "  late by: median %.0f us, 99%% %.0f us, 99.9%% %.0f us, max %.0f us%n",
                sorted[ticks / 2], sorted[ticks * 99 / 100], sorted[Math.min(ticks - 1, ticks * 999 / 1000)], sorted[ticks - 1]));
        t.append(String.format(Locale.US, "  ticks later than 1 ms: %d, 2 ms: %d, 5 ms: %d, 10 ms: %d%n", over1, over2, over5, over10));
        double driftUs = ((endMono - startMono) - (endReal - startReal)) / 1000.0;
        t.append(String.format(Locale.US, "  monotonic clock minus elapsed-real-time clock over the run: %.0f us%n", driftUs));
        return t.toString();
    }
}
