package org.midibottleneck.core.route;

import org.midibottleneck.core.NativeCore;

/**
 * JNI entry points of the routes (native side: core/src/main/cpp/route_jni.cpp) plus the "one active route" rule: the
 * native engine sends through exactly one output at a time, so opening a route closes the one that was open.
 */
public final class RouteNative {
    private RouteNative() {}

    private static MidiRoute active;

    /** True when the native library is loaded (false on a plain JVM, in the host tests). */
    public static boolean nativeAvailable() { return NativeCore.load() == null; }

    /** True when this build contains the AMidi route (the "modern" flavor). */
    public static boolean amidiCompiled() { return nativeAvailable() && nativeAmidiCompiled(); }

    /** Attaches a Java route to the native engine's output. Null on success, else an error text. */
    static String attachJava(MidiRoute route) { return nativeAvailable() ? nativeAttachJava(route) : "Native library not loaded."; }

    /** Attaches an opened android.media.midi.MidiDevice to the native (AMidi) route. Null on success, else an error text. */
    static String attachAMidi(Object midiDevice, int portNumber) {
        return nativeAvailable() ? nativeAttachAMidi(midiDevice, portNumber) : "Native library not loaded.";
    }

    static void detach() { if (nativeAvailable()) nativeDetach(); }

    /** True while the native engine has an output attached and open. */
    public static boolean isAttached() { return nativeAvailable() && nativeIsAttached(); }

    static void setLost(String message) { if (nativeAvailable()) nativeSetLost(message); }

    // Messages through the attached native output (what NativeMidiRoute uses). Null = success, else the error text.
    static String sendShort(int packed) { return nativeSendShort(packed); }
    static String sendLong(byte[] data, int offset, int length) { return nativeSendLong(data, offset, length); }
    static void beginBatch() { nativeBeginBatch(); }
    static void endBatch() { nativeEndBatch(); }
    static String reset() { return nativeReset(); }
    static String panic() { return nativePanic(); }

    /**
     * Times the real engine path on the attached route: {@code bursts} bursts of {@code perBurst} short messages, each
     * bracketed by begin/end batch exactly as the engine does. Returns {sent, failed, totalNs, minBurstNs,
     * medianBurstNs, p99BurstNs, maxBurstNs, targetWrites} or null when the native library is missing.
     */
    static long[] bench(int bursts, int perBurst, boolean audible) {
        return nativeAvailable() ? nativeBench(bursts, perBurst, audible) : null;
    }

    static String benchError() { return nativeAvailable() ? nativeBenchError() : "Native library not loaded."; }

    /** Makes {@code route} the only open route: closes the previously active one. */
    static void claim(MidiRoute route) {
        final MidiRoute previous;
        synchronized (RouteNative.class) {
            previous = active;
            active = route;
        }
        // The previous route's close may hang (a stuck system MIDI service): wait for it a bounded time, then go on.
        if (previous != null && previous != route) {
            BoundedCall.run("midibn-route-close", CLOSE_WAIT_MS, new Runnable() {
                @Override public void run() { previous.close(); }
            }, null, null);
        }
    }

    /** Longest wait for another route to close when a route is opened (ms). */
    static final long CLOSE_WAIT_MS = 1500;

    static synchronized void release(MidiRoute route) {
        if (active == route) active = null;
    }

    /** Lets a route that is not an Android MIDI service route (the direct USB route) take part in the one-active-route rule. */
    public static void claimRoute(MidiRoute route) { claim(route); }

    public static void releaseRoute(MidiRoute route) { release(route); }

    /** Write trace of the native side (BatchingOutput): switch. See {@link RouteTrace}. */
    static void traceEnable(boolean on) { if (nativeAvailable()) nativeTraceEnable(on); }

    /** The native ring as records {route, startNs, durationNs, bytes, result}, oldest first; null when not loaded. */
    static java.util.List<long[]> traceSnapshot() {
        if (!nativeAvailable()) return null;
        long[] flat = nativeTraceSnapshot();
        java.util.List<long[]> list = new java.util.ArrayList<long[]>();
        for (int i = 0; flat != null && i + 4 < flat.length; i += 5) {
            list.add(new long[] {flat[i], flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4]});
        }
        return list;
    }

    private static native void nativeTraceEnable(boolean on);
    private static native long[] nativeTraceSnapshot();
    private static native boolean nativeAmidiCompiled();
    private static native String nativeAttachJava(Object route);
    private static native String nativeAttachAMidi(Object midiDevice, int portNumber);
    private static native void nativeDetach();
    private static native boolean nativeIsAttached();
    private static native void nativeSetLost(String message);
    private static native String nativeSendShort(int packed);
    private static native String nativeSendLong(byte[] data, int offset, int length);
    private static native void nativeBeginBatch();
    private static native void nativeEndBatch();
    private static native String nativeReset();
    private static native String nativePanic();
    private static native long[] nativeBench(int bursts, int perBurst, boolean audible);
    private static native String nativeBenchError();
}
