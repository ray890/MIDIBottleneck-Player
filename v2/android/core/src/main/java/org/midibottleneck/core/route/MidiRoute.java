package org.midibottleneck.core.route;

/**
 * The output side of playback, as the Java layer sees it. Mirrors midibn::MidiOutput
 * (core/include/midibn/midi_output.h) in reduced form. Implemented by {@link JavaMidiRoute} (android.media.midi),
 * {@link NativeMidiRoute} (AMidi, API 29+) and {@link FakeMidiRoute} (tests); get the real ones from
 * {@link MidiRoutes#available(android.content.Context)}.
 *
 * Threading: send* and the batch calls come from one output thread at a time; the rest from the UI thread. {@code open}
 * blocks for up to a few seconds (the system opens the device asynchronously), so call it off the UI thread when
 * possible. Failures never throw: a send returns false and {@link #lastError()} gives the plain sentence.
 *
 * The interface only grows (default methods), so older callers keep working. The engine (native) does not call these
 * methods for playback: once a route is open it is attached to the native engine (see RouteNative), and the engine's
 * MidiOutput reaches the same device.
 */
public interface MidiRoute {
    /** Short name shown to the user, e.g. "Java MIDI (android.media.midi)". */
    String name();

    /** True when this route can work on this device. */
    boolean isAvailable();

    /** Display names of the output devices, in a stable order (a fresh reading each call). */
    String[] listDevices();

    /** Opens device number {@code index} of the last {@link #listDevices()} result. Returns null on success, else an error text. */
    String open(int index);

    void close();

    boolean isOpen();

    /** Sends one short message packed as status | data1 << 8 | data2 << 16 (the WinMM layout). */
    boolean sendShort(int packed);

    /** Sends one SysEx fragment (complete message or segment). */
    boolean sendLong(byte[] data, int offset, int length);

    /** Optional batching: messages between the two calls are sent together, with one write to the device. */
    void beginBatch();

    void endBatch();

    /** All notes off and controllers reset, on every channel (CC120, CC123, CC64 = 0 per channel, one write). */
    void panic();

    // ---------------------------------------------------------------- additions of U12 (all default methods)

    /** Details of the output devices, same order and count as {@link #listDevices()}. */
    default RouteDevice[] describeDevices() {
        String[] names = listDevices();
        RouteDevice[] devices = new RouteDevice[names.length];
        for (int i = 0; i < names.length; i++) {
            devices[i] = new RouteDevice(i, i, names[i], "", "", "Other", 0, "", 1);
        }
        return devices;
    }

    /** Sends raw MIDI bytes (whole messages) with one write. The native engine calls this once per burst. */
    default boolean sendBytes(byte[] data, int offset, int length) { return false; }

    /** Plain sentence for the last failure of any call, or null. */
    default String lastError() { return null; }

    /** The core's reset: discard unsent bytes, unfinished SysEx and whatever the device side has queued. */
    default void discardPending() {}

    /** True after the device disappeared (unplugged) or a send failed; cleared by close() and open(). */
    default boolean isLost() { return false; }

    /**
     * True for routes that reach real MIDI devices (USB, Bluetooth), false for a route that makes sound in the app
     * itself (the built-in synthesizer). The player lists one hardware route (the preferred one, so no device appears
     * twice) and every non-hardware route.
     */
    default boolean isHardware() { return true; }

    /**
     * True for the direct USB route ({@link UsbMidiRoute}): its entries are USB MIDI devices driven without Android's MIDI
     * service. The player prefers the first such entry when a USB device is plugged in.
     */
    default boolean isDirectUsb() { return false; }

    /** Called on a background thread when the device list changes or the open device is lost. May be null. */
    default void setListener(Listener listener) {}

    interface Listener {
        /** A device was plugged in or removed: read {@link #listDevices()} again. */
        void onDevicesChanged();

        /** The open device went away (unplugged); the route is lost and {@code message} says so. */
        void onDeviceLost(String message);
    }
}
