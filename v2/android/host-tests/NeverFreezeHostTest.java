import org.midibottleneck.core.PlayerBridge;
import org.midibottleneck.core.route.BoundedCall;
import org.midibottleneck.core.route.MidiRoute;
import org.midibottleneck.player.OutputOpenerTestAccess;
import org.midibottleneck.player.RouteCatalog;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * P13-USB, part A: nothing on the UI thread may wait for a MIDI device. A fake route that blocks forever (or for a
 * while) on open, on write and on close; the calls the UI thread makes (ready check, request, the catalog handed to the
 * native player, close) must return at once, a device that does not answer must give the plain message with details,
 * and the player must stay usable (another route opens, the stuck route is refused quickly). Plain main, no JUnit.
 */
public final class NeverFreezeHostTest {
    static int checks, failures;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    /** A route whose calls can be made to block (forever, or until released). */
    static final class HangRoute implements MidiRoute {
        final String routeName;
        final boolean hardware;
        volatile boolean hangOpen, hangClose, hangWrite;
        final CountDownLatch release = new CountDownLatch(1);
        volatile boolean open;
        volatile int closes;
        volatile int opens;
        Listener listener;

        HangRoute(String name, boolean hardware) { this.routeName = name; this.hardware = hardware; }

        @Override public String name() { return routeName; }
        @Override public boolean isAvailable() { return true; }
        @Override public boolean isHardware() { return hardware; }
        @Override public String[] listDevices() { return new String[] {"USB2.0-MIDI"}; }
        @Override public String open(int index) {
            opens++;
            if (hangOpen) awaitRelease();
            open = true;
            return null;
        }
        @Override public void close() {
            closes++;
            if (hangClose) awaitRelease();
            open = false;
        }
        @Override public boolean isOpen() { return open; }
        @Override public boolean sendShort(int packed) { return open; }
        @Override public boolean sendLong(byte[] data, int offset, int length) { return open; }
        @Override public boolean sendBytes(byte[] data, int offset, int length) {
            if (hangWrite) awaitRelease();
            return open;
        }
        @Override public void beginBatch() {}
        @Override public void endBatch() {}
        @Override public void panic() {}
        @Override public void setListener(Listener l) { listener = l; }

        void awaitRelease() {
            try { release.await(); } catch (InterruptedException ignored) { }
        }
    }

    static long ms(long startNs) { return (System.nanoTime() - startNs) / 1000000L; }

    static RouteCatalog.Target target(RouteCatalog catalog, int route) {
        catalog.list(0);
        catalog.list(1);
        return catalog.resolve(route == 0 ? 1 : 2);
    }

    static void testOpenNeverReturns() throws Exception {
        HangRoute stuck = new HangRoute("Android MIDI service", true);
        stuck.hangOpen = true;
        HangRoute other = new HangRoute("Direct USB", true);
        final RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {stuck, other});
        catalog.setOpenDeadlineMs(400);
        Object opener = OutputOpenerTestAccess.create(catalog);
        RouteCatalog.Target t = target(catalog, 0);
        check(t != null, "target resolved");

        // UI-thread calls return at once while the open hangs.
        final List<String> results = Collections.synchronizedList(new ArrayList<String>());
        final CountDownLatch done = new CountDownLatch(1);
        long t0 = System.nanoTime();
        OutputOpenerTestAccess.request(opener, t, new OutputOpenerTestAccess.Done() {
            @Override public void done(String problem) { results.add(problem == null ? "ok" : problem); done.countDown(); }
        });
        check(ms(t0) < 100, "request returns at once (" + ms(t0) + " ms)");
        PlayerBridge.OutputCatalog forBridge = OutputOpenerTestAccess.forBridge(opener);
        t0 = System.nanoTime();
        String early = forBridge.open(t.route, t.index, t.name);
        check(ms(t0) < 100 && early != null, "the core's open answers at once with a sentence, not a wait (" + ms(t0) + " ms)");
        t0 = System.nanoTime();
        boolean ready = OutputOpenerTestAccess.isReady(opener, t);
        forBridge.close(t.route);
        check(ms(t0) < 100 && !ready, "ready check and close do not wait");

        // The deadline passes: plain sentence first, details below.
        check(done.await(3, TimeUnit.SECONDS), "the request ends by itself");
        String msg = results.isEmpty() ? "" : results.get(0);
        check(msg.startsWith("The MIDI device did not answer when the player tried to open it."), "plain sentence first: " + msg);
        check(msg.contains("Unplug it and plug it in again, or choose another output."), "advice in the sentence");
        int d = msg.indexOf("\n\nDetails: ");
        check(d > 0 && msg.contains("USB2.0-MIDI") && msg.contains("stuck in:"), "details below, with the device and where it is stuck");
        check(catalog.isStuck(0), "the route is marked stuck");

        // The player stays usable: the stuck route is refused quickly, the other route opens.
        t0 = System.nanoTime();
        String again = catalog.open(0, 0, "USB2.0-MIDI");
        check(ms(t0) < 100 && again != null && again.startsWith("The MIDI device did not answer"), "a stuck route is refused at once (" + ms(t0) + " ms)");
        RouteCatalog.Target t2 = target(catalog, 1);
        t0 = System.nanoTime();
        String r2 = catalog.open(t2.route, t2.index, t2.name);
        check(r2 == null && other.open && ms(t0) < 1000, "another route opens although the first hangs");
        check(OutputOpenerTestAccess.isReady(opener, t2), "and is ready");

        // The hung call finally returns: the late device is closed again, the route is usable again.
        stuck.release.countDown();
        long end = System.nanoTime() + 2000000000L;
        while (catalog.isStuck(0) && System.nanoTime() < end) Thread.sleep(10);
        check(!catalog.isStuck(0), "when the hung call returns the route is no longer stuck");
        end = System.nanoTime() + 2000000000L;
        while (stuck.open && System.nanoTime() < end) Thread.sleep(10);
        check(!stuck.open && stuck.closes >= 1, "a device that opened too late is closed again");
        OutputOpenerTestAccess.shutdown(opener);
    }

    static void testCloseNeverReturns() throws Exception {
        HangRoute stuck = new HangRoute("Android MIDI service", true);
        HangRoute other = new HangRoute("Direct USB", true);
        RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {stuck, other});
        Object opener = OutputOpenerTestAccess.create(catalog);
        RouteCatalog.Target t = target(catalog, 0);
        check(catalog.open(t.route, t.index, t.name) == null, "opens");
        stuck.hangClose = true;
        long t0 = System.nanoTime();
        OutputOpenerTestAccess.forBridge(opener).close(0);
        check(ms(t0) < 100, "close from the UI thread does not wait (" + ms(t0) + " ms)");
        check(!OutputOpenerTestAccess.isReady(opener, t), "and the output counts as closed at once");
        // Output switching still works: the next open waits at most the close deadline, not forever.
        RouteCatalog.Target t2 = target(catalog, 1);
        final CountDownLatch done = new CountDownLatch(1);
        final String[] out = new String[1];
        t0 = System.nanoTime();
        OutputOpenerTestAccess.request(opener, t2, new OutputOpenerTestAccess.Done() {
            @Override public void done(String problem) { out[0] = problem; done.countDown(); }
        });
        check(ms(t0) < 100, "request returns at once");
        check(done.await(6, TimeUnit.SECONDS) && out[0] == null && other.open, "switching output works although the old close hangs (" + ms(t0) + " ms)");
        stuck.release.countDown();
        OutputOpenerTestAccess.shutdown(opener);
    }

    static void testWriteNeverReturns() throws Exception {
        // The engine thread is stuck in a write. Nothing the UI thread does may wait for it.
        HangRoute stuck = new HangRoute("Android MIDI service", true);
        RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {stuck});
        Object opener = OutputOpenerTestAccess.create(catalog);
        RouteCatalog.Target t = target(catalog, 0);
        check(catalog.open(t.route, t.index, t.name) == null, "opens");
        stuck.hangWrite = true;
        Thread engine = new Thread(new Runnable() {
            @Override public void run() { stuck.sendBytes(new byte[] {(byte) 0x90, 60, 100}, 0, 3); }
        }, "fake-engine");
        engine.setDaemon(true);
        engine.start();
        Thread.sleep(100);
        check(engine.isAlive(), "the engine thread is stuck in the write");
        long t0 = System.nanoTime();
        boolean ready = OutputOpenerTestAccess.isReady(opener, t);
        OutputOpenerTestAccess.forBridge(opener).close(0);
        String open = OutputOpenerTestAccess.forBridge(opener).open(t.route, t.index, t.name);
        check(ms(t0) < 100, "ready check, close and open answer at once while a write hangs (" + ms(t0) + " ms)");
        check(ready && open != null, "state is consistent (was ready, closed, reopening)");
        stuck.release.countDown();
        OutputOpenerTestAccess.shutdown(opener);
    }

    static void testBoundedCall() {
        long t0 = System.nanoTime();
        final CountDownLatch never = new CountDownLatch(1);
        BoundedCall.Result r = BoundedCall.run("test-hang", 200, new Runnable() {
            @Override public void run() { try { never.await(); } catch (InterruptedException ignored) { } }
        }, null, null);
        check(!r.finished && ms(t0) >= 190 && ms(t0) < 600, "a hanging call is abandoned after its deadline");
        check(r.where.contains("CountDownLatch") || r.where.contains("await") || r.where.contains("park"), "where the thread is stuck: " + r.where);
        never.countDown();
        BoundedCall.Result quick = BoundedCall.run("test-quick", 1000, new Runnable() { @Override public void run() { } }, null, null);
        check(quick.finished, "a quick call finishes");
    }

    public static void main(String[] args) throws Exception {
        testBoundedCall();
        testOpenNeverReturns();
        testCloseNeverReturns();
        testWriteNeverReturns();
        System.out.println("NeverFreezeHostTest: " + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
