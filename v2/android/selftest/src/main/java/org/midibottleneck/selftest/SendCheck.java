package org.midibottleneck.selftest;

import org.midibottleneck.core.route.MidiRoute;

import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/**
 * Send check to one MIDI output device, for each output route the build has. Only harmless messages are sent:
 * Control Change 20..31 on all 16 channels (undefined controllers, silent; the same set the desktop send checks use),
 * a silent SysEx (Device Inquiry, F0 7E 7F 06 01 F7: the device may answer, which this check does not read), and at
 * the end the three panic controllers (CC120 all sound off, CC123 all notes off, CC64 = 0 sustain off).
 */
final class SendCheck {
    private SendCheck() {}

    /** Sends one pattern for {@code seconds}; messages are packed CC messages. */
    private static final class Stat {
        long sent, errors;
        double seconds, slowestBurstUs;
        String firstError = "";
        double[] callNs = new double[0];
        double[] lateUs = new double[0];
    }

    private static int packed(int n) {
        return 0xB0 | (n & 15) | ((20 + (n >> 4) % 12) << 8) | (((n * 7) & 127) << 16);
    }

    /**
     * @param deviceName the device the user picked (matched by name in each route's list)
     * @param seconds    length of each of the two throughput patterns
     */
    static void run(Results results, MidiRoute[] routes, String deviceName, int seconds) {
        StringBuilder t = new StringBuilder("SEND CHECK to \"" + deviceName + "\"\n");
        t.append("Messages: Control Change 20..31 on 16 channels (silent), a Device Inquiry SysEx, then panic controllers.\n");
        boolean any = false, failed = false;
        for (MidiRoute route : routes) {
            t.append("\n== Route: ").append(route.name()).append(" ==\n");
            if (!route.isAvailable()) { t.append("not available on this phone\n"); continue; }
            int index = -1;
            String[] names = route.listDevices();
            for (int i = 0; names != null && i < names.length; i++) if (deviceName.equals(names[i])) { index = i; break; }
            if (index < 0) { t.append("this route does not list that device\n"); continue; }
            any = true;
            String error = route.open(index);
            if (error != null) {
                failed = true;
                t.append("open failed: ").append(error).append('\n');
                results.add("Send check, " + route.name(), Results.Status.FAIL, "open failed: " + error);
                continue;
            }
            try {
                Stat single = throughput(route, seconds, false);
                report(t, "Single sends (no batching)", single);
                Stat batched = throughput(route, seconds, true);
                report(t, "Batched sends (256 per batch)", batched);
                Stat paced = paced(route, 3);
                report(t, "Paced 500 messages per second for 3 s", paced);
                t.append(String.format(Locale.US, "  send call time: median %.1f us, 99%% %.1f us, max %.1f us%n",
                        pct(paced.callNs, 0.5) / 1000, pct(paced.callNs, 0.99) / 1000, pct(paced.callNs, 1.0) / 1000));
                t.append(String.format(Locale.US, "  start lateness vs schedule: median %.0f us, 99%% %.0f us, max %.0f us%n",
                        pct(paced.lateUs, 0.5), pct(paced.lateUs, 0.99), pct(paced.lateUs, 1.0)));
                boolean sysex = route.sendLong(new byte[] { (byte) 0xF0, 0x7E, 0x7F, 0x06, 0x01, (byte) 0xF7 }, 0, 6);
                t.append("SysEx (Device Inquiry, 6 bytes): ").append(sysex ? "sent" : "REFUSED").append('\n');
                route.panic();
                t.append("Panic (CC120, CC123, CC64=0 on all channels): sent\n");
                long errors = single.errors + batched.errors + paced.errors + (sysex ? 0 : 1);
                String detail = String.format(Locale.US, "single %.0f msg/s, batched %.0f msg/s, paced send call median %.0f us; %d error(s)",
                        single.sent / single.seconds, batched.sent / batched.seconds, pct(paced.callNs, 0.5) / 1000, errors);
                if (errors == 0) results.add("Send check, " + route.name(), Results.Status.PASS, detail);
                else {
                    failed = true;
                    results.add("Send check, " + route.name(), Results.Status.FAIL, detail + "; first: " + single.firstError + batched.firstError + paced.firstError);
                }
            } catch (Throwable th) {
                failed = true;
                t.append("exception: ").append(th).append('\n');
                results.add("Send check, " + route.name(), Results.Status.FAIL, "exception: " + th);
            } finally {
                try { route.close(); } catch (Throwable ignored) { }
            }
        }
        if (!any && !failed) results.add("Send check", Results.Status.INFO, "no route could reach that device; see send-check.txt");
        results.write("send-check.txt", t.toString());
    }

    private static void report(StringBuilder t, String title, Stat s) {
        t.append('\n').append(title).append('\n');
        t.append(String.format(Locale.US, "  sent %d in %.2f s: %.0f messages per second, %.0f ns per message, slowest 256-message burst %.0f us%n",
                s.sent, s.seconds, s.sent / s.seconds, s.seconds * 1e9 / Math.max(1, s.sent), s.slowestBurstUs));
        t.append("  errors: ").append(s.errors).append(s.errors > 0 ? " (first: " + s.firstError + ")" : "").append('\n');
    }

    private static Stat throughput(MidiRoute route, int seconds, boolean batch) {
        Stat s = new Stat();
        long limit = seconds * 1_000_000_000L, start = System.nanoTime();
        int n = 0;
        for (;;) {
            long burstStart = System.nanoTime();
            if (batch) route.beginBatch();
            for (int i = 0; i < 256; i++, n++) {
                if (!route.sendShort(packed(n))) { if (s.errors++ == 0) s.firstError = "sendShort returned false at message " + n; }
            }
            if (batch) route.endBatch();
            long now = System.nanoTime();
            s.sent += 256;
            s.slowestBurstUs = Math.max(s.slowestBurstUs, (now - burstStart) / 1000.0);
            if (now - start >= limit || s.errors > 1000) { s.seconds = (now - start) / 1e9; break; }
        }
        return s;
    }

    private static Stat paced(MidiRoute route, int seconds) {
        Stat s = new Stat();
        int total = seconds * 500;
        s.callNs = new double[total];
        s.lateUs = new double[total];
        long start = System.nanoTime();
        for (int i = 0; i < total; i++) {
            long due = start + i * 2_000_000L;
            long rest = due - System.nanoTime() - 200_000L;
            if (rest > 0) LockSupport.parkNanos(rest);
            while (System.nanoTime() < due) { /* spin */ }
            long begin = System.nanoTime();
            s.lateUs[i] = (begin - due) / 1000.0;
            boolean ok = route.sendShort(packed(i));
            s.callNs[i] = System.nanoTime() - begin;
            if (!ok && s.errors++ == 0) s.firstError = "sendShort returned false at message " + i;
            s.sent++;
        }
        s.seconds = (System.nanoTime() - start) / 1e9;
        s.slowestBurstUs = pct(s.callNs, 1.0) / 1000.0;
        return s;
    }

    private static double pct(double[] values, double fraction) {
        if (values.length == 0) return 0;
        double[] sorted = values.clone();
        Arrays.sort(sorted);
        return sorted[Math.min(sorted.length - 1, (int) (sorted.length * fraction))];
    }
}
