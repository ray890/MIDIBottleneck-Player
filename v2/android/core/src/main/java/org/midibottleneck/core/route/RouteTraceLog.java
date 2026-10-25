package org.midibottleneck.core.route;

import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * The Android side of {@link RouteTrace}: the debug switch from a marker file and the dump to logcat with tag
 * {@code MIDIBN-route} ({@code adb logcat -s MIDIBN-route}). Both rings are merged by time: the Java ring (direct USB
 * route) and the native one (the Android MIDI service routes, written by BatchingOutput).
 */
public final class RouteTraceLog {
    private RouteTraceLog() {}

    /** Turns tracing on or off, in both rings. */
    public static void setEnabled(boolean on) {
        RouteTrace.on = on;
        RouteNative.traceEnable(on);
        if (on) {
            RouteTrace.clear();
            Log.i("MIDIBN-route", "route trace is on");
        }
    }

    /** Writes everything recorded so far to logcat and a one-line summary. */
    public static void dump(String reason) {
        List<long[]> all = new ArrayList<long[]>(RouteTrace.snapshot());
        List<long[]> nativeRecords = RouteNative.traceSnapshot();
        if (nativeRecords != null) all.addAll(nativeRecords);
        Collections.sort(all, new Comparator<long[]>() {
            @Override public int compare(long[] a, long[] b) { return a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0; }
        });
        Log.i("MIDIBN-route", "dump (" + reason + ")");
        RouteTrace.dump(all, new RouteTrace.Sink() {
            @Override public void line(String text) { Log.i("MIDIBN-route", text); }
        });
    }
}
