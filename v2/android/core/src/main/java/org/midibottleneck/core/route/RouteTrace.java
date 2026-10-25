package org.midibottleneck.core.route;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/**
 * A small, lock-free record of the latest writes to a MIDI device, for the overload investigation: for each write the
 * bytes handed over, the time spent inside the call and the return value. Off by default: while {@link #on} is false a
 * write costs one volatile read and nothing else (no clock read, no store). The main session turns it on (see
 * PlayerHost: the file {@code files/route-trace.on}) and reads it from logcat with
 * {@code adb logcat -s MIDIBN-route}; {@link #dump} writes the ring and a one-line summary.
 *
 * Two rings: this one (written by the direct USB route's sender thread) and the native one (BatchingOutput, which covers
 * the Android MIDI service routes: JNI to MidiRoute.sendBytes and AMidi); {@link RouteNative} reads the native one.
 * Plain Java (host-tested).
 */
public final class RouteTrace {
    private RouteTrace() {}

    /** Route numbers in a record. */
    public static final int ROUTE_JAVA = 0, ROUTE_AMIDI = 1, ROUTE_USB = 2;

    /** The debug switch. */
    public static volatile boolean on;

    public static final int CAPACITY = 2048;           // records, power of two
    private static final int FIELDS = 5;         // route, startNs, durationNs, bytes, result
    private static final long[] ring = new long[CAPACITY * FIELDS];
    private static final AtomicLong next = new AtomicLong();

    /** Records one write (call only when {@link #on}). {@code result} is what the call returned (bytes sent, or a negative error). */
    public static void record(int route, long startNs, long durationNs, int bytes, long result) {
        int slot = (int) (next.getAndIncrement() & (CAPACITY - 1)) * FIELDS;
        ring[slot] = route;
        ring[slot + 1] = startNs;
        ring[slot + 2] = durationNs;
        ring[slot + 3] = bytes;
        ring[slot + 4] = result;
    }

    public static void clear() { next.set(0); }

    public static long count() { return next.get(); }

    /** One record of the ring, oldest first, as {route, startNs, durationNs, bytes, result}. */
    public static List<long[]> snapshot() {
        long n = next.get();
        long first = Math.max(0, n - CAPACITY);
        List<long[]> list = new ArrayList<long[]>();
        for (long i = first; i < n; i++) {
            int slot = (int) (i & (CAPACITY - 1)) * FIELDS;
            list.add(new long[] {ring[slot], ring[slot + 1], ring[slot + 2], ring[slot + 3], ring[slot + 4]});
        }
        return list;
    }

    public interface Sink { void line(String text); }

    /**
     * Writes the lines for {@code records} (the Java ring, plus the native ring when given: same layout) and a summary.
     * Times are milliseconds from the first record; durations are microseconds.
     */
    public static void dump(List<long[]> records, Sink sink) {
        if (records.isEmpty()) { sink.line("no writes recorded"); return; }
        long base = records.get(0)[1];
        long totalBytes = 0, maxDur = 0, sumDur = 0, failures = 0;
        long[] durations = new long[records.size()];
        int i = 0;
        for (long[] r : records) {
            long dur = r[2];
            durations[i++] = dur;
            totalBytes += r[3];
            sumDur += dur;
            if (dur > maxDur) maxDur = dur;
            if (r[4] < 0) failures++;
            sink.line(routeName((int) r[0]) + " t=" + ((r[1] - base) / 1000000L) + "ms dur=" + (dur / 1000L) + "us bytes=" + r[3]
                + " ret=" + r[4]);
        }
        java.util.Arrays.sort(durations);
        long p99 = durations[Math.min(durations.length - 1, (int) (durations.length * 0.99))];
        sink.line("summary: " + records.size() + " writes, " + totalBytes + " bytes, mean " + (sumDur / records.size() / 1000L)
            + " us, p99 " + (p99 / 1000L) + " us, max " + (maxDur / 1000L) + " us, failed " + failures);
    }

    static String routeName(int route) {
        switch (route) {
            case ROUTE_JAVA: return "java";
            case ROUTE_AMIDI: return "amidi";
            case ROUTE_USB: return "usb";
            default: return "route" + route;
        }
    }

}
