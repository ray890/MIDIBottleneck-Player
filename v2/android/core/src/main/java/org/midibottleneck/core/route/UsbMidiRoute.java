package org.midibottleneck.core.route;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;

import java.util.ArrayList;
import java.util.List;

/**
 * The direct USB route: drives a class-compliant USB MIDI 1.0 device (interface class 1 "audio", subclass 3 "MIDI
 * streaming", with a bulk OUT endpoint) through {@link UsbManager} alone, without Android's MIDI service. Reasons: that
 * service opens the device's ALSA ports in a way that makes some devices hang every app that uses it (the "USB2.0-MIDI"
 * cable with a CH345 chip does), and a bulk transfer returns when the device has taken the data, so the time inside each
 * write is the real time the device needed.
 *
 * Sending: the engine's bytes go to {@link UsbMidiSender} (USB-MIDI packets, one sender thread, bulk transfers of one
 * endpoint packet, finite timeout). Cable 0 only. Permission: Android asks the user once per device (or the intent filter
 * for USB_DEVICE_ATTACHED grants it when the device is plugged in and the player chosen); until then {@link #open}
 * returns {@link #PERMISSION_PENDING} and the route reports a device change when the answer is in.
 *
 * Works from Android 6 (both flavors); nothing here needs the native library except attaching to the engine's output.
 */
public final class UsbMidiRoute implements MidiRoute, ByteSink {
    /** Same text as the player's catalog constant: the dialog is open, not an error. */
    public static final String PERMISSION_PENDING = "Waiting for permission to use the USB device.";

    private static final int USB_CLASS_AUDIO = 1;
    private static final int USB_SUBCLASS_MIDISTREAMING = 3;
    private static final int BULK_TIMEOUT_MS = 1000;
    private static final int QUEUE_BYTES = 4096;
    private static final int STUCK_MS = 1000;

    private final Context context;
    private final UsbManager manager;
    private final String permissionAction;
    private final MidiBatcher batcher = new MidiBatcher(this);
    private volatile Listener listener;
    private volatile UsbMidiSender sender;
    private volatile UsbDevice openDevice;
    private volatile String error;
    private volatile boolean lost;
    private boolean attachedToEngine;
    private boolean receiverRegistered;
    private List<Found> found = new ArrayList<Found>();
    private String openedName = "";

    /** One usable USB MIDI output found on the bus. */
    private static final class Found {
        final UsbDevice device;
        final UsbInterface iface;
        final UsbEndpoint out;
        final String name;

        Found(UsbDevice device, UsbInterface iface, UsbEndpoint out, String name) {
            this.device = device;
            this.iface = iface;
            this.out = out;
            this.name = name;
        }
    }

    public UsbMidiRoute(Context context) {
        this.context = context.getApplicationContext();
        UsbManager m = null;
        try {
            m = (UsbManager) this.context.getSystemService(Context.USB_SERVICE);
        } catch (RuntimeException ignored) {
        }
        this.manager = m;
        this.permissionAction = this.context.getPackageName() + ".USB_MIDI_PERMISSION";
    }

    @Override public String name() { return "Direct USB MIDI"; }

    @Override public boolean isDirectUsb() { return true; }

    @Override public boolean isAvailable() {
        if (manager == null) return false;
        try {
            return context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_USB_HOST);
        } catch (RuntimeException e) {
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------ the device list

    private List<Found> scan() {
        List<Found> list = new ArrayList<Found>();
        if (manager == null) return list;
        java.util.HashMap<String, UsbDevice> devices;
        try {
            devices = manager.getDeviceList();
        } catch (RuntimeException e) {
            return list;
        }
        List<String> keys = new ArrayList<String>(devices.keySet());
        java.util.Collections.sort(keys);
        for (String key : keys) {
            UsbDevice d = devices.get(key);
            for (int i = 0; i < d.getInterfaceCount(); i++) {
                UsbInterface iface = d.getInterface(i);
                if (iface.getInterfaceClass() != USB_CLASS_AUDIO || iface.getInterfaceSubclass() != USB_SUBCLASS_MIDISTREAMING) continue;
                UsbEndpoint out = null;
                for (int e = 0; e < iface.getEndpointCount() && out == null; e++) {
                    UsbEndpoint ep = iface.getEndpoint(e);
                    if (ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep.getDirection() == UsbConstants.USB_DIR_OUT) out = ep;
                }
                if (out != null) {
                    list.add(new Found(d, iface, out, displayName(d) + " (direct USB)"));
                    break;   // one entry per device
                }
            }
        }
        return list;
    }

    private static String displayName(UsbDevice d) {
        String product = null;
        try {
            if (Build.VERSION.SDK_INT >= 21) product = d.getProductName();
        } catch (RuntimeException ignored) {
        }
        if (product != null && !product.trim().isEmpty()) return product.trim();
        return String.format("USB MIDI device %04x:%04x", d.getVendorId(), d.getProductId());
    }

    @Override public String[] listDevices() {
        ensureReceiver();
        found = scan();
        String[] names = new String[found.size()];
        for (int i = 0; i < names.length; i++) names[i] = found.get(i).name;
        return names;
    }

    @Override public RouteDevice[] describeDevices() {
        listDevices();
        RouteDevice[] devices = new RouteDevice[found.size()];
        for (int i = 0; i < devices.length; i++) {
            UsbDevice d = found.get(i).device;
            devices[i] = new RouteDevice(i, d.getDeviceId(), found.get(i).name, "", "", "USB", 0, "", 1);
        }
        return devices;
    }

    // ----------------------------------------------------------------------------------------- open and close

    @Override public String open(int index) {
        close();
        error = null;
        lost = false;
        ensureReceiver();
        if (index < 0 || index >= found.size()) found = scan();
        if (index < 0 || index >= found.size()) return "That USB MIDI device is no longer available. Choose the output again.";
        Found f = found.get(index);
        if (manager == null) return "This phone does not offer USB host mode.";
        if (!manager.hasPermission(f.device)) {
            requestPermission(f.device);
            return PERMISSION_PENDING;
        }
        UsbDeviceConnection connection = manager.openDevice(f.device);
        if (connection == null) return "The USB device could not be opened. Unplug it and plug it in again.";
        if (!connection.claimInterface(f.iface, true)) {
            connection.close();
            return "Another app or the system is using the USB device and would not let go. Close the other app, or unplug the device and plug it in again.";
        }
        RouteNative.claimRoute(this);
        batcher.clear();
        int packet = f.out.getMaxPacketSize();
        UsbMidiSender s = new UsbMidiSender(new ConnectionTransport(connection, f.iface, f.out), 0, packet <= 0 ? 32 : packet,
            BULK_TIMEOUT_MS, QUEUE_BYTES, STUCK_MS, new UsbMidiSender.Events() {
                @Override public void onFailed(String message) { deviceGone(message, true); }
            });
        sender = s;
        openDevice = f.device;
        openedName = f.name;
        if (RouteNative.nativeAvailable()) {
            String problem = RouteNative.attachJava(this);
            if (problem != null) {
                detachAll();
                RouteNative.releaseRoute(this);
                return problem;
            }
            attachedToEngine = true;
        }
        return null;
    }

    @Override public void close() {
        detachAll();
        RouteNative.releaseRoute(this);
    }

    private void detachAll() {
        if (attachedToEngine) {
            attachedToEngine = false;
            RouteNative.detach();
        }
        UsbMidiSender s = sender;
        sender = null;
        openDevice = null;
        batcher.reset();
        if (s != null) s.close(300);   // lets the last messages (the panic) out, then releases the interface off-thread
    }

    @Override public boolean isOpen() { return sender != null; }

    @Override public boolean isLost() { return lost; }

    @Override public String lastError() { return error; }

    @Override public void setListener(Listener l) { listener = l; }

    // ----------------------------------------------------------------------------------------------- sending

    @Override public boolean sendBytes(byte[] data, int offset, int length) { return write(data, offset, length); }

    @Override public boolean write(byte[] data, int offset, int length) {
        UsbMidiSender s = sender;
        if (s == null) {
            if (error == null) error = "No MIDI output device is open.";
            return false;
        }
        if (s.send(data, offset, length)) return true;
        String f = s.failure();
        error = f != null ? f : "The USB MIDI device is closed.";
        return false;
    }

    @Override public void discardPending() {
        UsbMidiSender s = sender;
        batcher.reset();
        if (s != null) s.discardPending();
    }

    @Override public boolean sendShort(int packed) { return sender != null && batcher.sendShort(packed); }

    @Override public boolean sendLong(byte[] data, int offset, int length) { return sender != null && batcher.sendLong(data, offset, length); }

    @Override public void beginBatch() { batcher.beginBatch(); }

    @Override public void endBatch() { if (sender != null) batcher.endBatch(); }

    @Override public void panic() { if (sender != null) batcher.panic(); }

    // ----------------------------------------------------------------------------- unplugging and permission

    /** The device failed or went away: later sends fail with {@code message}, the player is told. */
    private void deviceGone(String message, boolean tellListener) {
        lost = true;
        error = message;
        batcher.markLost(message);
        if (attachedToEngine) RouteNative.setLost(message);
        Listener l = listener;
        if (tellListener && l != null) l.onDeviceLost(message);
    }

    private void requestPermission(UsbDevice device) {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;   // the system adds the answer to the intent
        Intent intent = new Intent(permissionAction).setPackage(context.getPackageName());
        PendingIntent pending = PendingIntent.getBroadcast(context, 0, intent, flags);
        try {
            manager.requestPermission(device, pending);
        } catch (RuntimeException ignored) {
        }
    }

    private synchronized void ensureReceiver() {
        if (receiverRegistered) return;
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(permissionAction);
        try {
            if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
            else context.registerReceiver(receiver, filter);
            receiverRegistered = true;
        } catch (RuntimeException ignored) {
        }
    }

    /** Frees the broadcast receiver. The route is not used afterwards. */
    public synchronized void release() {
        close();
        if (receiverRegistered) {
            receiverRegistered = false;
            try { context.unregisterReceiver(receiver); } catch (RuntimeException ignored) { }
        }
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent intent) {
            String action = intent.getAction();
            if (action == null) return;
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            Listener l = listener;
            if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                UsbDevice mine = openDevice;
                if (mine != null && device != null && mine.getDeviceId() == device.getDeviceId()) {
                    deviceGone("The USB MIDI device \"" + openedName + "\" was unplugged. Choose the output again.", true);
                    UsbMidiSender s = sender;
                    sender = null;
                    openDevice = null;
                    if (s != null) s.close(0);
                }
                if (l != null) l.onDevicesChanged();
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                if (l != null) l.onDevicesChanged();
            } else if (permissionAction.equals(action)) {
                if (l != null) l.onDevicesChanged();   // the answer is in: the player opens the device again
            }
        }
    };

    // ------------------------------------------------------------------------------------- the real transport

    /** UsbDeviceConnection.bulkTransfer on the MIDI OUT endpoint. */
    private static final class ConnectionTransport implements UsbTransport {
        private final UsbDeviceConnection connection;
        private final UsbInterface iface;
        private final UsbEndpoint out;

        ConnectionTransport(UsbDeviceConnection connection, UsbInterface iface, UsbEndpoint out) {
            this.connection = connection;
            this.iface = iface;
            this.out = out;
        }

        @Override public int bulkOut(byte[] data, int offset, int length, int timeoutMs) {
            if (Build.VERSION.SDK_INT >= 18) return connection.bulkTransfer(out, data, offset, length, timeoutMs);
            byte[] copy = new byte[length];
            System.arraycopy(data, offset, copy, 0, length);
            return connection.bulkTransfer(out, copy, length, timeoutMs);
        }

        @Override public void close() {
            try { connection.releaseInterface(iface); } catch (RuntimeException ignored) { }
            try { connection.close(); } catch (RuntimeException ignored) { }
        }
    }
}
