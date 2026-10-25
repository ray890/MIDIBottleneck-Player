package org.midibottleneck.core;



/**
 * The player's bridge to the portable core (work item U13). Owns one native PlayerController (the app layer that does
 * all the thinking). The native side is v2/android/core/src/main/cpp/player_jni.cpp.
 *
 * Threading: call everything from the Android main thread, except {@link #onNativeWake()}, which the native engine
 * and loader call from their own threads; it only forwards to {@link WakeListener#onWake()} (which must post to the
 * main thread and call {@link #pump()} there).
 *
 * A refresh is ONE call: {@link #snapshot()} refreshes the controller and fills {@link #n} (numbers, index N_*) and
 * {@link #t} (texts, index T_*) in one native call. Keep the constants in step with player_jni.cpp.
 */
public final class PlayerBridge {
    public interface WakeListener {
        /** Any thread. Ask the main thread to call {@link PlayerBridge#pump()} and refresh the screen. */
        void onWake();
    }

    /**
     * The output devices as the native player sees them: a numbered list of routes, each with its devices. The native
     * side calls this (C++ class JavaCatalog in player_jni.cpp) to list and to open; it never calls it per message,
     * the messages go from C++ straight into the open route. The native device id is (route << 16) | device index.
     * {@link #open} may block for seconds (Android opens devices asynchronously). The methods run on the thread of the
     * caller (the main thread) and must not throw.
     */
    public interface OutputCatalog {
        /** Number of routes, in list order. */
        int routeCount();

        /** Fresh reading of the device names of one route. */
        String[] list(int route);

        /**
         * Opens device {@code index} of {@code route} so that the route's native output is attached for the engine.
         * {@code name} is the name the device had in the list: when the list changed, the device is found again by name.
         * Returns null on success, else a plain sentence.
         */
        String open(int route, int index, String name);

        /** Closes the open device of that route, if any. */
        void close(int route);
    }

    // ---- numbers (long[]), same order as enum Num in player_jni.cpp
    public static final int N_LOADING = 0, N_LOAD_OVERALL = 1, N_LOAD_STAGE = 2, N_OUTPUT_SELECTED = 3, N_OUTPUT_ENABLED = 4,
            N_QUEUE_CHECKED = 5, N_QUEUE_ENABLED = 6, N_QUEUE_VALUE = 7, N_QUEUE_MIN = 8, N_QUEUE_MAX = 9,
            N_QUEUE_VALUE_ENABLED = 10, N_OVERFLOW_SELECTED = 11, N_OVERFLOW_ENABLED = 12, N_RATE_CHOICE = 13,
            N_RATE_FORWARD = 14, N_PROC_VALUE = 15, N_PROC_MIN = 16, N_PROC_MAX = 17, N_PROC_ENABLED = 18,
            N_PROC_VISIBLE = 19, N_UNIT_VISIBLE = 20, N_DIN_VISIBLE = 21, N_DIN_ENABLED = 22, N_SLIDER_VALUE = 23,
            N_SLIDER_ENABLED = 24, N_PLAY_ENABLED = 25, N_STOP_ENABLED = 26, N_SEEK_ENABLED = 27, N_POSITION_US = 28,
            N_DURATION_US = 29, N_QUEUE_LIMITED = 30, N_VIRTUAL_QUEUE = 31, N_AGE_PRESSURE = 32, N_OCCUPIED = 33,
            N_LIMIT = 34, N_OVERFLOW_PULSE = 35, N_NEEDS_REFRESH = 36, N_HAS_SONG = 37, N_STATE = 38,
            N_ROUTING_AVAILABLE = 39, N_APPLY_QUEUE_CHECKED = 40, N_APPLY_QUEUE_ENABLED = 41, N_QUEUE_AGE_CHECKED = 42,
            N_QUEUE_AGE_ENABLED = 43, N_CHASE_CHECKED = 44, N_CHASE_ENABLED = 45, N_PRESERVE_CHECKED = 46,
            N_PRESERVE_ENABLED = 47, N_PACED_CHECKED = 48, N_PACED_ENABLED = 49, N_SCALE_CHECKED = 50,
            N_SCALE_ENABLED = 51, N_CURRENT_LAG_US = 52, N_COUNT = 53;

    // ---- texts (String[]), same order as enum Text in player_jni.cpp
    public static final int T_OPEN = 0, T_FILE = 1, T_FILE_INFO = 2, T_LOAD_STATUS = 3, T_STATE = 4, T_OUTPUT_ITEMS = 5,
            T_OVERFLOW_ITEMS = 6, T_QUEUE_CHECK = 7, T_EVENTS_LABEL = 8, T_OVERFLOW_LABEL = 9, T_RATE_LABEL = 10,
            T_SERVICE_LABEL = 11, T_SERVICE_UNIT = 12, T_PLAY = 13,
            // The statistics cells, with the wording and order of the desktop Analysis window's Playback statistics (state,
            // timeline / output, queue now / maximum, events sent / dropped, output rate, effective speed, lag current /
            // maximum), then the maximum rate (not in the Analysis list; the main window's own cell).
            T_STAT_PLAYBACK = 14, T_STAT_TIMELINE = 15, T_STAT_QUEUE = 16, T_STAT_EVENTS = 17, T_STAT_OUTPUT_RATE = 18,
            T_STAT_SPEED = 19, T_STAT_LAG = 20, T_STAT_MAX_RATE = 21, STAT_CELLS = 8,
            T_PRESSURE = 22, T_PRESSURE_TIP = 23, T_RATE_ITEMS = 24, T_RATE_TIP = 25, T_QUEUE_TIP = 26,
            T_OVERFLOW_TIP = 27, T_SLIDER_TIP = 28, T_PLAY_TIP = 29, T_FILE_TIP = 30, T_NO_SONG = 31, T_COUNT = 32;

    /** Engine state values in {@code n[N_STATE]}. */
    public static final int STATE_STOPPED = 0, STATE_PLAYING = 1, STATE_PAUSED = 2;

    // ---- action numbers for nativeAction (keep in step with the switch in player_jni.cpp)
    private static final int A_PLAY = 0, A_STOP = 1, A_SEEK_BY = 2, A_SEEK_TO = 3, A_DRAGGING = 4, A_OUTPUT = 5,
            A_RATE = 6, A_QUEUE_CHECK = 7, A_QUEUE_VALUE = 8, A_OVERFLOW = 9, A_PROC_VALUE = 10, A_SLIDER = 11,
            A_DIN = 12, A_RESET_STATS = 13, A_APPLY_QUEUE = 14, A_QUEUE_AGE = 15, A_CHASE = 16, A_PRESERVE = 17,
            A_PACED = 18, A_SCALE_INTERVAL = 19, A_REFRESH_DEVICES = 20;

    /**
     * The adaptive refresh rule of the live display, the Android twin of the desktop's DisplayPacer plus the decision rule
     * of core/include/midibn/app/display_tier.h (same ladder, same budget, same window, same steps). Pure Java, no Android
     * classes, so the host test runs it; the screen feeds it from Choreographer frame callbacks.
     *
     * The rule, exactly:
     * <ol>
     * <li>One callback is armed at a time and re-armed only after the previous update finished (the update runs inside the
     * callback, on the main thread). Display frames that pass while an update runs are skipped, never queued.</li>
     * <li>The screen's rate is measured from the callback times (smoothed, 7/8 old + 1/8 new); a gap of 1.5 frames or more is
     * counted as late frames, and when the previous callback ran an update the gap beyond one frame is added to that
     * update's cost (the layout and drawing pass that follows the callback is part of the gap).</li>
     * <li>Tier 0: an update on every display frame. Tier 1: the statistics cells are drawn half on one update, half on the
     * next. Tier 2 and up: also at most 60, 30, 15, 10 updates per second (a rung must be below 90 % of the screen's rate);
     * a cap is kept by an accumulating deadline half a frame early, so 60 on a 90 Hz screen averages 60 per second.</li>
     * <li>Every 0.5 s of updates the cost share (update time plus the gap overrun, over the window) is judged against the
     * budget (processors up to 1: 8 %, 2: 12 %, 3 or more: 25 %): over the budget twice in a row, or over twice the
     * budget once, goes one tier down. One tier up needs the cost predicted at the tier above to be at most 0.6 of the
     * budget for 6 windows in a row. With one or two processors a current playback lag of 20 ms or more also goes one
     * tier down at once (playback comes first).</li>
     * </ol>
     */
    public static final class RefreshPacing {
        public static final long WINDOW_NS = 500000000L;
        private static final int MAX_TIERS = 8;
        private static final double RELEASE = 0.6, FAR_OVER = 2.0, STATISTICS_HALF = 0.5;
        private static final int OVER_WINDOWS = 2, CALM_WINDOWS = 6;
        private static final long BEHIND_LAG_US = 20000;

        private final double budget;
        private final boolean behindBackoff;
        private final int[] rateHz = new int[MAX_TIERS];
        private final boolean[] half = new boolean[MAX_TIERS];
        private int count;
        private int ladderNativeHz;
        private int tier, over, calm;
        private long frameNs;
        private long lastFrameNs, dueNs;
        private boolean lastWasUpdate;
        private long windowStartNs, movingNs, statisticsNs, overrunNs;
        private int updates;
        // diagnostics (never shown by default)
        private long framesSeen, updatesRun, capSkipped, lateFrames, tierChanges, refreshNsTotal, refreshNsMax, overrunNsTotal;
        private int maxTier;

        public RefreshPacing(int processors, double displayHz) {
            budget = displayBudgetForProcessors(processors);
            behindBackoff = processors <= 2;
            double hz = displayHz >= 24 && displayHz <= 500 ? displayHz : 60;
            frameNs = Math.round(1e9 / hz);
            buildLadder(Math.round((float) hz));
        }

        public static double displayBudgetForProcessors(int processors) {
            return processors <= 1 ? 0.08 : processors == 2 ? 0.12 : 0.25;
        }

        private void buildLadder(int nativeHz) {
            if (nativeHz <= 0) nativeHz = 60;
            ladderNativeHz = nativeHz;
            count = 0;
            rateHz[count] = 0; half[count] = false; count++;
            rateHz[count] = 0; half[count] = true; count++;
            final int[] rates = { 60, 30, 15, 10 };
            for (int rate : rates) {
                if (rate * 10 < nativeHz * 9) { rateHz[count] = rate; half[count] = true; count++; }
            }
            if (tier >= count) tier = count - 1;
        }

        /** A new run of live updates (playback or loading started): the measuring window starts afresh, the tier is kept. */
        public void restart() {
            lastFrameNs = 0; dueNs = 0; lastWasUpdate = false;
            windowStartNs = 0; movingNs = 0; statisticsNs = 0; overrunNs = 0; updates = 0;
        }

        /**
         * A display frame arrived ({@code frameTimeNs}, the Choreographer frame time). Returns true when an update is to
         * run on this frame, false when the frame is skipped (the tier's rate cap).
         */
        public boolean onFrame(long frameTimeNs) {
            framesSeen++;
            if (lastFrameNs != 0) {
                final long interval = frameTimeNs - lastFrameNs;
                if (interval * 2 > frameNs && interval * 2 < frameNs * 3) {
                    frameNs = (frameNs * 7 + interval) / 8;
                } else if (interval * 2 >= frameNs * 3) {
                    lateFrames += Math.max(0, Math.round((double) interval / frameNs) - 1);
                    if (lastWasUpdate) overrunNs += interval - frameNs;
                }
            }
            lastFrameNs = frameTimeNs;
            boolean run = true;
            final int cap = rateHz[tier];
            if (cap > 0 && frameNs * cap < 1000000000L) {
                final long period = 1000000000L / cap;
                if (dueNs == 0 || frameTimeNs - dueNs > period) dueNs = frameTimeNs;
                if (frameTimeNs < dueNs - frameNs / 2) run = false;
                else dueNs += period;
            } else {
                dueNs = 0;
            }
            if (!run) capSkipped++;
            lastWasUpdate = run;
            return run;
        }

        /** True when the statistics cells are drawn in two halves (tier 1 and up). */
        public boolean halfStatistics() { return half[tier]; }

        public int tier() { return tier; }

        /** Updates per second the current tier delivers on this screen. */
        public int tierRate() { return tierRate(tier); }

        private int tierRate(int t) {
            final int native_ = nativeHz();
            return rateHz[t] > 0 && rateHz[t] < native_ ? rateHz[t] : native_;
        }

        public int nativeHz() { return (int) Math.round(1e9 / frameNs); }

        /**
         * One update finished. {@code movingNs} is everything but the statistics block, {@code statisticsNs} the statistics
         * block, {@code nowNs} the frame time, {@code currentLagUs} the playback worker's current lag.
         */
        public void onUpdateDone(long movingNs, long statisticsNs, long nowNs, long currentLagUs) {
            updatesRun++;
            final long total = movingNs + statisticsNs;
            refreshNsTotal += total;
            if (total > refreshNsMax) refreshNsMax = total;
            if (windowStartNs == 0) windowStartNs = nowNs;
            this.movingNs += movingNs;
            this.statisticsNs += statisticsNs;
            updates++;
            if (nowNs - windowStartNs < WINDOW_NS) return;
            final long windowNs = nowNs - windowStartNs;
            final long moving = this.movingNs + overrunNs;
            final long statistics = this.statisticsNs;
            overrunNsTotal += overrunNs;
            final int n = updates;
            windowStartNs = nowNs; this.movingNs = 0; this.statisticsNs = 0; overrunNs = 0; updates = 0;
            final int nativeNow = nativeHz();
            if (nativeNow * 100L > ladderNativeHz * 115L || nativeNow * 115L < ladderNativeHz * 100L) buildLadder(nativeNow);
            decide(windowNs / 1000, n, moving / 1000, statistics / 1000, behindBackoff && currentLagUs >= BEHIND_LAG_US);
        }

        private double predictedShare(int from, int to, int n, long movingUs, long statisticsUs) {
            if (n <= 0) return 0;
            final double perMoving = (double) movingUs / n;
            final double perStatistics = (double) statisticsUs / n;
            final double full = half[from] ? perStatistics / STATISTICS_HALF : perStatistics;
            final double target = half[to] ? full * STATISTICS_HALF : full;
            return tierRate(to) * (perMoving + target) / 1000000.0;
        }

        private void decide(long windowUs, int n, long movingUs, long statisticsUs, boolean behind) {
            final int last = count - 1;
            if (tier > last) tier = last;
            if (tier < 0) tier = 0;
            if (windowUs <= 0) return;
            final double share = (double) (movingUs + statisticsUs) / (double) windowUs;
            boolean down = false;
            if (behind) down = true;
            else if (n > 0 && share > budget * FAR_OVER) down = true;
            else if (n > 0 && share > budget) down = ++over >= OVER_WINDOWS;
            else over = 0;
            if (down) {
                if (tier < last) { tier++; tierChanges++; if (tier > maxTier) maxTier = tier; }
                over = 0; calm = 0;
                return;
            }
            if (behind || share > budget) { calm = 0; return; }
            if (tier > 0 && n > 0) {
                if (predictedShare(tier, tier - 1, n, movingUs, statisticsUs) <= budget * RELEASE) {
                    if (++calm >= CALM_WINDOWS) { tier--; tierChanges++; calm = 0; over = 0; }
                } else {
                    calm = 0;
                }
            }
        }

        /** One line of counters for a debug read-out (frames seen, updates run, frames skipped by the cap, late frames, cost). */
        public String summary() {
            final long mean = updatesRun == 0 ? 0 : refreshNsTotal / updatesRun;
            return "frames " + framesSeen + ", updates " + updatesRun + ", skipped by rate cap " + capSkipped
                    + ", late frames " + lateFrames + ", update cost mean " + String.format(java.util.Locale.US, "%.2f", mean / 1e6)
                    + " ms, max " + String.format(java.util.Locale.US, "%.2f", refreshNsMax / 1e6)
                    + " ms, overrun " + String.format(java.util.Locale.US, "%.0f", overrunNsTotal / 1e6)
                    + " ms, screen " + nativeHz() + " Hz, tier " + tier + " (highest " + maxTier + ", changes " + tierChanges
                    + "), budget " + String.format(java.util.Locale.US, "%.0f", budget * 100) + " %";
        }

        /** Zeroes the diagnostics (a new file or a manual reset); the tier stays. */
        public void resetCounters() {
            framesSeen = updatesRun = capSkipped = lateFrames = tierChanges = refreshNsTotal = refreshNsMax = overrunNsTotal = 0;
            maxTier = tier;
        }
    }

    /** The latest snapshot (valid after {@link #snapshot()}). */
    public final long[] n = new long[N_COUNT];
    public final String[] t = new String[T_COUNT];

    private final WakeListener listener;
    private long handle;

    /** Creates the controller. {@code catalog} lists and opens the output devices (null: only "None"). */
    public PlayerBridge(OutputCatalog catalog, WakeListener listener) {
        this.listener = listener;
        String error = NativeCore.load();
        if (error != null) throw new IllegalStateException(error);
        handle = nativeCreate(catalog);
        for (int i = 0; i < T_COUNT; i++) t[i] = "";
    }

    /** Called by the native code from any thread. */
    @SuppressWarnings("unused")
    private void onNativeWake() {
        if (listener != null) listener.onWake();
    }

    // ---- lifecycle and events
    public void pump() { if (handle != 0) nativePump(handle); }

    /** Stops the engine; false when it could not stop safely (a message is queued). Call before {@link #destroy()}. */
    public boolean shutdown() { return handle == 0 || nativeShutdown(handle); }

    public void destroy() {
        if (handle != 0) { long h = handle; handle = 0; nativeDestroy(h); }
    }

    /**
     * Opens a file by a descriptor from ContentResolver (detach it from its ParcelFileDescriptor first: the bridge owns
     * it from now on and closes it).
     */
    public void openFile(int detachedFd, String displayName) { nativeOpenFile(handle, detachedFd, displayName); }

    public void cancelLoad() { nativeCancelLoad(handle); }

    /** One refresh: refreshes the controller and fills {@link #n} and {@link #t}. */
    public void snapshot() { if (handle != 0) nativeSnapshot(handle, n, t); }

    /** {title, heading, text} when the controller wants a large-file decision, else null. */
    public String[] largeFileDecision() { return nativeLargeFileDecision(handle); }

    public void answerLargeFile(boolean accepted) { nativeAnswerLargeFile(handle, accepted); }

    /** Queued messages as {kind, title, text} triples; kind "0" information, "1" warning, "2" error. */
    public String[] takeMessages() { return nativeTakeMessages(handle); }

    // ---- actions (all go to the controller)
    public void play() { nativeAction(handle, A_PLAY, 0); }
    public void stop() { nativeAction(handle, A_STOP, 0); }
    public void seekByMicros(long delta) { nativeAction(handle, A_SEEK_BY, delta); }
    public void seekToMicros(long target) { nativeAction(handle, A_SEEK_TO, target); }
    public void setTimelineDragging(boolean dragging) { nativeAction(handle, A_DRAGGING, dragging ? 1 : 0); }
    public void selectOutput(int index) { nativeAction(handle, A_OUTPUT, index); }
    public void selectRateChoice(int choice) { nativeAction(handle, A_RATE, choice); }
    public void setQueueLimitChecked(boolean on) { nativeAction(handle, A_QUEUE_CHECK, on ? 1 : 0); }
    public void setQueueValue(long value) { nativeAction(handle, A_QUEUE_VALUE, value); }
    public void setOverflow(int index) { nativeAction(handle, A_OVERFLOW, index); }
    public void setProcessingValue(long value) { nativeAction(handle, A_PROC_VALUE, value); }
    public void setSlider(int value) { nativeAction(handle, A_SLIDER, value); }
    public void dinPreset() { nativeAction(handle, A_DIN, 0); }
    public void resetStatistics() { nativeAction(handle, A_RESET_STATS, 0); }
    public void toggleApplyQueueLimitWithoutSlowdown() { nativeAction(handle, A_APPLY_QUEUE, 0); }
    public void toggleQueueAgeLimit() { nativeAction(handle, A_QUEUE_AGE, 0); }
    public void toggleStateChase() { nativeAction(handle, A_CHASE, 0); }
    public void togglePreservePauseSeek() { nativeAction(handle, A_PRESERVE, 0); }
    public void toggleOutputPacedTimeline() { nativeAction(handle, A_PACED, 0); }
    public void toggleScaleNoteIntervalByTraffic() { nativeAction(handle, A_SCALE_INTERVAL, 0); }
    /** Reads the output device list again (stopped only); the file and options stay, a vanished output becomes None with a warning message. */
    public void refreshDevices() { nativeAction(handle, A_REFRESH_DEVICES, 0); }

    // ---- Help (static)
    public static int helpTopicCount() { return NativeCore.load() == null ? nativeHelpTopicCount() : 0; }
    public static String helpTitle(int topic) { return NativeCore.load() == null ? nativeHelpTitle(topic) : ""; }
    /** The shared Help markup of one topic (see core/include/midibn/app/help_text.h). */
    public static String helpText(int topic) { return NativeCore.load() == null ? nativeHelpText(topic) : ""; }

    private native long nativeCreate(OutputCatalog catalog);
    private native void nativeDestroy(long handle);
    private native boolean nativeShutdown(long handle);
    private native void nativePump(long handle);
    private native void nativeOpenFile(long handle, int fd, String name);
    private native void nativeCancelLoad(long handle);
    private native String[] nativeLargeFileDecision(long handle);
    private native void nativeAnswerLargeFile(long handle, boolean accepted);
    private native String[] nativeTakeMessages(long handle);
    private native void nativeAction(long handle, int action, long argument);
    private native void nativeSnapshot(long handle, long[] numbers, String[] texts);
    private static native int nativeHelpTopicCount();
    private static native String nativeHelpTitle(int topic);
    private static native String nativeHelpText(int topic);
}
