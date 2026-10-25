import org.midibottleneck.core.route.FakeMidiRoute;
import org.midibottleneck.core.route.MidiBatcher;
import org.midibottleneck.core.route.MidiCodec;
import org.midibottleneck.core.route.RouteBench;
import org.midibottleneck.core.route.RouteDevice;
import org.midibottleneck.core.route.ByteSink;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Plain-Java test (no JUnit): run with main(). Covers the Java-side encoding and batching logic of the routes. */
public final class RouteHostTest {
    static int checks, failures;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    static int pack(int s, int a, int b) { return s | (a << 8) | (b << 16); }

    static final class Recorder implements ByteSink {
        final List<byte[]> writes = new ArrayList<byte[]>();
        int failFrom = -1;
        public boolean write(byte[] d, int off, int len) {
            if (failFrom >= 0 && writes.size() >= failFrom) return false;
            writes.add(Arrays.copyOfRange(d, off, off + len));
            return true;
        }
        public String lastError() { return "Recorder: device gone."; }
        byte[] all() {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            for (byte[] w : writes) o.write(w, 0, w.length);
            return o.toByteArray();
        }
    }

    static void testCodec() {
        byte[] b = new byte[3];
        check(MidiCodec.encodeShort(pack(0x90, 60, 100), b, 0) == 3 && b[0] == (byte) 0x90 && b[1] == 60 && b[2] == 100, "note on");
        check(MidiCodec.encodeShort(pack(0xC2, 5, 99), b, 0) == 2 && b[1] == 5, "program change 2 bytes");
        check(MidiCodec.encodeShort(pack(0xD2, 77, 99), b, 0) == 2, "channel pressure 2 bytes");
        check(MidiCodec.encodeShort(pack(0xE0, 0xFF, 0xFF), b, 0) == 3 && b[1] == 0x7F && b[2] == 0x7F, "pitch bend masks to 7 bits");
        check(MidiCodec.encodeShort(0xF8, b, 0) == 1, "clock 1 byte");
        check(MidiCodec.encodeShort(pack(0xF2, 1, 2), b, 0) == 3, "song position 3 bytes");
        check(MidiCodec.encodeShort(pack(0xF3, 1, 2), b, 0) == 2, "song select 2 bytes");
        for (int s : new int[] {0x00, 0x7F, 0xF0, 0xF4, 0xF5, 0xF7, 0xF9, 0xFD})
            check(MidiCodec.encodeShort(s | 0x100, b, 0) == 0, "rejected status " + s);
        byte[] p = MidiCodec.panicBytes();
        check(p.length == 144, "panic length");
        check(p[0] == (byte) 0xB0 && p[1] == 120 && p[2] == 0 && p[3] == (byte) 0xB0 && p[4] == 123 && p[7] == 64, "panic order");
        check(p[141] == (byte) 0xBF && p[142] == 64 && p[143] == 0, "panic last channel");
        check(MidiCodec.typeName(1).equals("USB") && MidiCodec.typeName(2).equals("Virtual")
            && MidiCodec.typeName(3).equals("Bluetooth") && MidiCodec.typeName(9).startsWith("Other"), "type names");
    }

    static void testBatcher() {
        Recorder r = new Recorder();
        MidiBatcher m = new MidiBatcher(r);
        for (int i = 0; i < 5; i++) check(m.sendShort(pack(0x90, i, 64)), "unbatched send");
        check(r.writes.size() == 5, "one write per unbatched message");

        r = new Recorder();
        m = new MidiBatcher(r);
        m.beginBatch();
        for (int i = 0; i < 100; i++) m.sendShort(pack(0x90, i & 127, 64));
        check(r.writes.isEmpty() && m.pendingBytes() == 300, "nothing written inside a burst");
        check(m.endBatch(), "endBatch ok");
        check(r.writes.size() == 1 && r.writes.get(0).length == 300, "one write for the burst");
        m.beginBatch();
        check(m.endBatch() && r.writes.size() == 1, "empty burst writes nothing");

        // big burst: split at message boundaries, nothing lost
        r = new Recorder();
        m = new MidiBatcher(r);
        ByteArrayOutputStream expected = new ByteArrayOutputStream();
        m.beginBatch();
        byte[] tmp = new byte[3];
        for (int i = 0; i < 2000; i++) {
            int msg = (i % 3 == 0) ? pack(0xC0, i & 127, 0) : pack(0x90, i & 127, (i * 7) & 127);
            int n = MidiCodec.encodeShort(msg, tmp, 0);
            expected.write(tmp, 0, n);
            m.sendShort(msg);
        }
        m.endBatch();
        check(r.writes.size() >= 2, "big burst split");
        boolean boundaries = true;
        for (byte[] w : r.writes) boundaries &= w.length <= MidiCodec.MAX_WRITE && ((w[0] & 0xFF) == 0xC0 || (w[0] & 0xFF) == 0x90);
        check(boundaries, "writes start at message boundaries and fit");
        check(Arrays.equals(r.all(), expected.toByteArray()), "big burst content intact");

        // SysEx
        r = new Recorder();
        m = new MidiBatcher(r);
        m.beginBatch();
        m.sendShort(pack(0x90, 60, 64));
        check(m.sendLong(new byte[] {(byte) 0xF0, 1, 2, 3}, 0, 4) && r.writes.isEmpty(), "unfinished SysEx not sent");
        check(m.sendLong(new byte[] {4, 5, (byte) 0xF7}, 0, 3), "SysEx completed");
        check(r.writes.size() == 2 && r.writes.get(0).length == 3, "short message goes out first");
        check(Arrays.equals(r.writes.get(1), new byte[] {(byte) 0xF0, 1, 2, 3, 4, 5, (byte) 0xF7}), "SysEx assembled");
        m.endBatch();
        check(m.sendLong(new byte[0], 0, 0), "zero length ignored");
        int before = r.writes.size();
        check(m.sendLong(new byte[] {1, 2, (byte) 0xF7}, 0, 3) && r.writes.size() == before, "tail without start ignored");
        byte[] big = new byte[2500];
        Arrays.fill(big, (byte) 0x55);
        big[0] = (byte) 0xF0;
        big[big.length - 1] = (byte) 0xF7;
        before = r.writes.size();
        check(m.sendLong(big, 0, big.length) && r.writes.size() - before == 3, "long SysEx in 3 writes");
        ByteArrayOutputStream joined = new ByteArrayOutputStream();
        for (int i = before; i < r.writes.size(); i++) joined.write(r.writes.get(i), 0, r.writes.get(i).length);
        check(Arrays.equals(joined.toByteArray(), big), "long SysEx intact");

        // reset and panic
        r = new Recorder();
        m = new MidiBatcher(r);
        m.beginBatch();
        m.sendShort(pack(0x90, 60, 64));
        m.sendLong(new byte[] {(byte) 0xF0, 1}, 0, 2);
        m.reset();
        check(m.pendingBytes() == 0 && r.writes.isEmpty(), "reset drops buffer");
        check(!m.sendLong(new byte[] {2, (byte) 0xF7}, 0, 2) || r.writes.isEmpty(), "reset dropped unfinished SysEx");
        check(m.panic() && r.writes.size() == 1 && r.writes.get(0).length == 144, "panic is one 144 byte write");

        // failures: sticky, same text, cleared by clear()
        r = new Recorder();
        r.failFrom = 1;
        m = new MidiBatcher(r);
        check(m.sendShort(pack(0x90, 60, 64)), "first write ok");
        m.beginBatch();
        m.sendShort(pack(0x90, 61, 64));
        check(!m.endBatch(), "burst write fails");
        check(!m.sendShort(pack(0x90, 62, 64)) && "Recorder: device gone.".equals(m.failure()), "failure is sticky with text");
        check(!m.panic(), "panic fails too");
        check(r.writes.size() == 1, "no further attempts on a dead device");
        m.clear();
        r.failFrom = -1;
        check(m.sendShort(pack(0x90, 62, 64)), "clear() forgets the failure");
        m.markLost("unplugged");
        check(!m.sendShort(pack(0x90, 62, 64)) && "unplugged".equals(m.failure()), "markLost");
        check(!new MidiBatcher(r).sendShort(0x40), "invalid message rejected");
    }

    static void testFakeRoute() {
        FakeMidiRoute f = new FakeMidiRoute();
        check(f.isAvailable() && f.listDevices().length == 2 && f.describeDevices().length == 2, "fake devices");
        check(!f.sendShort(pack(0x90, 60, 64)) && f.lastError() != null, "send before open fails with a text");
        check(f.open(0) == null && f.isOpen(), "open");
        check(f.open(7) != null, "bad index");
        check(f.open(1) == null, "open second");
        f.beginBatch();
        f.sendShort(pack(0x90, 60, 64));
        f.sendShort(pack(0x80, 60, 0));
        f.endBatch();
        check(f.writes.size() == 1 && f.writes.get(0).length == 6, "fake: one write per burst");
        final String[] lost = new String[1];
        f.setListener(new org.midibottleneck.core.route.MidiRoute.Listener() {
            public void onDevicesChanged() { }
            public void onDeviceLost(String m) { lost[0] = m; }
        });
        f.unplug();
        check(lost[0] != null && f.isLost(), "listener told about unplug");
        check(!f.sendShort(pack(0x90, 60, 64)) && f.lastError().contains("unplugged"), "send after unplug fails cleanly");
        f.panic();
        f.close();
        check(!f.isOpen(), "close");
        f.open(0);
        check(!f.isLost() && f.sendShort(pack(0x90, 60, 64)), "reopen works");
        RouteDevice d = f.describeDevices()[1];
        check(d.displayName().equals("Fake module - Port 2") && d.details().contains("USB") && d.details().contains("id 102"), "device details: " + d.details());
    }

    static void testBench() {
        FakeMidiRoute f = new FakeMidiRoute();
        f.open(0);
        RouteBench.Result r = RouteBench.runDirect(f, 50, 200, false);
        check(r.sent == 10000 && r.failed == 0 && r.error == null, "bench sent everything");
        check(f.writes.size() == 50, "bench: one write per burst");
        check(r.medianBurstNs <= r.p99BurstNs && r.p99BurstNs <= r.maxBurstNs && r.minBurstNs <= r.medianBurstNs, "bench ordering");
        check(r.summary().contains("10000 messages"), "bench summary");
        RouteBench.Result audible = RouteBench.runDirect(f, 2, 10, true);
        check(audible.sent == 20, "audible bench");
        f.unplug();
        RouteBench.Result bad = RouteBench.runDirect(f, 2, 10, false);
        check(bad.sent == 0 && bad.failed == 20 && bad.error != null, "bench on a lost device reports failure");
        f.close();
        check(RouteBench.run(f, 1, 1, false).error != null, "engine bench needs an open route");
    }

    public static void main(String[] args) {
        testCodec();
        testBatcher();
        testFakeRoute();
        testBench();
        CatalogHostTest.run();
        checks += CatalogHostTest.checks;
        failures += CatalogHostTest.failures;
        PacingHostTest.run();
        checks += PacingHostTest.checks;
        failures += PacingHostTest.failures;
        if (failures == 0) {
            System.out.println("RESULT: PASS (" + checks + " checks)");
        } else {
            System.out.println("RESULT: FAIL (" + failures + " of " + checks + " checks)");
            System.exit(1);
        }
    }
}
