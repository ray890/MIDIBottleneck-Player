package org.midibottleneck.core.route;

import java.util.Arrays;

/**
 * Timing of sends to the open device of a route, for the self-test app.
 *
 * <pre>
 * RouteBench.Result r = RouteBench.run(route, 200, 250, false);        // engine path (C++ output -> device)
 * RouteBench.Result d = RouteBench.runDirect(route, 200, 250, false);  // Java API path (sendShort/beginBatch/endBatch)
 * String text = r.summary();
 * </pre>
 *
 * A burst is what the engine does when events fall due together: beginBatch, N short messages, endBatch (one write to
 * the device). The silent mode sends controller 20 (undefined, no sound); audible mode sends note on/off pairs with a
 * low velocity. Both must be called from a background thread with a device open; they block for the duration of the run.
 * Times are nanoseconds from System.nanoTime (Java) or std::chrono::steady_clock (native).
 */
public final class RouteBench {
    private RouteBench() {}

    public static final class Result {
        public String path;
        public long bursts, perBurst, sent, failed, writes;
        public long totalNs, minBurstNs, medianBurstNs, p99BurstNs, maxBurstNs;
        public String error;

        /** Microseconds per message over the whole run. */
        public double microsPerMessage() { return sent == 0 ? 0 : totalNs / 1000.0 / sent; }

        public String summary() {
            if (error != null && sent == 0) return path + ": failed - " + error;
            return String.format(java.util.Locale.US,
                "%s: %d messages in %d bursts of %d, %s, %d failed; total %.2f ms, %.2f us/message; "
                    + "burst min %.1f us, median %.1f us, p99 %.1f us, max %.1f us%s",
                path, sent, bursts, perBurst, writes < 0 ? "writes not counted" : writes + " writes", failed, totalNs / 1e6, microsPerMessage(),
                minBurstNs / 1e3, medianBurstNs / 1e3, p99BurstNs / 1e3, maxBurstNs / 1e3,
                error != null ? "; first error: " + error : "");
        }
    }

    /** The engine path: the attached route's C++ output. Needs the native library and an open route. */
    public static Result run(MidiRoute route, int bursts, int perBurst, boolean audible) {
        Result r = new Result();
        r.path = route.name() + " (engine path)";
        r.bursts = bursts;
        r.perBurst = perBurst;
        if (!route.isOpen()) { r.error = "No MIDI output device is open."; return r; }
        long[] v = RouteNative.bench(bursts, perBurst, audible);
        if (v == null) { r.error = "The native library is not loaded."; return r; }
        r.sent = v[0];
        r.failed = v[1];
        r.totalNs = v[2];
        r.minBurstNs = v[3];
        r.medianBurstNs = v[4];
        r.p99BurstNs = v[5];
        r.maxBurstNs = v[6];
        r.writes = v[7];
        r.error = RouteNative.benchError();
        return r;
    }

    /** The Java API path: route.beginBatch(), route.sendShort(...) x perBurst, route.endBatch(). */
    public static Result runDirect(MidiRoute route, int bursts, int perBurst, boolean audible) {
        Result r = new Result();
        r.path = route.name() + " (Java API)";
        r.bursts = bursts;
        r.perBurst = perBurst;
        if (!route.isOpen()) { r.error = "No MIDI output device is open."; return r; }
        long[] times = new long[Math.max(bursts, 1)];
        long start = System.nanoTime();
        for (int b = 0; b < bursts; b++) {
            long t0 = System.nanoTime();
            route.beginBatch();
            for (int i = 0; i < perBurst; i++) {
                if (route.sendShort(message(i, audible))) r.sent++;
                else { r.failed++; if (r.error == null) r.error = route.lastError(); }
            }
            route.endBatch();
            times[b] = System.nanoTime() - t0;
        }
        r.totalNs = System.nanoTime() - start;
        if (bursts > 0) {
            long[] sorted = Arrays.copyOf(times, bursts);
            Arrays.sort(sorted);
            r.minBurstNs = sorted[0];
            r.medianBurstNs = sorted[bursts / 2];
            r.p99BurstNs = sorted[Math.min(bursts - 1, (int) (bursts * 0.99))];
            r.maxBurstNs = sorted[bursts - 1];
        }
        r.writes = -1;   // not visible from the Java API
        return r;
    }

    static int message(int i, boolean audible) {
        if (audible) {
            int key = 60 + ((i / 2) % 12);
            return (i % 2 == 0) ? (0x90 | (key << 8) | (20 << 16)) : (0x80 | (key << 8));
        }
        return 0xB0 | (20 << 8) | ((i & 127) << 16);
    }
}
