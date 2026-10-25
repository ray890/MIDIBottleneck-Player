package org.midibottleneck.core.route;

import java.util.Arrays;

/**
 * The Java-side counterpart of the native BatchingOutput: encodes messages, collects the bytes of one burst
 * (beginBatch .. endBatch) and writes them to the {@link ByteSink} with one write. Outside a batch every message is
 * written at once. SysEx follows the core's assembler rule (an F0 piece starts a message, F7 pieces continue it, only a
 * complete F0..F7 message is written). Pure Java, thread safe (all methods synchronized).
 */
public final class MidiBatcher {
    private final ByteSink sink;
    private final byte[] buffer = new byte[MidiCodec.MAX_WRITE + 16];
    private int size;
    private boolean inBatch;
    private String failure;          // sticky until clear()
    private byte[] pending;          // unfinished SysEx
    private int pendingLength;
    private long writes;
    private final byte[] tmp = new byte[3];

    public MidiBatcher(ByteSink sink) {
        this.sink = sink;
    }

    /** Forget a previous failure and everything buffered (a new device was opened). */
    public synchronized void clear() {
        size = 0;
        inBatch = false;
        failure = null;
        pending = null;
        pendingLength = 0;
    }

    /** The device is gone: every later send fails with {@code message}. */
    public synchronized void markLost(String message) {
        if (failure == null) failure = message;
    }

    public synchronized String failure() { return failure; }

    public synchronized long writeCount() { return writes; }

    public synchronized int pendingBytes() { return size; }

    public synchronized void beginBatch() { inBatch = true; }

    public synchronized boolean endBatch() {
        inBatch = false;
        if (failure != null) { size = 0; return false; }
        return flush();
    }

    public synchronized boolean sendShort(int packed) {
        if (failure != null) return false;
        int n = MidiCodec.encodeShort(packed, tmp, 0);
        if (n == 0) return false;   // not a sendable message; no sticky failure
        if (size + n > MidiCodec.MAX_WRITE && !flush()) return false;
        System.arraycopy(tmp, 0, buffer, size, n);
        size += n;
        return inBatch || flush();
    }

    public synchronized boolean sendLong(byte[] data, int offset, int length) {
        if (failure != null) return false;
        if (length <= 0) return true;
        byte[] message = null;
        if ((data[offset] & 0xFF) == 0xF0) {
            pending = Arrays.copyOfRange(data, offset, offset + length);
            pendingLength = length;
        } else if (pending != null) {
            if (pendingLength + length > pending.length) pending = Arrays.copyOf(pending, (pendingLength + length) * 2);
            System.arraycopy(data, offset, pending, pendingLength, length);
            pendingLength += length;
        } else {
            return true;   // a continuation with no start: ignored
        }
        if (pendingLength >= 2 && (pending[pendingLength - 1] & 0xFF) == 0xF7) {
            message = Arrays.copyOf(pending, pendingLength);
            pending = null;
            pendingLength = 0;
        }
        if (message == null) return true;
        if (!flush()) return false;   // keep the order of earlier short messages
        int done = 0;
        while (done < message.length) {
            int count = Math.min(MidiCodec.MAX_WRITE, message.length - done);
            if (!writeNow(message, done, count)) return false;
            done += count;
        }
        return true;
    }

    /** Discards the unsent bytes and any unfinished SysEx (the core's reset). Sends nothing. */
    public synchronized void reset() {
        size = 0;
        inBatch = false;
        pending = null;
        pendingLength = 0;
    }

    /** CC120, CC123, CC64 = 0 on every channel, as one write. */
    public synchronized boolean panic() {
        if (failure != null) return false;
        size = 0;
        inBatch = false;
        byte[] bytes = MidiCodec.panicBytes();
        System.arraycopy(bytes, 0, buffer, 0, bytes.length);
        size = bytes.length;
        return flush();
    }

    private boolean flush() {
        if (size == 0) return true;
        int n = size;
        size = 0;
        return writeNow(buffer, 0, n);
    }

    private boolean writeNow(byte[] data, int offset, int length) {
        writes++;
        if (sink.write(data, offset, length)) return true;
        String reason = sink.lastError();
        failure = reason == null ? "The MIDI device did not accept the messages." : reason;
        return false;
    }
}
