package org.midibottleneck.core.route;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.os.Build;

/**
 * The native route (AMidi, Android 10 and newer; the "modern" flavor). Java only lists and opens the device (Android
 * hands MIDI devices out through Java only) and gives it to native code, which then sends from C++ with
 * AMidiInputPort_send, one send per burst. The calls below go to the same C++ output the engine uses
 * (BatchingOutput), so what is measured and tested here is the engine's own path.
 */
public final class NativeMidiRoute extends AndroidRouteBase {
    private volatile boolean attached;
    private volatile String error;

    public NativeMidiRoute(Context context) { super(context); }

    @Override public String name() { return "Native MIDI (AMidi)"; }

    @Override public boolean isAvailable() {
        return Build.VERSION.SDK_INT >= 29 && RouteNative.amidiCompiled() && super.isAvailable();
    }

    @Override protected String attach(MidiDevice device, int portNumber) {
        if (!isAvailable()) return "The native MIDI route needs Android 10 or newer.";
        String problem = RouteNative.attachAMidi(device, portNumber);
        if (problem != null) return problem;
        attached = true;
        error = null;
        return null;
    }

    @Override protected void detach() {
        if (attached) {
            attached = false;
            RouteNative.detach();
        }
    }

    @Override protected void deviceGone(String message) {
        markLost(message);
        error = message;
        if (attached) RouteNative.setLost(message);
    }

    @Override public boolean isOpen() { return attached; }

    private boolean result(String problem) {
        if (problem == null) return true;
        error = problem;
        return false;
    }

    @Override public boolean sendShort(int packed) { return attached && result(RouteNative.sendShort(packed)); }

    @Override public boolean sendLong(byte[] data, int offset, int length) {
        return attached && result(RouteNative.sendLong(data, offset, length));
    }

    @Override public void beginBatch() { if (attached) RouteNative.beginBatch(); }

    @Override public void endBatch() { if (attached) RouteNative.endBatch(); }

    @Override public void panic() { if (attached) result(RouteNative.panic()); }

    @Override public void discardPending() { if (attached) result(RouteNative.reset()); }

    @Override public boolean sendBytes(byte[] data, int offset, int length) {
        // Raw bytes: short messages are re-encoded by the native output; used for tests only.
        return false;
    }

    @Override public String lastError() {
        String e = error;
        return e != null ? e : super.lastError();
    }
}
