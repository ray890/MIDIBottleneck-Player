package org.midibottleneck.player;

import org.midibottleneck.core.PlayerBridge;

/** Test-only door into the package-private OutputOpener (host tests live in the default package). */
public final class OutputOpenerTestAccess {
    private OutputOpenerTestAccess() {}

    public interface Done { void done(String problem); }

    public static Object create(RouteCatalog catalog) { return new OutputOpener(catalog); }

    public static void request(Object opener, RouteCatalog.Target t, final Done done) {
        ((OutputOpener) opener).request(t, done == null ? null : new OutputOpener.Callback() {
            @Override public void onOpened(RouteCatalog.Target target, String problem) { done.done(problem); }
        });
    }

    public static boolean isReady(Object opener, RouteCatalog.Target t) { return ((OutputOpener) opener).isReady(t); }

    public static PlayerBridge.OutputCatalog forBridge(Object opener) { return ((OutputOpener) opener).forBridge(); }

    public static void shutdown(Object opener) { ((OutputOpener) opener).shutdown(); }
}
