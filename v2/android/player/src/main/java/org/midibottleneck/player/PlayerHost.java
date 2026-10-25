package org.midibottleneck.player;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import org.midibottleneck.core.PlayerBridge;
import org.midibottleneck.core.route.BoundedCall;
import org.midibottleneck.core.route.RouteTraceLog;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;

/**
 * The one player of this process: owns the native PlayerController (through {@link PlayerBridge}) so it outlives the
 * screen (rotation, back button, screen off). All methods run on the main thread. The activity registers a
 * {@link Runnable} that redraws the screen; the foreground service is started and stopped from here.
 */
final class PlayerHost {
    private static PlayerHost instance;

    static synchronized PlayerHost get(Context context) {
        if (instance == null) instance = new PlayerHost(context.getApplicationContext());
        return instance;
    }

    private final Context app;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final RouteCatalog catalog;
    private PlayerBridge bridge;
    private Runnable screen;                 // the visible screen's redraw, or null
    private boolean serviceRunning;
    private Uri lastUri;                     // the last chosen file, for a rescan of the outputs
    private boolean pendingRescan;           // the device list changed while playing: read it again once stopped
    private final java.util.concurrent.atomic.AtomicBoolean wakePosted = new java.util.concurrent.atomic.AtomicBoolean();
    private final OutputOpener opener;      // opens and closes devices on its worker, never on the main thread
    private RouteCatalog.Target playTarget; // Play was tapped and waits for this output to open
    private String openingText;             // screen text while that wait lasts
    private String notice;                  // a failed open, for one dialog
    static final String OPENING_TEXT = "Opening the output...";
    static final String PERMISSION_TEXT = "Waiting for you to allow the USB device on the screen...";

    private PlayerHost(Context app) {
        this.app = app;
        this.catalog = PlayerRoutes.create(app);
        this.catalog.setEvents(new RouteCatalog.Events() {
            // Background threads: hand over to the main thread.
            @Override public void onDevicesChanged() {
                handler.post(new Runnable() {
                    @Override public void run() { devicesChanged(); }
                });
            }

            @Override public void onDeviceLost(String message) {
                // The route already refuses sends with a plain sentence (the engine reports it); here the output list
                // is read again so the output becomes None, exactly as with the Rescan button.
                handler.post(new Runnable() {
                    @Override public void run() { devicesChanged(); }
                });
            }
        });
        this.opener = new OutputOpener(catalog);
        this.bridge = newBridge();
        // Debug switch for the write trace (off by default, zero cost off): the marker file files/route-trace.on, made with
        // adb shell run-as org.midibottleneck.player touch files/route-trace.on; read with adb logcat -s MIDIBN-route.
        try {
            if (new java.io.File(app.getFilesDir(), "route-trace.on").exists()) {
                RouteTraceLog.setEnabled(true);
                traceOn = true;
            }
        } catch (RuntimeException ignored) {
        }
        knownUsb = usbNames();
        refresh();
        warmSelected();
    }

    private boolean traceOn;
    private int lastState = -1;
    private java.util.List<String> knownUsb = new java.util.ArrayList<String>();   // USB entries seen at the last device change

    /** Device list changed or the open device went away (main thread). Rescans now when stopped, else once stopped. */
    private void devicesChanged() {
        pendingRescan = true;
        refresh();
        autoSelectNewUsb(false);
        warmSelected();
    }

    /** The names of the direct USB entries of the output list (empty when this phone has no such route). */
    private java.util.List<String> usbNames() {
        java.util.List<String> names = new java.util.ArrayList<String>();
        for (int r = 0; r < catalog.routeCount(); r++) {
            if (catalog.isDirectUsb(r)) names.addAll(java.util.Arrays.asList(catalog.list(r)));
        }
        return names;
    }

    /**
     * USB synth auto-select: when a USB MIDI device has just been plugged in, the output becomes the first USB entry of
     * the list. Only while the player is stopped (no file loaded, or stopped), never during playback or pause. With
     * {@code force} the device counts as new even if the list already had it (the system just offered the player for it).
     */
    private void autoSelectNewUsb(boolean force) {
        java.util.List<String> now = usbNames();
        boolean added = force && !now.isEmpty();
        for (String name : now) if (!knownUsb.contains(name)) added = true;
        knownUsb = now;
        if (!added) return;
        bridge.snapshot();
        if (bridge.n[PlayerBridge.N_STATE] != PlayerBridge.STATE_STOPPED) return;   // never during playback or pause
        bridge.refreshDevices();   // the core reads the list again, so the new entry has a position
        for (int p = 1; ; p++) {
            RouteCatalog.Target t = catalog.resolve(p);
            if (t == null) return;
            if (catalog.isDirectUsb(t.route)) {
                bridge.selectOutput(p);
                refresh();
                return;
            }
        }
    }

    /** MainActivity calls this when the system started or resumed it because a USB device was plugged in (see the manifest filter). */
    void onUsbAttached() {
        catalog.list(0);
        for (int r = 0; r < catalog.routeCount(); r++) catalog.list(r);
        autoSelectNewUsb(true);
        warmSelected();
    }

    private PlayerBridge newBridge() {
        final Runnable wakeWork = new Runnable() {
            @Override public void run() {
                wakePosted.set(false);   // a wake that arrives from here on posts again; those before it are served by this run
                bridge.pump();
                refresh();
            }
        };
        return new PlayerBridge(opener.forBridge(), new PlayerBridge.WakeListener() {
            @Override public void onWake() {
                // Any thread. One posted wake at a time: wakes that arrive while one waits are served by it, so a busy main
                // thread never collects a pile of pump-and-refresh runs.
                if (wakePosted.compareAndSet(false, true)) handler.post(wakeWork);
            }
        });
    }

    PlayerBridge bridge() { return bridge; }

    void setScreen(Runnable redraw) { this.screen = redraw; }

    /** Takes one snapshot, keeps the foreground service in step with the state, and redraws the screen. */
    void refresh() {
        bridge.snapshot();
        int state = (int) bridge.n[PlayerBridge.N_STATE];
        if (traceOn && lastState > 0 && state == PlayerBridge.STATE_STOPPED) {
            BoundedCall.detached("midibn-trace-dump", new Runnable() {   // off the main thread: up to 4096 log lines
                @Override public void run() { RouteTraceLog.dump("playback ended"); }
            });
        }
        lastState = state;
        if (pendingRescan && bridge.n[PlayerBridge.N_STATE] == PlayerBridge.STATE_STOPPED) {
            pendingRescan = false;
            bridge.refreshDevices();   // a vanished output becomes None with a warning, the file and options stay
            bridge.snapshot();
        }
        updateService();
        if (screen != null) screen.run();
    }

    private void updateService() {
        final int state = (int) bridge.n[PlayerBridge.N_STATE];
        final boolean wanted = state == PlayerBridge.STATE_PLAYING || state == PlayerBridge.STATE_PAUSED;
        if (wanted && !serviceRunning) {
            Intent intent = new Intent(app, PlayerService.class);
            try {
                if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(intent); else app.startService(intent);
                serviceRunning = true;
            } catch (RuntimeException e) {
                serviceRunning = false;   // not allowed right now (app in the background): playback still runs
            }
        } else if (!wanted && serviceRunning) {
            app.stopService(new Intent(app, PlayerService.class));
            serviceRunning = false;
        }
        if (serviceRunning) PlayerService.update(bridge.t[PlayerBridge.T_FILE], bridge.t[PlayerBridge.T_STATE]);
    }

    /** Opens a file chosen with ACTION_OPEN_DOCUMENT: read through a descriptor, no path. Null on success, else a message. */
    String openUri(Uri uri) {
        ContentResolver resolver = app.getContentResolver();
        String name = "MIDI file";
        Cursor cursor = null;
        try {
            cursor = resolver.query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) name = cursor.getString(0);
        } catch (RuntimeException ignored) {
        } finally {
            if (cursor != null) cursor.close();
        }
        try {
            ParcelFileDescriptor pfd = resolver.openFileDescriptor(uri, "r");
            if (pfd == null) return "The file could not be opened.";
            lastUri = uri;
            bridge.openFile(pfd.detachFd(), name);   // the bridge owns the descriptor now
        } catch (java.io.FileNotFoundException e) {
            return "The file could not be opened: " + e.getMessage();
        } catch (SecurityException e) {
            return "No permission to read the file.";
        }
        refresh();
        return null;
    }

    /**
     * Lists the output devices again while stopped. The controller keeps the loaded file and every option; the selected
     * output stays selected by name, or becomes None with a warning (shown by the screen) when that device is gone.
     */
    void rescanOutputs() {
        if (bridge.n[PlayerBridge.N_STATE] != PlayerBridge.STATE_STOPPED) return;
        bridge.refreshDevices();
        refresh();
        warmSelected();
    }

    /** The user chose entry {@code position} of the output list. */
    void selectOutput(int position) {
        bridge.selectOutput(position);
        refresh();
        warmSelected();
    }

    /**
     * Opens the selected hardware device on a worker thread while stopped, so that Play finds it open: the player core
     * opens the output synchronously on the main thread when Play is tapped (a device can take up to 5 seconds to open
     * and the core cannot be changed from this work item), and a device that is already open costs nothing there. If the
     * worker's open fails, Play tries again and shows the error. Selecting None closes the open device. A route that
     * is not hardware (the built-in synthesizer) is not opened ahead.
     */
    private void warmSelected() {
        if (bridge.n[PlayerBridge.N_STATE] != PlayerBridge.STATE_STOPPED) return;
        final int selected = (int) bridge.n[PlayerBridge.N_OUTPUT_SELECTED];
        final RouteCatalog.Target target = catalog.resolve(selected);
        if (selected > 0 && (target == null || !target.hardware)) return;
        if (target == null) {
            clearPlayWait();
            opener.closeAllAsync();
        } else {
            opener.request(target, openCallback);
        }
    }

    // ------------------------------------------------------------------------------------------ Play without waiting

    /** What the screen shows instead of the state while the output opens, or null. */
    String openingText() { return openingText; }

    /** A message for a dialog (a failed open), or null; reading it clears it. */
    String takeNotice() {
        String n = notice;
        notice = null;
        return n;
    }

    /**
     * Play: when the selected output is open (or None) the player starts at once; otherwise the output is opened on the
     * worker and Play starts as soon as it is ready, or a plain message is shown if the device does not answer. The
     * screen never waits. Pause and resume go straight to the player.
     */
    void play() {
        bridge.snapshot();
        if (bridge.n[PlayerBridge.N_STATE] != PlayerBridge.STATE_STOPPED) {
            bridge.play();
            refresh();
            return;
        }
        final RouteCatalog.Target target = catalog.resolve((int) bridge.n[PlayerBridge.N_OUTPUT_SELECTED]);
        if (target == null || opener.isReady(target)) {
            clearPlayWait();
            bridge.play();
            refresh();
            return;
        }
        playTarget = target;
        openingText = OPENING_TEXT;
        opener.request(target, openCallback);
        refresh();
    }

    /** Stop: also cancels a Play that is waiting for the output to open. */
    void stop() {
        clearPlayWait();
        bridge.stop();
        refresh();
    }

    private void clearPlayWait() {
        playTarget = null;
        openingText = null;
    }

    private final OutputOpener.Callback openCallback = new OutputOpener.Callback() {
        @Override public void onOpened(final RouteCatalog.Target target, final String problem) {
            handler.post(new Runnable() {
                @Override public void run() { openFinished(target, problem); }
            });
        }
    };

    /** The worker finished opening {@code target} (main thread). */
    private void openFinished(RouteCatalog.Target target, String problem) {
        RouteCatalog.Target wanted = playTarget;
        if (wanted != null && wanted.route == target.route && wanted.index == target.index
                && wanted.name != null && wanted.name.equals(target.name)) {
            if (problem == null) {
                clearPlayWait();
                bridge.snapshot();
                RouteCatalog.Target now = catalog.resolve((int) bridge.n[PlayerBridge.N_OUTPUT_SELECTED]);
                if (bridge.n[PlayerBridge.N_STATE] == PlayerBridge.STATE_STOPPED && now != null
                        && now.route == target.route && now.name.equals(target.name)) {
                    bridge.play();
                }
            } else if (problem.equals(RouteCatalog.PERMISSION_PENDING)) {
                openingText = PERMISSION_TEXT;   // keep waiting: the permission answer reopens the device
            } else {
                clearPlayWait();
                notice = problem;
            }
        }
        refresh();
    }

    /** From the notification's Stop button. */
    void stopFromService() {
        clearPlayWait();
        bridge.stop();
        refresh();
    }
}
