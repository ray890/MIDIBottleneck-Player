package org.midibottleneck.core.route;

import android.content.Context;
import android.os.Build;

import java.util.ArrayList;
import java.util.List;

/**
 * The routes this build can offer. The Java route is always first; the native (AMidi) route follows when this build
 * contains it (the "modern" flavor) and the phone runs Android 10 or newer; the built-in synthesizer (Sonivox) is always
 * last, in both flavors. Only one route is open at a time (opening one closes the others). Call {@code isAvailable()}
 * on each before offering it.
 */
public final class MidiRoutes {
    private MidiRoutes() {}

    public static MidiRoute[] available(Context context) {
        List<MidiRoute> routes = new ArrayList<MidiRoute>();
        routes.add(new JavaMidiRoute(context));
        if (Build.VERSION.SDK_INT >= 29 && RouteNative.amidiCompiled()) routes.add(new NativeMidiRoute(context));
        routes.add(new SynthMidiRoute());
        return routes.toArray(new MidiRoute[0]);
    }

    /** The direct USB route (not part of {@link #available}: the player adds it in front of the others). */
    public static MidiRoute usb(Context context) { return new UsbMidiRoute(context); }

    /**
     * The route the player should use by default: native on a build that has it, else Java. The built-in synthesizer is
     * never the default (the user chooses it); it is returned only when no other route can work at all.
     */
    public static MidiRoute preferred(MidiRoute[] routes) {
        for (int i = routes.length - 1; i >= 0; i--) {
            if (!(routes[i] instanceof SynthMidiRoute) && routes[i].isAvailable()) return routes[i];
        }
        for (int i = routes.length - 1; i >= 0; i--) if (routes[i].isAvailable()) return routes[i];
        return routes.length > 0 ? routes[0] : null;
    }
}
