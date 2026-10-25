package org.midibottleneck.core.route;

/**
 * Raw MIDI bytes to USB-MIDI 1.0 event packets (4 bytes each: cable number in the high nibble and Code Index Number in
 * the low nibble of byte 0, then up to three MIDI bytes). Channel messages and system common messages become one packet
 * each (CIN 8 to E, 2, 3, 5), real-time bytes one packet (CIN F, also inside a SysEx message), SysEx is cut into packets
 * of three bytes (CIN 4) and ends with CIN 5, 6 or 7 (one, two or three bytes including F7).
 *
 * The encoder keeps state between calls (a SysEx message or a short message may continue in the next write, running
 * status is understood), so the bytes of one write are never reordered. Bytes that cannot be part of a valid message
 * (a data byte with no status) are not sent and are counted in {@link #malformed()}; nothing is dropped silently.
 * Plain Java (host-tested), one thread at a time.
 */
public final class UsbMidiPackets {
    private final int cable;
    private boolean inSysex;
    private final byte[] sysex = new byte[3];
    private int sysexCount;
    private int status;          // status of the short message being collected (or running status), 0 = none
    private int need;            // data bytes the status takes
    private int have;
    private final byte[] data = new byte[2];
    private long malformed;
    private long packets;

    public UsbMidiPackets(int cable) { this.cable = cable & 0x0F; }

    public long malformed() { return malformed; }

    public long packets() { return packets; }

    /** Forgets an unfinished message and running status (the core's reset). */
    public void reset() {
        inSysex = false;
        sysexCount = 0;
        status = 0;
        have = 0;
        need = 0;
    }

    /** Bytes of SysEx collected but not yet sent as a packet (tests). */
    public int pendingSysexBytes() { return inSysex ? sysexCount : 0; }

    /** Largest output for {@code inputLength} input bytes. */
    public static int maxOutput(int inputLength) { return inputLength * 4 + 8; }

    /** Encodes {@code length} bytes; writes packets to {@code out} from {@code outOffset}; returns the number of output bytes. */
    public int encode(byte[] in, int offset, int length, byte[] out, int outOffset) {
        int o = outOffset;
        for (int i = offset; i < offset + length; i++) {
            int b = in[i] & 0xFF;
            if (b >= 0xF8) {
                o = put(out, o, 0xF, b, 0, 0);
            } else if (b >= 0x80) {
                if (b == 0xF7) {
                    if (inSysex) {
                        sysex[sysexCount++] = (byte) b;
                        o = put(out, o, sysexCount == 1 ? 5 : sysexCount == 2 ? 6 : 7,
                            sysex[0] & 0xFF, sysexCount > 1 ? sysex[1] & 0xFF : 0, sysexCount > 2 ? sysex[2] & 0xFF : 0);
                        inSysex = false;
                        sysexCount = 0;
                    } else {
                        malformed++;
                    }
                    status = 0;
                    have = 0;
                } else if (b == 0xF0) {
                    if (inSysex) malformed++;   // the earlier SysEx never ended
                    inSysex = true;
                    sysex[0] = (byte) b;
                    sysexCount = 1;
                    status = 0;
                    have = 0;
                } else {
                    if (inSysex) {              // a status byte ends the SysEx without F7: report it
                        malformed++;
                        inSysex = false;
                        sysexCount = 0;
                    }
                    if (b == 0xF4 || b == 0xF5) {
                        malformed++;
                        status = 0;
                        have = 0;
                    } else if (b == 0xF6) {
                        o = put(out, o, 5, b, 0, 0);
                        status = 0;
                        have = 0;
                    } else {
                        status = b;
                        need = dataLength(b);
                        have = 0;
                    }
                }
            } else {
                if (inSysex) {
                    sysex[sysexCount++] = (byte) b;
                    if (sysexCount == 3) {
                        o = put(out, o, 4, sysex[0] & 0xFF, sysex[1] & 0xFF, sysex[2] & 0xFF);
                        sysexCount = 0;
                    }
                } else if (status != 0) {
                    data[have++] = (byte) b;
                    if (have == need) {
                        o = put(out, o, cin(status), status, data[0] & 0xFF, need > 1 ? data[1] & 0xFF : 0);
                        have = 0;
                        if (status >= 0xF0) status = 0;   // system common messages have no running status
                    }
                } else {
                    malformed++;
                }
            }
        }
        return o - outOffset;
    }

    private int put(byte[] out, int o, int cin, int b1, int b2, int b3) {
        out[o] = (byte) ((cable << 4) | cin);
        out[o + 1] = (byte) b1;
        out[o + 2] = (byte) b2;
        out[o + 3] = (byte) b3;
        packets++;
        return o + 4;
    }

    private static int dataLength(int status) {
        if (status < 0xF0) {
            int type = status & 0xF0;
            return type == 0xC0 || type == 0xD0 ? 1 : 2;
        }
        return status == 0xF2 ? 2 : 1;   // F1 and F3: one data byte
    }

    private static int cin(int status) {
        if (status < 0xF0) return status >> 4;
        return status == 0xF2 ? 3 : 2;   // F1 and F3: two-byte system common; F2: three-byte
    }
}
