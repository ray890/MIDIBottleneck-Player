package org.midibottleneck.core.route;

/** The bulk OUT pipe of a USB MIDI device (UsbDeviceConnection.bulkTransfer in the app; a fake in the tests). */
public interface UsbTransport {
    /**
     * One transfer of up to {@code length} bytes; waits for the device (bulk transfers complete when the device took the
     * data) at most {@code timeoutMs}. Returns the bytes transferred, or a negative value on error or timeout.
     */
    int bulkOut(byte[] data, int offset, int length, int timeoutMs);

    /** Releases the interface and closes the connection. May be slow; the sender calls it off every important thread. */
    void close();
}
