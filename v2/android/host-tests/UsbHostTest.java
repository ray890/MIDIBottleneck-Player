import org.midibottleneck.core.route.RouteTrace;
import org.midibottleneck.core.route.UsbMidiPackets;
import org.midibottleneck.core.route.UsbMidiSender;
import org.midibottleneck.core.route.UsbTransport;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * P13-USB, part B on a plain JVM: the USB-MIDI packet encoding (CIN codes, SysEx cut, real-time inside SysEx, running
 * status, split writes), the sender thread against a fake transport (order, 32-byte transfers, partial transfers, a
 * transfer that times out, a transport that blocks for ever), and the write trace. Plain main.
 */
public final class UsbHostTest {
    static int checks, failures;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static byte[] enc(UsbMidiPackets p, int... bytes) {
        byte[] in = new byte[bytes.length];
        for (int i = 0; i < in.length; i++) in[i] = (byte) bytes[i];
        byte[] out = new byte[UsbMidiPackets.maxOutput(in.length)];
        int n = p.encode(in, 0, in.length, out, 0);
        byte[] r = new byte[n];
        System.arraycopy(out, 0, r, 0, n);
        return r;
    }

    static boolean eq(byte[] actual, int... expected) {
        if (actual.length != expected.length) return false;
        for (int i = 0; i < expected.length; i++) if ((actual[i] & 0xFF) != expected[i]) return false;
        return true;
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(String.format("%02X ", x & 0xFF));
        return s.toString().trim();
    }

    static void testPackets() {
        UsbMidiPackets p = new UsbMidiPackets(0);
        check(eq(enc(p, 0x90, 60, 100), 0x09, 0x90, 60, 100), "note on: CIN 9");
        check(eq(enc(p, 0x80, 60, 0), 0x08, 0x80, 60, 0), "note off: CIN 8");
        check(eq(enc(p, 0xB3, 64, 127), 0x0B, 0xB3, 64, 127), "control change: CIN B on channel 4");
        check(eq(enc(p, 0xC1, 5), 0x0C, 0xC1, 5, 0), "program change: CIN C, two bytes");
        check(eq(enc(p, 0xD0, 33), 0x0D, 0xD0, 33, 0), "channel pressure: CIN D");
        check(eq(enc(p, 0xE0, 0, 64), 0x0E, 0xE0, 0, 64), "pitch bend: CIN E");
        check(eq(enc(p, 0xF1, 7), 0x02, 0xF1, 7, 0), "MTC quarter frame: CIN 2");
        check(eq(enc(p, 0xF2, 1, 2), 0x03, 0xF2, 1, 2), "song position: CIN 3");
        check(eq(enc(p, 0xF3, 9), 0x02, 0xF3, 9, 0), "song select: CIN 2");
        check(eq(enc(p, 0xF6), 0x05, 0xF6, 0, 0), "tune request: CIN 5");
        check(eq(enc(p, 0xF8), 0x0F, 0xF8, 0, 0), "clock: CIN F");
        check(eq(enc(p, 0xFF), 0x0F, 0xFF, 0, 0), "reset: CIN F");
        // two messages in one write
        check(eq(enc(p, 0x90, 60, 100, 0x80, 60, 0), 0x09, 0x90, 60, 100, 0x08, 0x80, 60, 0), "two messages, two packets");
        // running status
        check(eq(enc(p, 0x90, 60, 100, 61, 100), 0x09, 0x90, 60, 100, 0x09, 0x90, 61, 100), "running status");
        // a message split over two writes
        byte[] a = enc(p, 0x90, 70);
        byte[] b = enc(p, 100);
        check(a.length == 0 && eq(b, 0x09, 0x90, 70, 100), "a short message split over two writes: nothing sent until complete");
        // cable number
        UsbMidiPackets c2 = new UsbMidiPackets(2);
        check(eq(enc(c2, 0x90, 60, 100), 0x29, 0x90, 60, 100), "cable 2 in the high nibble");

        // SysEx: F0 + 7 data + F7 = 9 bytes: 4,4,5? -> (F0 01 02)(03 04 05)(06 07 F7)
        UsbMidiPackets s = new UsbMidiPackets(0);
        check(eq(enc(s, 0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7), 0x04, 0xF0, 1, 2, 0x04, 3, 4, 5, 0x07, 6, 7, 0xF7), "SysEx of 9 bytes: CIN 4, 4, 7");
        check(eq(enc(s, 0xF0, 0xF7), 0x06, 0xF0, 0xF7, 0), "empty SysEx F0 F7: CIN 6");
        check(eq(enc(s, 0xF0, 1, 0xF7), 0x07, 0xF0, 1, 0xF7), "F0 01 F7: CIN 7");
        check(eq(enc(s, 0xF0, 1, 2, 0xF7), 0x04, 0xF0, 1, 2, 0x05, 0xF7, 0, 0), "F0 01 02 F7: CIN 4 then 5");
        check(eq(enc(s, 0xF0, 1, 2, 3, 0xF7), 0x04, 0xF0, 1, 2, 0x06, 3, 0xF7, 0), "F0 01 02 03 F7: CIN 4 then 6");
        // SysEx over two writes
        byte[] x = enc(s, 0xF0, 1, 2, 3, 4);
        byte[] y = enc(s, 5, 0xF7);
        check(eq(x, 0x04, 0xF0, 1, 2) && s.pendingSysexBytes() == 0, "first part of a SysEx: complete triplets are sent, the rest waits");
        check(eq(y, 0x04, 3, 4, 5, 0x05, 0xF7, 0, 0), "second part finishes it in order: " + hex(y));
        // real-time inside SysEx
        check(eq(enc(s, 0xF0, 1, 0xF8, 2, 3, 0xF7), 0x0F, 0xF8, 0, 0, 0x04, 0xF0, 1, 2, 0x06, 3, 0xF7, 0), "clock inside SysEx is sent at once: " + hex(enc(new UsbMidiPackets(0), 0xF0, 1, 0xF8, 2, 3, 0xF7)));
        // a real-time byte inside a short message
        UsbMidiPackets r = new UsbMidiPackets(0);
        check(eq(enc(r, 0x90, 0xF8, 60, 100), 0x0F, 0xF8, 0, 0, 0x09, 0x90, 60, 100), "clock inside a note on");
        // stray data byte: counted, not sent
        UsbMidiPackets m = new UsbMidiPackets(0);
        check(enc(m, 33).length == 0 && m.malformed() == 1, "a data byte without status is counted, never sent");
        // a new status ends an open SysEx (counted)
        check(eq(enc(m, 0xF0, 1, 0x90, 60, 100), 0x09, 0x90, 60, 100) && m.malformed() == 2, "a status byte inside SysEx ends it and is counted");
        s.reset();
        check(s.pendingSysexBytes() == 0, "reset");
    }

    /** A transport that records every transfer; can be made slow, partial, failing, or blocking for ever. */
    static final class FakeTransport implements UsbTransport {
        final List<byte[]> transfers = Collections.synchronizedList(new ArrayList<byte[]>());
        volatile boolean blockForever, fail, partial;
        volatile long delayMs;
        final CountDownLatch never = new CountDownLatch(1);
        volatile boolean closed;

        @Override public int bulkOut(byte[] data, int offset, int length, int timeoutMs) {
            if (blockForever) {
                try { never.await(); } catch (InterruptedException ignored) { }
                return -1;
            }
            if (delayMs > 0) {
                try { Thread.sleep(delayMs); } catch (InterruptedException ignored) { }
            }
            if (fail) return -1;
            int n = partial && length > 4 ? length - 4 : length;
            byte[] copy = new byte[n];
            System.arraycopy(data, offset, copy, 0, n);
            transfers.add(copy);
            return n;
        }

        @Override public void close() { closed = true; }

        byte[] all() {
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream();
            synchronized (transfers) { for (byte[] t : transfers) o.write(t, 0, t.length); }
            return o.toByteArray();
        }
    }

    static boolean waitFor(java.util.concurrent.Callable<Boolean> c, long ms) throws Exception {
        long end = System.nanoTime() + ms * 1000000L;
        while (System.nanoTime() < end) {
            if (c.call()) return true;
            Thread.sleep(5);
        }
        return c.call();
    }

    static void testSender() throws Exception {
        final FakeTransport t = new FakeTransport();
        final UsbMidiSender s = new UsbMidiSender(t, 0, 32, 1000, 4096, 500, null);
        byte[] burst = new byte[3 * 40];
        for (int i = 0; i < 40; i++) { burst[i * 3] = (byte) 0x90; burst[i * 3 + 1] = (byte) (i % 100); burst[i * 3 + 2] = 100; }
        check(s.send(burst, 0, burst.length), "40 note ons are accepted");
        check(waitFor(new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return t.all().length == 160; } }, 2000),
            "all 40 packets (160 bytes) reach the device");
        boolean small = true;
        synchronized (t.transfers) { for (byte[] tr : t.transfers) if (tr.length > 32 || tr.length % 4 != 0) small = false; }
        check(small, "no transfer is larger than 32 bytes, always whole packets");
        byte[] all = t.all();
        boolean order = true;
        for (int i = 0; i < 40; i++) if ((all[i * 4 + 2] & 0xFF) != i % 100) order = false;
        check(order, "order kept");
        check(!s.isFailed() && s.malformed() == 0, "no failure");

        // partial transfers continue with the rest
        t.transfers.clear();
        t.partial = true;
        check(s.send(burst, 0, 30), "ten more");
        check(waitFor(new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return t.all().length == 40; } }, 2000),
            "a partial transfer continues with the remaining bytes (nothing lost)");
        t.partial = false;

        // reset drops what is queued
        t.delayMs = 50;
        s.send(burst, 0, burst.length);
        Thread.sleep(20);
        s.discardPending();
        t.delayMs = 0;
        Thread.sleep(150);
        check(t.all().length < 40 + 160, "discardPending drops the queued packets");

        long t0 = System.nanoTime();
        s.close(300);
        check((System.nanoTime() - t0) / 1000000L < 600, "close returns in a bounded time");
        check(waitFor(new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return t.closed; } }, 2000), "the transport is released");
        check(!s.send(burst, 0, 3), "a closed sender refuses sends (never a silent drop)");
    }

    static void testSenderTimeout() throws Exception {
        // A transfer that fails (device gone, timeout): honest error, later sends refused, event once.
        final FakeTransport t = new FakeTransport();
        t.fail = true;
        final List<String> events = Collections.synchronizedList(new ArrayList<String>());
        UsbMidiSender s = new UsbMidiSender(t, 0, 32, 1000, 4096, 500, new UsbMidiSender.Events() {
            @Override public void onFailed(String message) { events.add(message); }
        });
        byte[] one = {(byte) 0x90, 60, 100};
        s.send(one, 0, 3);
        check(waitFor(new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return events.size() == 1; } }, 2000), "failure reported once");
        check(s.isFailed() && !s.send(one, 0, 3), "after a failed transfer sends return false");
        String f = s.failure();
        check(f != null && f.startsWith("The USB MIDI device did not accept the data.") && f.contains("\n\nDetails: "), "plain sentence, then Details: " + f);
        s.close(100);
    }

    static void testSenderBlocksForever() throws Exception {
        // The bulk call never returns (even past its timeout): the sending thread waits at most stuckMs, then gets false.
        FakeTransport t = new FakeTransport();
        t.blockForever = true;
        final List<String> events = Collections.synchronizedList(new ArrayList<String>());
        UsbMidiSender s = new UsbMidiSender(t, 0, 32, 1000, 64, 300, new UsbMidiSender.Events() {
            @Override public void onFailed(String message) { events.add(message); }
        });
        byte[] burst = new byte[3 * 100];
        for (int i = 0; i < 100; i++) { burst[i * 3] = (byte) 0x90; burst[i * 3 + 1] = 60; burst[i * 3 + 2] = 100; }
        long t0 = System.nanoTime();
        boolean ok = true;
        for (int i = 0; i < 20 && ok; i++) ok = s.send(burst, 0, burst.length);
        long ms = (System.nanoTime() - t0) / 1000000L;
        check(!ok, "a device that stops accepting data makes send return false");
        check(ms >= 250 && ms < 1500, "and only after the limit (" + ms + " ms), not for ever");
        check(s.isFailed() && s.failure().startsWith("The USB MIDI device stopped accepting data."), "plain sentence: " + s.failure());
        check(events.size() == 1, "reported once");
        t0 = System.nanoTime();
        s.discardPending();
        s.close(200);
        check((System.nanoTime() - t0) / 1000000L < 800, "close does not wait for the stuck transfer");
        // the stuck transfer stays blocked (daemon thread) so it cannot record into the later trace test
    }

    static void testTrace() {
        RouteTrace.on = false;
        RouteTrace.clear();
        check(RouteTrace.count() == 0, "nothing recorded while off");
        RouteTrace.on = true;
        RouteTrace.record(RouteTrace.ROUTE_USB, 1000000L, 250000L, 32, 32);
        RouteTrace.record(RouteTrace.ROUTE_USB, 2000000L, 1500000L, 8, -1);
        RouteTrace.on = false;
        final List<String> lines = new ArrayList<String>();
        RouteTrace.dump(RouteTrace.snapshot(), new RouteTrace.Sink() {
            @Override public void line(String text) { lines.add(text); }
        });
        check(lines.size() == 3 && lines.get(0).equals("usb t=0ms dur=250us bytes=32 ret=32"), "record line: " + (lines.isEmpty() ? "" : lines.get(0)));
        check(lines.get(2).contains("2 writes") && lines.get(2).contains("failed 1") && lines.get(2).contains("max 1500 us"), "summary: " + lines.get(2));
        for (int i = 0; i < RouteTrace.CAPACITY + 10; i++) { RouteTrace.on = true; RouteTrace.record(0, i, 1, 1, 1); }
        RouteTrace.on = false;
        check(RouteTrace.snapshot().size() == RouteTrace.CAPACITY, "the ring keeps the latest records only");
        RouteTrace.clear();
    }

    public static void main(String[] args) throws Exception {
        testPackets();
        testSender();
        testSenderTimeout();
        testSenderBlocksForever();
        testTrace();
        System.out.println("UsbHostTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
