package org.midibottleneck.player;

import org.midibottleneck.core.PlayerBridge;
import org.midibottleneck.core.route.BoundedCall;
import org.midibottleneck.core.route.MidiRoute;

/**
 * The output devices of the player, across the routes it uses (see {@link PlayerRoutes#choose}): the preferred hardware
 * route first, then every non-hardware route. The native player lists and opens through this class
 * ({@link PlayerBridge.OutputCatalog}); once a device is open, messages go from C++ straight into the route's native
 * output and never come back here.
 *
 * Plain Java (no Android classes) so the host tests run it with {@code FakeMidiRoute}.
 *
 * Threads: open and close block (open up to the 3 s deadline), so the player never hands this class to the native side
 * directly: {@link OutputOpener} wraps it, opens on a worker and answers the UI thread at once. Each route has its own
 * lock, and every call into a route that can hang is bounded ({@link BoundedCall}). The route listeners run on
 * background threads of the routes and take no lock.
 */
public final class RouteCatalog implements PlayerBridge.OutputCatalog {
    /** Called on a background thread. */
    public interface Events {
        /** A device was plugged in or removed: read the lists again. */
        void onDevicesChanged();

        /** The open device went away; {@code message} is a plain sentence. */
        void onDeviceLost(String message);
    }

    /** One output device: where it is and what it is called. */
    public static final class Target {
        public final int route;
        public final int index;
        public final String name;
        public final boolean hardware;

        Target(int route, int index, String name, boolean hardware) {
            this.route = route;
            this.index = index;
            this.name = name;
            this.hardware = hardware;
        }
    }

    /** Picks the one hardware route to use (the app passes MidiRoutes.preferred). */
    public interface Picker {
        MidiRoute pick(MidiRoute[] hardware);
    }

    /**
     * The routes the player lists, in list order: the picked hardware route first, then every route that is not
     * hardware, in the order given. Routes that cannot work on this device ({@code isAvailable()} false) are left out.
     */
    public static MidiRoute[] choose(MidiRoute[] all, Picker picker) {
        java.util.List<MidiRoute> hardware = new java.util.ArrayList<MidiRoute>();
        java.util.List<MidiRoute> others = new java.util.ArrayList<MidiRoute>();
        for (MidiRoute route : all) {
            if (!route.isAvailable()) continue;
            if (route.isHardware()) hardware.add(route); else others.add(route);
        }
        java.util.List<MidiRoute> chosen = new java.util.ArrayList<MidiRoute>();
        if (!hardware.isEmpty()) {
            MidiRoute picked = picker.pick(hardware.toArray(new MidiRoute[0]));
            if (picked != null) chosen.add(picked);
        }
        chosen.addAll(others);
        return chosen.toArray(new MidiRoute[0]);
    }

    /** The plain sentence shown when a device does not answer (the technical part follows under "Details:"). */
    public static final String STUCK_SENTENCE = "The MIDI device did not answer when the player tried to open it. "
        + "Unplug it and plug it in again, or choose another output.";

    /**
     * What a route's open returns when it has asked the user for permission (the USB dialog) and the answer is still
     * to come: not an error; the route reports the device list changed once the answer is in.
     */
    public static final String PERMISSION_PENDING = "Waiting for permission to use the USB device.";

    /** Longest wait for a route to open a device (ms). */
    public static final long OPEN_DEADLINE_MS = 3000;
    /** Longest wait for a route to close, and for a device list (ms). */
    public static final long CLOSE_DEADLINE_MS = 1500;
    public static final long LIST_DEADLINE_MS = 1500;
    /** Longest wait for a route's own lock (another request to the same route is running) (ms). */
    public static final long LOCK_WAIT_MS = 300;

    private final MidiRoute[] routes;
    private final String[][] listed;         // last list() result per route (guarded by listLock)
    private final Object listLock = new Object();
    private final java.util.concurrent.locks.ReentrantLock[] locks;   // one per route: serializes open/close of that route
    private final java.util.concurrent.atomic.AtomicInteger[] stuckCalls;   // calls to this route that were abandoned and still run
    private final java.util.concurrent.atomic.AtomicBoolean[] listing;      // a device list call is in flight
    private final String[] stuckWhere;       // where the latest abandoned call is stuck (stuckLock)
    private final Object stuckLock = new Object();
    private volatile int openRoute = -1;      // route with an open device, or -1
    private volatile String openName = "";    // name of that device
    private volatile Events events;
    private volatile long openDeadlineMs = OPEN_DEADLINE_MS;

    public RouteCatalog(MidiRoute[] routes) {
        this.routes = routes;
        this.listed = new String[routes.length][];
        this.locks = new java.util.concurrent.locks.ReentrantLock[routes.length];
        this.stuckCalls = new java.util.concurrent.atomic.AtomicInteger[routes.length];
        this.listing = new java.util.concurrent.atomic.AtomicBoolean[routes.length];
        this.stuckWhere = new String[routes.length];
        for (int i = 0; i < routes.length; i++) {
            listed[i] = new String[0];
            locks[i] = new java.util.concurrent.locks.ReentrantLock();
            stuckCalls[i] = new java.util.concurrent.atomic.AtomicInteger();
            listing[i] = new java.util.concurrent.atomic.AtomicBoolean();
            stuckWhere[i] = "";
            final int number = i;
            routes[i].setListener(new MidiRoute.Listener() {
                @Override public void onDevicesChanged() {
                    Events e = events;
                    if (e != null) e.onDevicesChanged();
                }

                @Override public void onDeviceLost(String message) {
                    if (openRoute == number) openRoute = -1;
                    Events e = events;
                    if (e != null) e.onDeviceLost(message);
                }
            });
        }
    }

    public void setEvents(Events events) { this.events = events; }

    /** Tests: shorter open deadline. */
    public void setOpenDeadlineMs(long ms) { openDeadlineMs = ms; }

    @Override public int routeCount() { return routes.length; }

    /**
     * The device names of one route. A route that does not answer within {@link #LIST_DEADLINE_MS} (or one whose earlier
     * list call is still running) gives the previous list; this never blocks for long.
     */
    @Override public String[] list(final int route) {
        if (route < 0 || route >= routes.length) return new String[0];
        String[] names = null;
        if (listing[route].compareAndSet(false, true)) {
            final String[][] box = new String[1][];
            BoundedCall.Result r = BoundedCall.run("midibn-list", LIST_DEADLINE_MS, new Runnable() {
                @Override public void run() {
                    try {
                        box[0] = routes[route].listDevices();
                    } catch (RuntimeException e) {
                        box[0] = null;
                    } finally {
                        listing[route].set(false);
                    }
                }
            }, null, null);
            if (r.finished) names = box[0];
            else synchronized (listLock) { names = listed[route].clone(); }
        } else {
            synchronized (listLock) { names = listed[route].clone(); }
        }
        if (names == null) names = new String[0];
        synchronized (listLock) { listed[route] = names.clone(); }
        return names;
    }

    /**
     * The device behind position {@code position} of the output list (0 is "None" and gives null), counted over the
     * lists read by the latest {@link #list} calls.
     */
    public Target resolve(int position) {
        int remaining = position - 1;
        if (remaining < 0) return null;
        synchronized (listLock) {
            for (int r = 0; r < routes.length; r++) {
                if (remaining < listed[r].length) {
                    return new Target(r, remaining, listed[r][remaining], routes[r].isHardware());
                }
                remaining -= listed[r].length;
            }
        }
        return null;
    }

    /** True when this route is the direct USB route. */
    public boolean isDirectUsb(int route) { return route >= 0 && route < routes.length && routes[route].isDirectUsb(); }

    /** True when the device {@code name} of this route is open and usable: opening it again costs nothing. Never blocks. */
    public boolean isReady(int route, String name) {
        if (route < 0 || route >= routes.length) return false;
        MidiRoute r = routes[route];
        return openRoute == route && name != null && name.equals(openName) && r.isOpen() && !r.isLost();
    }

    /** From now on the route counts as closed (so a later open does a real open); the physical close follows with close(). Never blocks. */
    public void markClosing(int route) {
        if (openRoute == route) openRoute = -1;
    }

    /** True while an abandoned call to this route is still stuck: the route is not used until it returns. */
    public boolean isStuck(int route) {
        return route >= 0 && route < routes.length && stuckCalls[route].get() > 0;
    }

    private String stuckMessage(int route, String what) {
        String where;
        synchronized (stuckLock) { where = stuckWhere[route]; }
        return STUCK_SENTENCE + "\n\nDetails: " + what + " (" + routes[route].name() + ")"
            + (where.isEmpty() ? "" : "; stuck in: " + where);
    }

    /**
     * Opens a device (blocks the calling thread, up to the open deadline plus short lock waits; call it from a worker,
     * never from the UI thread: {@link OutputOpener} does). A route call that does not return in time is abandoned on
     * its own thread and reported with the plain sentence and the details; a late success is closed again.
     */
    @Override public String open(final int route, int index, String name) {
        if (route < 0 || route >= routes.length) return "No such MIDI route.";
        if (isReady(route, name)) return null;   // opened ahead by the warm-up: nothing to wait for
        if (isStuck(route)) return stuckMessage(route, "an earlier request to this route has still not returned");
        java.util.concurrent.locks.ReentrantLock lock = locks[route];
        if (!tryLock(lock, LOCK_WAIT_MS)) return stuckMessage(route, "another request to this route is still running");
        try {
            if (isReady(route, name)) return null;
            final MidiRoute r = routes[route];
            if (openRoute >= 0 && openRoute != route) closeRoute(openRoute);
            // The list may have changed since the player listed it: find the device again by its name.
            String[] names = null;
            if (listing[route].compareAndSet(false, true)) {
                final String[][] box = new String[1][];
                BoundedCall.Result lr = BoundedCall.run("midibn-list", LIST_DEADLINE_MS, new Runnable() {
                    @Override public void run() {
                        try { box[0] = r.listDevices(); } catch (RuntimeException e) { box[0] = null; } finally { listing[route].set(false); }
                    }
                }, null, null);
                if (lr.finished) names = box[0];
                else synchronized (listLock) { names = listed[route].clone(); }
            } else {
                synchronized (listLock) { names = listed[route].clone(); }
            }
            int at = -1;
            if (names != null) {
                if (index >= 0 && index < names.length && (name == null || name.equals(names[index]))) at = index;
                else if (name != null) {
                    for (int i = 0; i < names.length && at < 0; i++) if (name.equals(names[i])) at = i;
                }
            }
            if (at < 0) return "The MIDI device \"" + (name == null ? "" : name) + "\" is no longer available. Choose the output again.";
            final int device = at;
            final String[] problem = new String[1];
            final boolean[] ok = new boolean[1];
            final String openedName = names[at];
            BoundedCall.Result result = BoundedCall.run("midibn-open", openDeadlineMs, new Runnable() {
                @Override public void run() {
                    try {
                        problem[0] = r.open(device);
                    } catch (RuntimeException e) {
                        problem[0] = "The MIDI device could not be opened.";
                    }
                    ok[0] = problem[0] == null;
                }
            }, new Runnable() {
                @Override public void run() { stuckCalls[route].incrementAndGet(); }
            }, new Runnable() {
                // The open came back after the caller gave up: nobody waits for this device, so close it again
                // (unless another request has meanwhile made this route's device the open one).
                @Override public void run() {
                    stuckCalls[route].decrementAndGet();
                    if (openRoute != route) {
                        try { r.close(); } catch (RuntimeException ignored) { }
                    }
                }
            });
            if (!result.finished) {
                openRoute = -1;
                synchronized (stuckLock) { stuckWhere[route] = result.where; }
                return stuckMessage(route, "opening \"" + openedName + "\" had not returned after " + openDeadlineMs + " ms");
            }
            if (!ok[0]) {
                openRoute = -1;
                return problem[0];
            }
            openName = openedName;
            openRoute = route;
            return null;
        } finally {
            lock.unlock();
        }
    }

    @Override public void close(int route) {
        if (route >= 0 && route < routes.length) closeRoute(route);
    }

    /** Closes whatever is open. */
    public void closeAll() {
        for (int r = 0; r < routes.length; r++) closeRoute(r);
    }

    /** Closes one route without ever waiting long: the route's lock for a short time, the route's own close for a bounded time. */
    private void closeRoute(final int route) {
        if (openRoute == route) openRoute = -1;
        final MidiRoute r = routes[route];
        java.util.concurrent.locks.ReentrantLock lock = locks[route];
        boolean locked = tryLock(lock, LOCK_WAIT_MS);
        try {
            BoundedCall.run("midibn-close", CLOSE_DEADLINE_MS, new Runnable() {
                @Override public void run() { r.close(); }
            }, null, null);
        } finally {
            if (locked) lock.unlock();
        }
        if (openRoute == route) openRoute = -1;
    }

    private static boolean tryLock(java.util.concurrent.locks.ReentrantLock lock, long ms) {
        try {
            return lock.tryLock(ms, java.util.concurrent.TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    /** True when a device of this route is open (tests). */
    public boolean isOpen(int route) { return openRoute == route && routes[route].isOpen(); }
}
