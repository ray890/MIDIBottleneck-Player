package org.midibottleneck.core.route;

import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Sends MIDI bytes to a USB MIDI device: encodes them to USB-MIDI event packets ({@link UsbMidiPackets}), queues the
 * packets and sends them from a dedicated thread in bulk transfers of at most {@code maxTransfer} bytes (the endpoint's
 * packet size, normally 32 for a full-speed device).
 *
 * Honest timing: a bulk transfer returns when the device has taken the data, so a slow device slows the sender; when the
 * queue is full the caller of {@link #send} waits for room, and that wait is real lag, not hidden. Nothing waits for ever:
 * the bulk transfer has a timeout, the wait for room has a limit, closing waits a bounded time. When the device stops
 * accepting data (a transfer times out or fails, or no room for {@code stuckMs}) the sender is failed: {@link #send}
 * returns false at once from then on, {@link #failure()} says why in a plain sentence, and the event is reported once.
 * Plain Java (host-tested with a fake transport that blocks).
 */
public final class UsbMidiSender {
    /** Called once, on the sender or the sending thread, when the device stopped working. */
    public interface Events {
        void onFailed(String message);
    }

    private final UsbTransport transport;
    private final int maxTransfer;
    private final int timeoutMs;
    private final long stuckNs;
    private final Events events;
    private final UsbMidiPackets packets;
    private final byte[] queue;
    private int head, tail, size;
    private final ReentrantLock lock = new ReentrantLock();
    private final Condition notEmpty = lock.newCondition();
    private final Condition notFull = lock.newCondition();
    private final Condition drained = lock.newCondition();
    private boolean closing, stop;
    private volatile String failure;
    private byte[] scratch = new byte[256];
    private final Thread thread;
    private volatile long transfers, bytesSent, maxTransferNs;

    /**
     * @param maxTransfer bytes per bulk transfer (a multiple of 4; the endpoint's wMaxPacketSize)
     * @param timeoutMs   bulk transfer timeout (finite, for example 1000)
     * @param queueBytes  queue size in bytes (multiple of 4)
     * @param stuckMs     longest wait of {@link #send} for room in the queue before the device counts as stuck
     */
    public UsbMidiSender(UsbTransport transport, int cable, int maxTransfer, int timeoutMs, int queueBytes, int stuckMs, Events events) {
        this.transport = transport;
        this.maxTransfer = Math.max(4, maxTransfer & ~3);
        this.timeoutMs = timeoutMs;
        this.stuckNs = stuckMs * 1000000L;
        this.events = events;
        this.packets = new UsbMidiPackets(cable);
        this.queue = new byte[Math.max(this.maxTransfer, queueBytes & ~3)];
        this.thread = new Thread(new Runnable() {
            @Override public void run() { loop(); }
        }, "midibn-usb-send");
        this.thread.setDaemon(true);
        this.thread.start();
    }

    public boolean isFailed() { return failure != null; }

    /** Plain sentence for the failure, or null. */
    public String failure() { return failure; }

    public long transfers() { return transfers; }

    public long bytesSent() { return bytesSent; }

    public long malformed() { return packets.malformed(); }

    /** Longest single transfer so far, in nanoseconds (time inside the bulk call). */
    public long maxTransferNs() { return maxTransferNs; }

    /** Queues bytes for the device. False when the sender has failed (the reason is {@link #failure()}). One thread at a time. */
    public boolean send(byte[] data, int offset, int length) {
        if (failure != null) return false;
        if (length <= 0) return true;
        int need = UsbMidiPackets.maxOutput(length);
        if (scratch.length < need) scratch = new byte[need];
        int n = packets.encode(data, offset, length, scratch, 0);
        int done = 0;
        long deadline = System.nanoTime() + stuckNs;
        lock.lock();
        try {
            while (done < n) {
                if (failure != null || closing) return false;
                int room = queue.length - size;
                if (room < 4) {
                    long left = deadline - System.nanoTime();
                    if (left <= 0) {
                        fail("The USB MIDI device stopped accepting data. Check the cable, or unplug it and plug it in again.",
                            "no room in the send queue for " + (stuckNs / 1000000L) + " ms; " + size + " bytes waiting");
                        return false;
                    }
                    try {
                        notFull.awaitNanos(left);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return false;
                    }
                    continue;
                }
                int chunk = Math.min(n - done, room & ~3);
                for (int i = 0; i < chunk; i++) {
                    queue[tail] = scratch[done + i];
                    tail = (tail + 1) % queue.length;
                }
                size += chunk;
                done += chunk;
                deadline = System.nanoTime() + stuckNs;   // progress: the limit restarts
                notEmpty.signal();
            }
            return true;
        } finally {
            lock.unlock();
        }
    }

    /** The core's reset: drops queued packets and the unfinished message. */
    public void discardPending() {
        lock.lock();
        try {
            head = tail = size = 0;
            packets.reset();
            notFull.signalAll();
        } finally {
            lock.unlock();
        }
    }

    /**
     * Lets the queue drain for at most {@code drainMs} (so the last messages, such as the panic, reach the device), stops
     * the sender thread and releases the transport on a detached thread. Returns after at most about
     * {@code drainMs} plus a short join; never waits for a stuck transfer.
     */
    public void close(long drainMs) {
        lock.lock();
        try {
            closing = true;
            long left = drainMs * 1000000L;
            while (size > 0 && failure == null && left > 0) {
                try {
                    left = drained.awaitNanos(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            stop = true;
            notEmpty.signalAll();
            notFull.signalAll();
        } finally {
            lock.unlock();
        }
        try {
            thread.join(150);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        // The transport is released after the sender thread has ended (a transfer in progress ends by its timeout), never here.
        BoundedCall.detached("midibn-usb-release", new Runnable() {
            @Override public void run() {
                try { thread.join(timeoutMs * 3L + 1000L); } catch (InterruptedException ignored) { }
                try { transport.close(); } catch (RuntimeException ignored) { }
            }
        });
    }

    private void fail(String sentence, String details) {
        boolean first;
        lock.lock();
        try {
            first = failure == null;
            if (first) failure = sentence + "\n\nDetails: " + details;
            head = tail = size = 0;
            notFull.signalAll();
            notEmpty.signalAll();
            drained.signalAll();
        } finally {
            lock.unlock();
        }
        if (first && events != null) {
            try { events.onFailed(failure); } catch (RuntimeException ignored) { }
        }
    }

    private void loop() {
        byte[] buf = new byte[maxTransfer];
        while (true) {
            int len;
            lock.lock();
            try {
                while (size == 0 && !stop) {
                    drained.signalAll();
                    notEmpty.awaitUninterruptibly();
                }
                if (size == 0) return;   // stopped and empty
                len = Math.min(size, maxTransfer) & ~3;
                for (int i = 0; i < len; i++) {
                    buf[i] = queue[head];
                    head = (head + 1) % queue.length;
                }
                size -= len;
                notFull.signalAll();
            } finally {
                lock.unlock();
            }
            int sent = 0;
            while (sent < len) {
                long t0 = System.nanoTime();
                int r = transport.bulkOut(buf, sent, len - sent, timeoutMs);
                long dur = System.nanoTime() - t0;
                transfers++;
                if (dur > maxTransferNs) maxTransferNs = dur;
                if (RouteTrace.on) RouteTrace.record(RouteTrace.ROUTE_USB, t0, dur, len - sent, r);
                if (r > 0) {
                    sent += r;
                    bytesSent += r;
                    continue;
                }
                boolean timedOut = dur >= timeoutMs * 1000000L * 9 / 10;
                fail(timedOut ? "The USB MIDI device stopped accepting data. Check the cable, or unplug it and plug it in again."
                              : "The USB MIDI device did not accept the data. It may have been unplugged. Choose the output again.",
                    "a bulk transfer of " + (len - sent) + " bytes returned " + r + " after " + (dur / 1000000L) + " ms (limit "
                    + timeoutMs + " ms)");
                return;
            }
        }
    }
}
