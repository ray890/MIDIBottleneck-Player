package org.midibottleneck.player;

import android.content.Context;

import org.midibottleneck.core.route.MidiRoute;
import org.midibottleneck.core.route.MidiRoutes;

/**
 * Builds the player's output catalog from {@link MidiRoutes#available}: of the hardware routes (USB, Bluetooth) only
 * the preferred one (native AMidi on the modern flavor, the Java route on android6), so a device is never listed
 * twice; plus every route that is not hardware (the built-in synthesizer). The output list is "None", the hardware
 * devices, then the devices of the other routes. The choice itself is {@link RouteCatalog#choose}.
 */
final class PlayerRoutes {
    private PlayerRoutes() {}

    static RouteCatalog create(Context context) {
        MidiRoute[] all;
        try {
            all = MidiRoutes.available(context);
        } catch (RuntimeException e) {
            all = new MidiRoute[0];   // no routes: the list shows "None" only
        }
        MidiRoute[] chosen = RouteCatalog.choose(all, new RouteCatalog.Picker() {
            @Override public MidiRoute pick(MidiRoute[] hardware) { return MidiRoutes.preferred(hardware); }
        });
        // The direct USB route comes first: USB devices appear there ("... (direct USB)") before the same devices in the
        // Android MIDI service route, so the first USB entry, the default for a plugged-in device, is the direct one.
        MidiRoute usb = null;
        try {
            usb = MidiRoutes.usb(context);
        } catch (RuntimeException ignored) {
        }
        if (usb != null && usb.isAvailable()) {
            MidiRoute[] withUsb = new MidiRoute[chosen.length + 1];
            withUsb[0] = usb;
            System.arraycopy(chosen, 0, withUsb, 1, chosen.length);
            chosen = withUsb;
        }
        return new RouteCatalog(chosen);
    }
}
