package org.midibottleneck.core.route;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiInputPort;

import java.io.IOException;

/**
 * The Java route (android.media.midi, works from Android 6). Messages are collected in a buffer and written to the
 * MidiInputPort with one send per burst. The native engine reaches this route through RouteNative.attachJava: its
 * output collects a burst in native memory and calls {@link #sendBytes} once (one JNI call, one MidiInputPort.send).
 * Direct Java use (sendShort and so on) goes through a {@link MidiBatcher} with the same rules.
 */
public final class JavaMidiRoute extends AndroidRouteBase implements ByteSink {
    private final MidiBatcher batcher = new MidiBatcher(this);
    private volatile MidiInputPort port;
    private volatile String sinkError;
    private boolean attachedToEngine;

    public JavaMidiRoute(Context context) { super(context); }

    @Override public String name() { return "Java MIDI (android.media.midi)"; }

    @Override protected String attach(MidiDevice device, int portNumber) {
        MidiInputPort p = device.openInputPort(portNumber);
        if (p == null) return "That MIDI port is already in use by another app. Close the other app and try again.";
        batcher.clear();
        sinkError = null;
        port = p;
        if (RouteNative.nativeAvailable()) {
            String problem = RouteNative.attachJava(this);
            if (problem != null) {
                detach();
                return problem;
            }
            attachedToEngine = true;
        }
        return null;
    }

    @Override protected void detach() {
        if (attachedToEngine) {
            attachedToEngine = false;
            RouteNative.detach();
        }
        MidiInputPort p = port;
        port = null;
        batcher.reset();
        sinkError = null;
        if (p != null) {
            try { p.close(); } catch (IOException | RuntimeException ignored) { }
        }
    }

    @Override protected void deviceGone(String message) {
        markLost(message);
        batcher.markLost(message);
        sinkError = message;
        if (attachedToEngine) RouteNative.setLost(message);
        MidiInputPort p = port;
        port = null;
        if (p != null) {
            try { p.close(); } catch (IOException | RuntimeException ignored) { }
        }
    }

    @Override public boolean isOpen() { return port != null; }

    // ---- the engine path: one call per burst ---------------------------------------------------------------------

    @Override public boolean sendBytes(byte[] data, int offset, int length) { return write(data, offset, length); }

    @Override public void discardPending() {
        MidiInputPort p = port;
        batcher.reset();
        if (p == null) return;
        try { p.flush(); } catch (IOException | RuntimeException ignored) { }
    }

    // ---- ByteSink: the one place that touches the port ---------------------------------------------------------

    @Override public boolean write(byte[] data, int offset, int length) {
        MidiInputPort p = port;
        if (p == null) {
            if (sinkError == null) sinkError = "No MIDI output device is open.";
            return false;
        }
        try {
            p.send(data, offset, length);
            return true;
        } catch (IOException | RuntimeException e) {
            String message = "The MIDI device did not accept the messages (" + e.getMessage()
                + "). It may have been unplugged. Choose the output again.";
            sinkError = message;
            markLost(message);
            return false;
        }
    }

    @Override public String lastError() {
        String s = sinkError;
        return s != null ? s : super.lastError();
    }

    // ---- direct Java use ---------------------------------------------------------------------------------------

    @Override public boolean sendShort(int packed) { return port != null && batcher.sendShort(packed); }

    @Override public boolean sendLong(byte[] data, int offset, int length) { return port != null && batcher.sendLong(data, offset, length); }

    @Override public void beginBatch() { batcher.beginBatch(); }

    @Override public void endBatch() { if (port != null) batcher.endBatch(); }

    @Override public void panic() { if (port != null) batcher.panic(); }
}
