package org.midibottleneck.core.route;

/**
 * The built-in software synthesizer (Sonivox, Android's own synthesizer; Apache 2.0) as a route with one device,
 * "Built-in synth (Sonivox)". It needs no MIDI hardware and no permission. Like the native route, the Java side only
 * opens and attaches it: the sound is made in C++ (platform/android/synth), which plays through OpenSL ES, and every
 * send below goes to the same C++ output the engine uses (BatchingOutput), so batching, SysEx, reset and panic are the
 * shared code. Works on every build (Android 6 and newer); there is no Android class in this file.
 */
public final class SynthMidiRoute implements MidiRoute {
    /** Name of the one device, as shown in the output list. */
    public static final String DEVICE_NAME = "Built-in synth (Sonivox)";

    private volatile boolean attached;
    private volatile String error;

    @Override public String name() { return "Built-in synthesizer (Sonivox)"; }

    /** A software synth inside the app: listed beside the preferred hardware route, never instead of it. */
    @Override public boolean isHardware() { return false; }

    @Override public boolean isAvailable() { return RouteNative.nativeAvailable(); }

    @Override public String[] listDevices() { return new String[] {DEVICE_NAME}; }

    @Override public RouteDevice[] describeDevices() {
        return new RouteDevice[] {new RouteDevice(0, 0, DEVICE_NAME, "Sonic Network / Android Open Source Project",
            "Sonivox EAS", "Software", 0, "", 1)};
    }

    /** Starts the synthesizer and its audio and attaches it to the engine. Quick, but call it off the UI thread anyway. */
    @Override public String open(int index) {
        close();
        if (index != 0) return fail("No such MIDI device.");
        if (!isAvailable()) return fail("The built-in synthesizer needs the native library, which is not loaded.");
        RouteNative.claim(this);   // closes any other open route: the engine has one output
        error = null;
        String problem = nativeAttach();
        if (problem != null) {
            RouteNative.release(this);
            return fail(problem);
        }
        attached = true;
        return null;
    }

    @Override public void close() {
        if (attached) {
            attached = false;
            RouteNative.detach();   // closes the output: audio stops, the synthesizer is released
        }
        RouteNative.release(this);
    }

    @Override public boolean isOpen() { return attached; }

    /** Latency and counters of the open synthesizer as one sentence (dropped messages, slow buffers); null when closed. */
    public String status() { return attached && RouteNative.nativeAvailable() ? nativeStatus() : null; }

    private String fail(String text) {
        error = text;
        return text;
    }

    private boolean result(String problem) {
        if (problem == null) return true;
        error = problem;
        return false;
    }

    @Override public boolean sendShort(int packed) { return attached && result(RouteNative.sendShort(packed)); }

    @Override public boolean sendLong(byte[] data, int offset, int length) {
        return attached && result(RouteNative.sendLong(data, offset, length));
    }

    @Override public void beginBatch() { if (attached) RouteNative.beginBatch(); }

    @Override public void endBatch() { if (attached) RouteNative.endBatch(); }

    @Override public void panic() { if (attached) result(RouteNative.panic()); }

    @Override public void discardPending() { if (attached) result(RouteNative.reset()); }

    @Override public String lastError() { return error; }

    // ---- Synth settings --------------------------------------------------------------------------------------------
    // The settings belong to the process, not to this object: they can be changed while the synth is closed and apply when it
    // opens, and while it plays the audio thread applies them before its next buffer (the Java side never calls Sonivox).
    // Nothing is saved between runs. Without the native library the setters do nothing and the getters give the defaults.

    /** Number of reverb presets of Sonivox, and their names (index = the value of {@link #setReverbPreset}). */
    public static final String[] REVERB_PRESETS = {"Large hall", "Hall", "Chamber", "Room"};
    /** Highest volume, in percent (100 is the synth's normal level; above 100 amplifies and may distort). Below 100 the volume
     *  is applied before Sonivox saturates its mix, so a lower volume gives loud passages headroom against clipping. */
    public static final int VOLUME_MAX = 200;

    /** The current settings: volume %, voice limit, reverb on (0/1), reverb preset, reverb amount %, build maximum of voices. */
    public static int[] settings() {
        int[] values = RouteNative.nativeAvailable() ? nativeGetSettings() : null;
        return values != null && values.length >= 6 ? values : new int[] {100, 64, 0, 1, 85, 64};
    }

    /** The most voices this build of the synthesizer can play (the compiled MAX_SYNTH_VOICES, read from native code). */
    public static int maxVoices() { return RouteNative.nativeAvailable() ? nativeMaxVoices() : 64; }

    /** The voice limit the synth starts with (Android's own default). */
    public static final int DEFAULT_VOICES = 64;

    /** Master volume in percent, 0 (silent) to {@link #VOLUME_MAX}. */
    public static void setVolume(int percent) { if (RouteNative.nativeAvailable()) nativeSetVolume(percent); }

    /** Voice limit (polyphony), 1 up to the build maximum. */
    public static void setVoiceLimit(int voices) { if (RouteNative.nativeAvailable()) nativeSetVoiceLimit(voices); }

    public static void setReverbOn(boolean on) { if (RouteNative.nativeAvailable()) nativeSetReverbOn(on); }

    /** Index into {@link #REVERB_PRESETS}. */
    public static void setReverbPreset(int preset) { if (RouteNative.nativeAvailable()) nativeSetReverbPreset(preset); }

    /** How much reverb is mixed in, 0 to 100 percent. */
    public static void setReverbAmount(int percent) { if (RouteNative.nativeAvailable()) nativeSetReverbAmount(percent); }

    /** Back to the defaults (volume 100 %, 64 voices, reverb off, preset Hall, amount 85 %). */
    public static void resetSettings() { if (RouteNative.nativeAvailable()) nativeResetSettings(); }

    /** Status of whichever synth is playing now (any instance); null when the synth is not open. */
    public static String liveStatus() { return RouteNative.nativeAvailable() ? nativeStatus() : null; }

    /**
     * Level meter of the synth that is playing now: {peak in percent of full scale (falls back quickly), number of buffers
     * that had clipped samples so far, number of clipped samples so far}. A sample is clipped when it sits at the 16-bit limit,
     * which is where Sonivox saturates its mix. Null when the synth is not open.
     */
    public static int[] meter() { return RouteNative.nativeAvailable() ? nativeMeter() : null; }

    // ---- DLS sound bank ---------------------------------------------------------------------------------------------
    // Not remembered: the bank is held in memory by the native settings and is gone when the app's process ends. Loading reads
    // the file on the calling thread (call it off the UI thread) and hands the finished bank to the audio thread, which
    // installs it before its next buffer; the audio is never paused and Sonivox is never touched from this thread.

    /** Returned by {@link #loadDls} when the descriptor cannot be read at an offset (a pipe): copy the document to a file first. */
    public static final String DLS_NOT_SEEKABLE = "NOT_SEEKABLE";
    /** Largest bank accepted, in bytes (the native loader checks the same limit). */
    public static final long DLS_MAX_BYTES = 192L * 1024 * 1024;

    /**
     * Loads a DLS sound bank from an open, seekable file descriptor (read at offsets; not closed here). Returns null on success,
     * {@link #DLS_NOT_SEEKABLE}, or one plain sentence saying why not (not a DLS file, unsupported, too large); the previously
     * loaded bank stays in use after a failure. {@code displayName} is what {@link #dlsName()} reports.
     */
    public static String loadDls(int fd, String displayName) {
        if (!RouteNative.nativeAvailable()) return "The built-in synthesizer is not available in this build.";
        return nativeLoadDls(fd, displayName);
    }

    /** Back to Sonivox's built-in sounds. */
    public static void useBuiltInSounds() { if (RouteNative.nativeAvailable()) nativeUseBuiltInSounds(); }

    /** File name of the loaded DLS bank, or null when the built-in sounds are in use. */
    public static String dlsName() { return RouteNative.nativeAvailable() ? nativeDlsName() : null; }

    private static native String nativeAttach();
    private static native String nativeStatus();
    private static native int nativeMaxVoices();
    private static native int[] nativeGetSettings();
    private static native void nativeSetVolume(int percent);
    private static native void nativeSetVoiceLimit(int voices);
    private static native void nativeSetReverbOn(boolean on);
    private static native void nativeSetReverbPreset(int preset);
    private static native void nativeSetReverbAmount(int percent);
    private static native void nativeResetSettings();
    private static native String nativeLoadDls(int fd, String name);
    private static native void nativeUseBuiltInSounds();
    private static native String nativeDlsName();
    private static native int[] nativeMeter();
}
