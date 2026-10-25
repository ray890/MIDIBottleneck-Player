package org.midibottleneck.core.route;

/**
 * Pure message encoding shared by the Java side of the routes (no Android classes, so a plain JVM test can run it).
 * Mirrors midibn::android::encode_short_message in platform/android/route/route_output.cpp.
 */
public final class MidiCodec {
    private MidiCodec() {}

    /** Largest single write handed to a port; whole messages are never split. */
    public static final int MAX_WRITE = 1000;

    /**
     * Packed short message (status | data1 << 8 | data2 << 16) to raw bytes at {@code out[off..]}. Returns the byte count
     * (1 to 3), or 0 when the value is not a sendable message (not a status byte, or F0, F4, F5, F7, F9, FD).
     */
    public static int encodeShort(int packed, byte[] out, int off) {
        int status = packed & 0xFF;
        int d1 = (packed >> 8) & 0x7F;
        int d2 = (packed >> 16) & 0x7F;
        if (status < 0x80) return 0;
        out[off] = (byte) status;
        if (status < 0xF0) {
            int type = status & 0xF0;
            out[off + 1] = (byte) d1;
            if (type == 0xC0 || type == 0xD0) return 2;
            out[off + 2] = (byte) d2;
            return 3;
        }
        switch (status) {
            case 0xF1:
            case 0xF3:
                out[off + 1] = (byte) d1;
                return 2;
            case 0xF2:
                out[off + 1] = (byte) d1;
                out[off + 2] = (byte) d2;
                return 3;
            case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF:
                return 1;
            default:
                return 0;
        }
    }

    /** The panic sequence of the core (v1): per channel CC120, CC123, CC64 = 0; 48 messages, 144 bytes. */
    public static byte[] panicBytes() {
        final int[] controllers = {120, 123, 64};
        byte[] bytes = new byte[48 * 3];
        for (int i = 0; i < 48; i++) {
            bytes[i * 3] = (byte) (0xB0 | (i / 3));
            bytes[i * 3 + 1] = (byte) controllers[i % 3];
            bytes[i * 3 + 2] = 0;
        }
        return bytes;
    }

    /** android.media.midi.MidiDeviceInfo type constants: 1 USB, 2 virtual, 3 Bluetooth. */
    public static String typeName(int type) {
        switch (type) {
            case 1: return "USB";
            case 2: return "Virtual";
            case 3: return "Bluetooth";
            default: return "Other (" + type + ")";
        }
    }
}
