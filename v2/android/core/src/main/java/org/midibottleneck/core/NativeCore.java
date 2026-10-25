package org.midibottleneck.core;

import org.midibottleneck.core.route.MidiRoute;

/**
 * Entry points into the portable core (JNI). The native side is v2/android/core/src/main/cpp/jni_bridge.cpp.
 * Shared by the player and the self-test app; each helper adds its own methods here and its own native file.
 */
public final class NativeCore {
    private NativeCore() {}

    private static boolean loaded;
    private static String loadError;

    /** Loads libmidibn_android.so once. Returns null on success, else a message. */
    public static synchronized String load() {
        if (!loaded && loadError == null) {
            try {
                System.loadLibrary("midibn_android");
                loaded = true;
            } catch (UnsatisfiedLinkError e) {
                loadError = "Native library not loaded: " + e.getMessage();
            }
        }
        return loadError;
    }

    /** "MIDIBottleneck core v2 (native, arm64-v8a)". */
    public static String coreVersion() { return load() == null ? nativeCoreVersion() : loadError; }

    /** "Android 14 (API 34), model, abi" from the platform layer. */
    public static String deviceDescription() { return load() == null ? nativeDeviceDescription() : loadError; }

    /** A trivial call into the model layer: loads a tiny built-in MIDI file and says what was found. */
    public static String modelDemo() { return load() == null ? nativeModelDemo() : loadError; }

    /**
     * Stub (output-route helper): registers the route the native engine sends its MIDI through.
     * Returns false until the route helper wires it.
     */
    public static boolean setMidiRoute(MidiRoute route) { return load() == null && nativeSetMidiRoute(route); }

    private static native String nativeCoreVersion();
    private static native String nativeDeviceDescription();
    private static native String nativeModelDemo();
    private static native boolean nativeSetMidiRoute(Object route);
}
