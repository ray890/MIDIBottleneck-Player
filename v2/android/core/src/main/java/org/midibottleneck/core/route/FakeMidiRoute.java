package org.midibottleneck.core.route;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The stand-in route for tests and for trying the screens without a MIDI device: two pretend devices, records every write.
 * Pure Java (no Android classes). {@link #unplug()} makes the open device disappear the way a pulled USB cable does.
 */
public final class FakeMidiRoute implements MidiRoute, ByteSink {
    public final List<byte[]> writes = new ArrayList<byte[]>();
    private final MidiBatcher batcher = new MidiBatcher(this);
    private boolean open;
    private boolean lost;
    private String error;
    private int discards;
    private Listener listener;
    /** Delay in microseconds added to every write, to imitate a slow device (0 = none). */
    public long writeDelayMicros;

    @Override public String name() { return "Fake route (no device)"; }
    @Override public boolean isAvailable() { return true; }

    @Override public String[] listDevices() {
        return new String[] {"Fake keyboard", "Fake module - port 2"};
    }

    @Override public RouteDevice[] describeDevices() {
        return new RouteDevice[] {
            new RouteDevice(0, 101, "Fake keyboard", "Example Inc.", "Keyboard", "USB", 0, "", 1),
            new RouteDevice(1, 102, "Fake module", "Example Inc.", "Module", "USB", 1, "Port 2", 2),
        };
    }

    @Override public String open(int index) {
        close();
        if (index < 0 || index > 1) return "No such MIDI device.";
        batcher.clear();
        open = true;
        lost = false;
        error = null;
        return null;
    }

    @Override public void close() {
        open = false;
        batcher.reset();
    }

    @Override public boolean isOpen() { return open; }
    @Override public boolean isLost() { return lost; }
    @Override public void setListener(Listener l) { listener = l; }

    @Override public boolean sendShort(int packed) { return ready() && batcher.sendShort(packed); }
    @Override public boolean sendLong(byte[] data, int offset, int length) { return ready() && batcher.sendLong(data, offset, length); }
    @Override public void beginBatch() { batcher.beginBatch(); }
    @Override public void endBatch() { if (open) batcher.endBatch(); }
    @Override public void panic() { if (ready()) batcher.panic(); }
    @Override public void discardPending() { batcher.reset(); discards++; }

    @Override public boolean sendBytes(byte[] data, int offset, int length) {
        return ready() && write(data, offset, length);
    }

    @Override public String lastError() { return error != null ? error : batcher.failure(); }

    /** Pretend the cable was pulled. */
    public void unplug() {
        lost = true;
        error = "The MIDI device was unplugged. Choose the output again.";
        batcher.markLost(error);
        if (listener != null) listener.onDeviceLost(error);
    }

    public int discardCount() { return discards; }
    public long batcherWrites() { return batcher.writeCount(); }

    /** Everything written so far, as one byte string. */
    public byte[] allBytes() {
        ByteArrayOutputStream all = new ByteArrayOutputStream();
        for (byte[] w : writes) all.write(w, 0, w.length);
        return all.toByteArray();
    }

    private boolean ready() {
        if (!open) { error = "No MIDI output device is open."; return false; }
        return true;
    }

    // ByteSink
    @Override public boolean write(byte[] data, int offset, int length) {
        if (lost) return false;
        if (writeDelayMicros > 0) {
            long end = System.nanoTime() + writeDelayMicros * 1000L;
            while (System.nanoTime() < end) { /* busy wait: imitates a device that takes time */ }
        }
        byte[] copy = new byte[length];
        System.arraycopy(data, offset, copy, 0, length);
        writes.add(copy);
        return true;
    }
}
