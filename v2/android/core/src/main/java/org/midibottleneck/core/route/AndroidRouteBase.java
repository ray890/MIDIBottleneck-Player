package org.midibottleneck.core.route;

import android.content.Context;
import android.media.midi.MidiDevice;

import java.util.List;

/** What the Java route and the native route have in common: device list, opening through {@link AndroidMidiHost}, unplug handling. */
abstract class AndroidRouteBase implements MidiRoute, AndroidMidiHost.Events {
    protected final AndroidMidiHost host;
    private volatile Listener listener;
    private volatile boolean lost;
    private volatile String error;

    AndroidRouteBase(Context context) {
        host = new AndroidMidiHost(context);
        host.setEvents(this);
    }

    /** Takes over the opened device and port; null on success, else an error text. Called by open(). */
    protected abstract String attach(MidiDevice device, int portNumber);

    /** Releases what attach() took (port, native handles); the device itself is closed by the caller. */
    protected abstract void detach();

    protected void setError(String text) { error = text; }

    @Override public boolean isAvailable() { return host.isAvailable(); }

    @Override public String[] listDevices() {
        List<RouteDevice> devices = host.refresh();
        String[] names = new String[devices.size()];
        for (int i = 0; i < names.length; i++) names[i] = devices.get(i).displayName();
        return names;
    }

    @Override public RouteDevice[] describeDevices() {
        List<RouteDevice> devices = host.refresh();
        return devices.toArray(new RouteDevice[0]);
    }

    @Override public String open(int index) {
        close();
        RouteNative.claim(this);   // closes any other open route: the engine has one output
        lost = false;
        error = null;
        String problem = host.open(index);
        if (problem == null) {
            problem = attach(host.device(), host.portNumber());
            if (problem != null) host.closeDevice();
        }
        if (problem != null) {
            error = problem;
            RouteNative.release(this);
        }
        return problem;
    }

    @Override public void close() {
        detach();
        host.closeDevice();
        RouteNative.release(this);
    }

    /** Frees the background looper and the device callback. The route cannot be used afterwards. */
    public void release() {
        close();
        host.release();
    }

    @Override public boolean isLost() { return lost; }
    @Override public String lastError() { return error; }
    @Override public void setListener(Listener l) { listener = l; }

    protected void markLost(String message) {
        lost = true;
        error = message;
    }

    // AndroidMidiHost.Events (background thread)
    @Override public void onDevicesChanged() {
        Listener l = listener;
        if (l != null) l.onDevicesChanged();
    }

    @Override public void onDeviceRemoved(int deviceId) {
        if (deviceId != host.deviceId() || !isOpen()) return;
        String message = "The MIDI device \"" + host.openedName() + "\" was unplugged. Choose the output again.";
        deviceGone(message);
        Listener l = listener;
        if (l != null) l.onDeviceLost(message);
    }

    /** The open device disappeared: later sends fail with {@code message} and nothing throws. */
    protected abstract void deviceGone(String message);
}
