import org.midibottleneck.core.route.FakeMidiRoute;
import org.midibottleneck.core.route.MidiRoute;
import org.midibottleneck.player.RouteCatalog;

import java.util.ArrayList;
import java.util.List;

/**
 * Tests of the player's output catalog (RouteCatalog) on a plain JVM: which routes are listed and in which order, the
 * device ids behind the list positions, open (attach), open ahead of Play, a changed list, a lost device. The native half
 * (RoutedOutput: sends reach the target with no JNI) is in platform/android/route/tests/route_tests.cpp.
 * Run through RouteHostTest.main.
 */
final class CatalogHostTest {
    static int checks, failures;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    /** A route with a settable name list, a hardware flag and a log of what was called. */
    static final class TestRoute implements MidiRoute {
        final String routeName;
        final boolean hardware;
        final boolean available;
        String[] names;
        final List<String> log = new ArrayList<String>();
        boolean open, lost;
        int openIndex = -1;
        String openProblem;
        Listener listener;

        TestRoute(String routeName, boolean hardware, boolean available, String... names) {
            this.routeName = routeName;
            this.hardware = hardware;
            this.available = available;
            this.names = names;
        }

        @Override public String name() { return routeName; }
        @Override public boolean isAvailable() { return available; }
        @Override public boolean isHardware() { return hardware; }
        @Override public String[] listDevices() { return names.clone(); }
        @Override public String open(int index) {
            log.add("open " + index);
            if (openProblem != null) return openProblem;
            open = true;
            lost = false;
            openIndex = index;
            return null;
        }
        @Override public void close() { log.add("close"); open = false; }
        @Override public boolean isOpen() { return open; }
        @Override public boolean isLost() { return lost; }
        @Override public void setListener(Listener l) { listener = l; }
        @Override public boolean sendShort(int packed) { return open; }
        @Override public boolean sendLong(byte[] data, int offset, int length) { return open; }
        @Override public void beginBatch() {}
        @Override public void endBatch() {}
        @Override public void panic() {}
        void unplug() {
            lost = true;
            open = false;
            if (listener != null) listener.onDeviceLost("The MIDI device was unplugged. Choose the output again.");
        }
    }

    /** Stand-in for MidiRoutes.preferred: the last available hardware route. */
    static final RouteCatalog.Picker LAST = new RouteCatalog.Picker() {
        @Override public MidiRoute pick(MidiRoute[] hardware) { return hardware[hardware.length - 1]; }
    };

    static void testChoose() {
        TestRoute java = new TestRoute("Java", true, true, "Keyboard");
        TestRoute nativeRoute = new TestRoute("Native", true, true, "Keyboard");
        TestRoute synth = new TestRoute("Synth", false, true, "Built-in synthesizer");
        TestRoute off = new TestRoute("Unavailable", true, false, "Ghost");
        MidiRoute[] chosen = RouteCatalog.choose(new MidiRoute[] {java, nativeRoute, synth}, LAST);
        check(chosen.length == 2 && chosen[0] == nativeRoute && chosen[1] == synth, "preferred hardware route, then the synth");
        chosen = RouteCatalog.choose(new MidiRoute[] {java, synth}, LAST);
        check(chosen.length == 2 && chosen[0] == java && chosen[1] == synth, "android6 flavor: Java route, then the synth");
        chosen = RouteCatalog.choose(new MidiRoute[] {java, nativeRoute}, LAST);
        check(chosen.length == 1 && chosen[0] == nativeRoute, "a device is never listed twice");
        chosen = RouteCatalog.choose(new MidiRoute[] {off, synth}, LAST);
        check(chosen.length == 1 && chosen[0] == synth, "an unavailable hardware route is left out");
        chosen = RouteCatalog.choose(new MidiRoute[] {off}, LAST);
        check(chosen.length == 0, "no usable route: the list shows None only");
        check(new FakeMidiRoute().isHardware(), "the default of isHardware is true");
        // a hardware route listed after the synth is still first
        chosen = RouteCatalog.choose(new MidiRoute[] {synth, java}, LAST);
        check(chosen.length == 2 && chosen[0] == java && chosen[1] == synth, "hardware first whatever the order of available()");
    }

    static void testListAndResolve() {
        TestRoute hw = new TestRoute("Native", true, true, "Piano", "Module - port 2");
        TestRoute synth = new TestRoute("Synth", false, true, "Built-in synthesizer");
        RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {hw, synth});
        check(catalog.routeCount() == 2, "route count");
        check(catalog.list(0).length == 2 && catalog.list(0)[1].equals("Module - port 2"), "names as the route gives them");
        check(catalog.list(1).length == 1 && catalog.list(1)[0].equals("Built-in synthesizer"), "synth list");
        check(catalog.list(7).length == 0, "unknown route lists nothing");
        check(catalog.resolve(0) == null, "position 0 is None");
        RouteCatalog.Target t = catalog.resolve(1);
        check(t != null && t.route == 0 && t.index == 0 && t.name.equals("Piano") && t.hardware, "position 1");
        t = catalog.resolve(2);
        check(t != null && t.route == 0 && t.index == 1 && t.name.equals("Module - port 2"), "position 2");
        t = catalog.resolve(3);
        check(t != null && t.route == 1 && t.index == 0 && !t.hardware, "position 3 is the first device of the synth route");
        check(catalog.resolve(4) == null, "past the end");
    }

    static void testOpen() {
        TestRoute hw = new TestRoute("Native", true, true, "Piano", "Module");
        TestRoute synth = new TestRoute("Synth", false, true, "Built-in synthesizer");
        RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {hw, synth});
        catalog.list(0);
        catalog.list(1);
        check(catalog.open(0, 1, "Module") == null && hw.open && hw.openIndex == 1, "open attaches the route's device");
        check(catalog.isOpen(0), "catalog knows the open route");
        int opens = countOpens(hw);
        check(catalog.open(0, 1, "Module") == null && countOpens(hw) == opens, "open of the device that is already open (warm-up) does not open again");
        check(catalog.open(1, 0, "Built-in synthesizer") == null && synth.open && !hw.open, "switching route closes the other route");
        check(catalog.open(0, 0, "Piano") == null && hw.open && hw.openIndex == 0 && !synth.open, "and back");
        catalog.close(0);
        check(!hw.open && !catalog.isOpen(0), "close");
        // the list changed after the player listed it: found again by name
        hw.names = new String[] {"Drum pad", "Piano", "Module"};
        check(catalog.open(0, 0, "Piano") == null && hw.openIndex == 1, "a changed list: the device is found by name");
        hw.names = new String[] {"Drum pad"};
        String gone = catalog.open(0, 1, "Module");
        check(gone != null && gone.contains("Module") && gone.contains("no longer available"), "a vanished device: plain message");
        check(!catalog.isOpen(0) || hw.open, "state consistent after the failed open");
        hw.names = new String[] {"Drum pad", "Module"};
        hw.openProblem = "The MIDI device is in use by another app.";
        check("The MIDI device is in use by another app.".equals(catalog.open(0, 1, "Module")), "the route's own error text is passed on");
        hw.openProblem = null;
        catalog.closeAll();
        check(!hw.open && !synth.open, "closeAll");
        check(catalog.open(9, 0, "x") != null, "unknown route");
    }

    static int countOpens(TestRoute r) {
        int n = 0;
        for (String s : r.log) if (s.startsWith("open")) n++;
        return n;
    }

    static void testEvents() {
        TestRoute hw = new TestRoute("Native", true, true, "Piano");
        RouteCatalog catalog = new RouteCatalog(new MidiRoute[] {hw});
        final List<String> events = new ArrayList<String>();
        catalog.setEvents(new RouteCatalog.Events() {
            @Override public void onDevicesChanged() { events.add("changed"); }
            @Override public void onDeviceLost(String message) { events.add("lost:" + message); }
        });
        hw.listener.onDevicesChanged();
        check(events.size() == 1 && events.get(0).equals("changed"), "device change reaches the player");
        catalog.list(0);
        check(catalog.open(0, 0, "Piano") == null && catalog.isOpen(0), "open before unplugging");
        hw.unplug();
        check(events.size() == 2 && events.get(1).startsWith("lost:The MIDI device was unplugged"), "lost device reaches the player with a plain message");
        check(!catalog.isOpen(0), "a lost route is no longer open in the catalog");
        // after the rescan removed the device, Play must not reuse the dead handle
        check(catalog.open(0, 0, "Piano") == null && hw.open, "a lost device is opened afresh, not reused");
        check(countOpens(hw) == 2, "two real opens in total");
        catalog.setEvents(null);
        hw.listener.onDevicesChanged();
        hw.unplug();   // no listener set: nothing throws
        check(true, "no events object: nothing throws");
    }

    static void run() {
        testChoose();
        testListAndResolve();
        testOpen();
        testEvents();
    }
}
