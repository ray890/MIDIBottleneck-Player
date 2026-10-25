package org.midibottleneck.player;

import org.midibottleneck.core.PlayerBridge;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;

/**
 * Opens and closes output devices off the UI thread, so the screen never waits for a MIDI device. One worker thread
 * runs {@link RouteCatalog#open} and {@link RouteCatalog#close} one after the other (those calls are bounded: a device
 * that does not answer is given up after a few seconds with a plain message). The UI thread only asks "is it ready?"
 * and says "open it", both without waiting.
 *
 * {@link #forBridge} is the catalog handed to the native player: its {@code open} (called by the player core on the UI
 * thread when Play is tapped) answers at once, either "open" or a plain sentence, and starts the real open on the worker.
 * {@link PlayerHost#play} uses {@link #request} first, so in normal use the core only ever finds the device ready.
 *
 * Plain Java (host-tested).
 */
final class OutputOpener {
    /** Called on the worker thread (or at once on the caller's thread when the device is already open). */
    interface Callback {
        /** {@code problem} is null when the device is open, else the message (plain sentence, then "Details:"). */
        void onOpened(RouteCatalog.Target target, String problem);
    }

    /** What the core shows when it asks for a device that is still being opened (only when PlayerHost.play was not used). */
    static final String STILL_OPENING = "The output is still opening. Press Play again in a moment.";

    private static final class Job {
        final RouteCatalog.Target target;
        final List<Callback> callbacks = new ArrayList<Callback>();

        Job(RouteCatalog.Target target) { this.target = target; }
    }

    private final RouteCatalog catalog;
    private final ExecutorService worker;
    private final Object lock = new Object();
    private final List<Job> pending = new ArrayList<Job>();
    private RouteCatalog.Target failedTarget;     // latest failed open and its message (lock)
    private String failedProblem;

    OutputOpener(RouteCatalog catalog) {
        this.catalog = catalog;
        this.worker = Executors.newSingleThreadExecutor(new ThreadFactory() {
            @Override public Thread newThread(Runnable r) {
                Thread t = new Thread(r, "midibn-output-open");
                t.setDaemon(true);
                return t;
            }
        });
    }

    /** Stops the worker thread after its queued work (tests). */
    void shutdown() { worker.shutdown(); }

    /** True when the device is open and usable: Play can start at once. Never blocks. */
    boolean isReady(RouteCatalog.Target target) {
        return target != null && catalog.isReady(target.route, target.name);
    }

    /** True while an open of this device is queued or running. */
    boolean isOpening(RouteCatalog.Target target) {
        synchronized (lock) {
            for (Job j : pending) if (same(j.target, target)) return true;
        }
        return false;
    }

    /** Starts opening {@code target} unless it is open; {@code callback} (may be null) hears the result. Never blocks. */
    void request(RouteCatalog.Target target, Callback callback) {
        if (target == null) return;
        if (catalog.isReady(target.route, target.name)) {
            if (callback != null) callback.onOpened(target, null);
            return;
        }
        final Job job;
        synchronized (lock) {
            Job existing = null;
            for (Job j : pending) if (same(j.target, target)) existing = j;
            if (existing != null) {
                if (callback != null) existing.callbacks.add(callback);
                return;
            }
            job = new Job(target);
            if (callback != null) job.callbacks.add(callback);
            pending.add(job);
        }
        worker.execute(new Runnable() {
            @Override public void run() {
                String problem;
                try {
                    problem = catalog.open(job.target.route, job.target.index, job.target.name);
                } catch (RuntimeException e) {
                    problem = "The MIDI device could not be opened.\n\nDetails: " + e;
                }
                List<Callback> toCall;
                synchronized (lock) {
                    pending.remove(job);
                    toCall = new ArrayList<Callback>(job.callbacks);
                    if (problem != null) { failedTarget = job.target; failedProblem = problem; }
                    else if (failedTarget != null && same(failedTarget, job.target)) { failedTarget = null; failedProblem = null; }
                }
                for (Callback c : toCall) {
                    try { c.onOpened(job.target, problem); } catch (RuntimeException ignored) { }
                }
            }
        });
    }

    /** Marks the route closed now and closes it on the worker (after any open already queued). Never blocks. */
    void closeAsync(final int route) {
        catalog.markClosing(route);
        worker.execute(new Runnable() {
            @Override public void run() { catalog.close(route); }
        });
    }

    /** Closes whatever is open, on the worker. Never blocks. */
    void closeAllAsync() {
        for (int r = 0; r < catalog.routeCount(); r++) catalog.markClosing(r);
        worker.execute(new Runnable() {
            @Override public void run() { catalog.closeAll(); }
        });
    }

    private static boolean same(RouteCatalog.Target a, RouteCatalog.Target b) {
        return a != null && b != null && a.route == b.route && a.index == b.index
            && (a.name == null ? b.name == null : a.name.equals(b.name));
    }

    /** The catalog for the native player: lists as before; open and close answer at once and work on the worker. */
    PlayerBridge.OutputCatalog forBridge() {
        return new PlayerBridge.OutputCatalog() {
            @Override public int routeCount() { return catalog.routeCount(); }

            @Override public String[] list(int route) { return catalog.list(route); }

            @Override public String open(int route, int index, String name) {
                RouteCatalog.Target t = new RouteCatalog.Target(route, index, name, true);
                if (catalog.isReady(route, name)) return null;
                String earlier = null;
                synchronized (lock) {
                    if (failedTarget != null && same(failedTarget, t)) {
                        earlier = failedProblem;
                        failedTarget = null;
                        failedProblem = null;
                    }
                }
                request(t, null);   // Play again retries
                return earlier != null ? earlier : STILL_OPENING;
            }

            @Override public void close(int route) { closeAsync(route); }
        };
    }
}
