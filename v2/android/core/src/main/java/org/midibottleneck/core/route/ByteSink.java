package org.midibottleneck.core.route;

/** The last hop of the Java side: takes raw MIDI bytes (whole messages) and delivers them to the device. */
public interface ByteSink {
    /** One write. False on failure; the plain-sentence reason is then available from {@link #lastError()}. */
    boolean write(byte[] data, int offset, int length);

    String lastError();
}
