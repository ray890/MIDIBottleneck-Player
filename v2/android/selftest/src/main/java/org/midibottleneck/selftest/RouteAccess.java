package org.midibottleneck.selftest;

import android.content.Context;

import org.midibottleneck.core.route.MidiRoute;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * Finds the output routes of the shared core library. The routes come from MidiRoutes.available(Context) (written by the
 * output-route work item); it is looked up by name so this app also builds and runs while that class is missing or still
 * a stub: the result is then an empty list and a note, never a failure of the self-test.
 */
final class RouteAccess {
    private RouteAccess() {}

    static final class Found {
        MidiRoute[] routes = new MidiRoute[0];
        String note = "";
    }

    static Found find(Context context) {
        Found found = new Found();
        try {
            Class<?> factory = Class.forName("org.midibottleneck.core.route.MidiRoutes");
            Method available = factory.getMethod("available", Context.class);
            Object result = available.invoke(null, context.getApplicationContext());
            if (result instanceof MidiRoute[]) found.routes = (MidiRoute[]) result;
            else found.note = "MidiRoutes.available returned " + (result == null ? "null" : result.getClass().getName());
        } catch (ClassNotFoundException | NoSuchMethodException e) {
            found.note = "this build has no MidiRoutes factory yet (" + e.getClass().getSimpleName() + ")";
        } catch (Throwable t) {
            found.note = "MidiRoutes.available failed: " + t;
        }
        return found;
    }

    /** One line per device from MidiRoute.describeDevices() when the route has it (RouteDevice fields are read by name). */
    static String[] describeDevices(MidiRoute route) {
        try {
            Method describe = route.getClass().getMethod("describeDevices");
            Object result = describe.invoke(route);
            if (result instanceof Object[]) {
                Object[] devices = (Object[]) result;
                String[] lines = new String[devices.length];
                for (int i = 0; i < devices.length; i++) lines[i] = describeOne(devices[i]);
                return lines;
            }
        } catch (NoSuchMethodException e) {
            // route without details: the plain names are used
        } catch (Throwable t) {
            return new String[] { "(describeDevices failed: " + t + ")" };
        }
        return null;
    }

    private static String describeOne(Object device) {
        StringBuilder text = new StringBuilder();
        for (Field field : device.getClass().getFields()) {
            if (Modifier.isStatic(field.getModifiers())) continue;
            try {
                if (text.length() > 0) text.append(", ");
                text.append(field.getName()).append('=').append(field.get(device));
            } catch (Throwable ignored) {
                // skip
            }
        }
        try {
            Method details = device.getClass().getMethod("details");
            Object more = details.invoke(device);
            if (more != null) text.append(" | ").append(more);
        } catch (Throwable ignored) {
            // no details()
        }
        return text.length() == 0 ? String.valueOf(device) : text.toString();
    }
}
