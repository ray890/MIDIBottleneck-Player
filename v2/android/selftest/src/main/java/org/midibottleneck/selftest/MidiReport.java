package org.midibottleneck.selftest;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Bundle;

import org.midibottleneck.core.route.MidiRoute;

/** The list of MIDI devices with their details: through the shared routes and, independently, straight from Android. */
final class MidiReport {
    private MidiReport() {}

    /** Writes midi-devices.txt and adds one INFO or FAIL check. */
    static void run(Context context, Results results) {
        StringBuilder text = new StringBuilder("MIDI DEVICES (information; no port is opened, nothing is sent)\n");
        int total = 0;
        boolean failed = false;

        PackageManager pm = context.getPackageManager();
        text.append("\nFeature android.software.midi: ").append(pm.hasSystemFeature(PackageManager.FEATURE_MIDI)).append('\n');
        text.append("Feature android.hardware.usb.host: ").append(pm.hasSystemFeature(PackageManager.FEATURE_USB_HOST)).append('\n');

        text.append("\n== Through the player's routes (MidiRoute) ==\n");
        RouteAccess.Found found = RouteAccess.find(context);
        if (!found.note.isEmpty()) text.append("Note: ").append(found.note).append('\n');
        if (found.routes.length == 0) text.append("No routes available in this build.\n");
        for (MidiRoute route : found.routes) {
            text.append("\nRoute: ").append(route.name()).append("  (available: ").append(route.isAvailable()).append(")\n");
            try {
                String[] names = route.listDevices();
                String[] details = RouteAccess.describeDevices(route);
                if (names == null || names.length == 0) text.append("  no output devices listed\n");
                else {
                    for (int i = 0; i < names.length; i++) {
                        text.append("  [").append(i).append("] ").append(names[i]).append('\n');
                        if (details != null && i < details.length) text.append("      ").append(details[i]).append('\n');
                    }
                    total += names.length;
                }
            } catch (Throwable t) {
                failed = true;
                text.append("  listing failed: ").append(t).append('\n');
            }
        }

        text.append("\n== Straight from Android (android.media.midi.MidiManager) ==\n");
        try {
            MidiManager manager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);
            if (manager == null) text.append("No MidiManager on this phone.\n");
            else {
                MidiDeviceInfo[] infos = manager.getDevices();
                if (infos.length == 0) text.append("No MIDI devices connected.\n");
                for (MidiDeviceInfo info : infos) {
                    Bundle p = info.getProperties();
                    text.append("\nDevice id ").append(info.getId()).append(": type ").append(typeName(info.getType()))
                            .append(", input ports ").append(info.getInputPortCount())
                            .append(", output ports ").append(info.getOutputPortCount())
                            .append(", private ").append(info.isPrivate()).append('\n');
                    text.append("  name: ").append(p.getString(MidiDeviceInfo.PROPERTY_NAME)).append('\n');
                    text.append("  manufacturer: ").append(p.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER)).append('\n');
                    text.append("  product: ").append(p.getString(MidiDeviceInfo.PROPERTY_PRODUCT)).append('\n');
                    text.append("  version: ").append(p.getString(MidiDeviceInfo.PROPERTY_VERSION)).append('\n');
                    for (MidiDeviceInfo.PortInfo port : info.getPorts()) {
                        text.append("  port ").append(port.getPortNumber()).append(": ")
                                .append(port.getType() == MidiDeviceInfo.PortInfo.TYPE_INPUT ? "input (the device receives)" : "output (the device sends)")
                                .append(", name \"").append(port.getName()).append("\"\n");
                    }
                }
            }
        } catch (Throwable t) {
            text.append("MidiManager query failed: ").append(t).append('\n');
        }

        results.write("midi-devices.txt", text.toString());
        if (failed) results.add("MIDI device list", Results.Status.FAIL, "a route threw while listing devices; see midi-devices.txt");
        else if (total == 0) results.add("MIDI device list", Results.Status.INFO,
                "no MIDI output device listed (expected when no USB MIDI device is plugged in); see midi-devices.txt");
        else results.add("MIDI device list", Results.Status.INFO, total + " device entr" + (total == 1 ? "y" : "ies") + " listed; see midi-devices.txt");
    }

    private static String typeName(int type) {
        switch (type) {
            case MidiDeviceInfo.TYPE_USB: return "USB";
            case MidiDeviceInfo.TYPE_VIRTUAL: return "virtual";
            case MidiDeviceInfo.TYPE_BLUETOOTH: return "Bluetooth";
            default: return "unknown(" + type + ")";
        }
    }
}
