package org.midibottleneck.selftest;

/** JNI entry points of libselftest_jni.so (v2/android/selftest/src/main/cpp/selftest_jni.cpp). */
final class Native {
    private Native() {}

    private static boolean loaded;
    private static String loadError;

    /** Loads the library once. Returns null on success, else a message. */
    static synchronized String load() {
        if (!loaded && loadError == null) {
            try {
                System.loadLibrary("selftest_jni");
                loaded = true;
            } catch (UnsatisfiedLinkError e) {
                loadError = "libselftest_jni.so did not load: " + e.getMessage();
            }
        }
        return loadError;
    }

    /**
     * Runs the test program in library {@code library} (for example "libst_unit.so") in a child process: working folder
     * {@code cwd}, stdout and stderr appended to {@code outFile}. Returns the exit code (0 = success), 128 + signal for a
     * crash, -1 when the process could not be started, -2 on time out.
     */
    static native int runProgram(String library, String[] args, String cwd, String outFile, String tmpDir, int timeoutSeconds);

    /** Loads a MIDI file with the portable core. "OK;format=..;tracks=..;..." or "ERROR;type;message". */
    static native String loadMidi(String path);
}
