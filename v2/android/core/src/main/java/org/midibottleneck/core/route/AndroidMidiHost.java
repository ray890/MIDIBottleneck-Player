package org.midibottleneck.core.route;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The part both routes share: listing the MIDI devices through MidiManager, opening one (the system opens it
 * asynchronously, so open() waits up to {@link #OPEN_TIMEOUT_SECONDS} seconds on a background looper) and telling the
 * route when the open device is unplugged.
 *
 * Permission: none is needed. Android itself opens a class-compliant USB MIDI device when it is plugged in and offers it
 * to every app through MidiManager; there is no USB permission dialog and no manifest permission. (Bluetooth MIDI
 * devices appear here once the system has connected them; pairing is done in the system's Bluetooth settings.)
 */
final class AndroidMidiHost {
    static final int OPEN_TIMEOUT_SECONDS = 5;

    interface Events {
        void onDevicesChanged();
        void onDeviceRemoved(int deviceId);
    }

    private final MidiManager manager;
    private final boolean hasFeature;
    private final HandlerThread thread;
    private final Handler handler;
    private final MidiManager.DeviceCallback callback;
    private volatile Events events;

    private final List<RouteDevice> snapshot = new ArrayList<RouteDevice>();
    private final List<MidiDeviceInfo> snapshotInfos = new ArrayList<MidiDeviceInfo>();

    private MidiDevice device;
    private int deviceId = -1;
    private int portNumber = -1;
    private String openedName = "";

    AndroidMidiHost(Context context) {
        Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        hasFeature = app.getPackageManager().hasSystemFeature(PackageManager.FEATURE_MIDI);
        manager = hasFeature ? (MidiManager) app.getSystemService(Context.MIDI_SERVICE) : null;
        thread = new HandlerThread("midibn-midi-host");
        thread.start();
        handler = new Handler(thread.getLooper());
        callback = new MidiManager.DeviceCallback() {
            @Override public void onDeviceAdded(MidiDeviceInfo info) {
                Events e = events;
                if (e != null) e.onDevicesChanged();
            }
            @Override public void onDeviceRemoved(MidiDeviceInfo info) {
                Events e = events;
                if (e != null) {
                    e.onDeviceRemoved(info.getId());
                    e.onDevicesChanged();
                }
            }
        };
        if (manager != null) manager.registerDeviceCallback(callback, handler);
    }

    boolean isAvailable() { return manager != null; }

    void setEvents(Events events) { this.events = events; }

    /** Stops listening and the background thread. The host cannot be used afterwards. */
    void release() {
        closeDevice();
        if (manager != null) manager.unregisterDeviceCallback(callback);
        thread.quitSafely();
    }

    /** Reads the device list again: one entry per output port (a port the player sends to). Replaces the snapshot. */
    @SuppressWarnings("deprecation")
    synchronized List<RouteDevice> refresh() {
        snapshot.clear();
        snapshotInfos.clear();
        if (manager == null) return snapshot;
        MidiDeviceInfo[] infos = manager.getDevices();
        if (infos == null) return snapshot;
        for (MidiDeviceInfo info : infos) {
            if (info.isPrivate()) continue;                       // another app's private device
            int ports = info.getInputPortCount();                  // the device's input ports are where we send
            if (ports <= 0) continue;
            Bundle p = info.getProperties();
            String name = text(p, MidiDeviceInfo.PROPERTY_NAME);
            String manufacturer = text(p, MidiDeviceInfo.PROPERTY_MANUFACTURER);
            String product = text(p, MidiDeviceInfo.PROPERTY_PRODUCT);
            if (name.isEmpty()) name = !product.isEmpty() ? product : "MIDI device " + info.getId();
            String typeName = MidiCodec.typeName(info.getType());
            MidiDeviceInfo.PortInfo[] portInfos = info.getPorts();
            for (MidiDeviceInfo.PortInfo port : portInfos) {
                if (port.getType() != MidiDeviceInfo.PortInfo.TYPE_INPUT) continue;
                snapshot.add(new RouteDevice(snapshot.size(), info.getId(), name, manufacturer, product, typeName,
                    port.getPortNumber(), port.getName() == null ? "" : port.getName(), ports));
                snapshotInfos.add(info);
            }
        }
        return new ArrayList<RouteDevice>(snapshot);
    }

    synchronized List<RouteDevice> snapshot() { return new ArrayList<RouteDevice>(snapshot); }

    /** Opens entry {@code index} of the last refresh(). Returns null on success, else an error text. Do not call on this host's own thread. */
    String open(int index) {
        closeDevice();
        MidiDeviceInfo info;
        RouteDevice entry;
        synchronized (this) {
            if (manager == null) return "This phone has no MIDI support.";
            if (index < 0 || index >= snapshotInfos.size()) return "That MIDI device is not in the list. Refresh the list and choose again.";
            info = snapshotInfos.get(index);
            entry = snapshot.get(index);
        }
        final CountDownLatch done = new CountDownLatch(1);
        final AtomicReference<MidiDevice> result = new AtomicReference<MidiDevice>();
        manager.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override public void onDeviceOpened(MidiDevice opened) {
                result.set(opened);
                done.countDown();
            }
        }, handler);
        try {
            if (!done.await(OPEN_TIMEOUT_SECONDS, TimeUnit.SECONDS))
                return "The MIDI device did not answer within " + OPEN_TIMEOUT_SECONDS + " seconds. Unplug it and plug it in again.";
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "Opening the MIDI device was interrupted.";
        }
        MidiDevice opened = result.get();
        if (opened == null)
            return "Android could not open \"" + entry.displayName() + "\". It may have been unplugged, or another app may be using it.";
        synchronized (this) {
            device = opened;
            deviceId = entry.deviceId;
            portNumber = entry.portNumber;
            openedName = entry.displayName();
        }
        return null;
    }

    synchronized MidiDevice device() { return device; }
    synchronized int portNumber() { return portNumber; }
    synchronized int deviceId() { return deviceId; }
    synchronized String openedName() { return openedName; }

    void closeDevice() {
        MidiDevice d;
        synchronized (this) {
            d = device;
            device = null;
            deviceId = -1;
            portNumber = -1;
        }
        if (d != null) {
            try { d.close(); } catch (java.io.IOException | RuntimeException ignored) { }
        }
    }

    private static String text(Bundle bundle, String key) {
        if (bundle == null) return "";
        String value = bundle.getString(key);
        return value == null ? "" : value.trim();
    }
}
