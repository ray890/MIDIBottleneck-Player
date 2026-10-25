package org.midibottleneck.player;

import android.content.Context;

import org.midibottleneck.core.PlayerBridge;

/**
 * Puts the remembered choices of the main screen back (the MIDI output and the six toggles of the menu) and writes them
 * when the user changes them. One per process; the main screen calls {@link #step} after every redraw while something is
 * still pending. The list of what is remembered is in {@link Prefs}. A remembered output is chosen only when its entry is in
 * the output list (a USB device can show up a moment after the app starts, so the step keeps trying until the user picks an
 * output); a remembered toggle is applied once it is enabled, while stopped.
 */
final class Remembered {
    private static Remembered instance;

    static synchronized Remembered get(Context context, PlayerHost host) {
        if (instance == null) instance = new Remembered(context.getApplicationContext(), host);
        return instance;
    }

    static final int TOGGLES = 6;
    private final Prefs prefs;
    private final PlayerHost host;
    private String pendingOutput;                                   // entry text, or null
    private final Boolean[] pendingToggle = new Boolean[TOGGLES];   // null = nothing to do

    private Remembered(Context app, PlayerHost host) {
        this.host = host;
        this.prefs = AppPrefs.get(app);
        String saved = prefs.getString(Prefs.OUTPUT_ITEM, "");
        pendingOutput = saved.isEmpty() ? null : saved;
        for (int i = 0; i < TOGGLES; i++) pendingToggle[i] = prefs.getBool(Prefs.MENU_TOGGLE + i);
    }

    /** True while a remembered value is waiting to be applied. */
    boolean pending() {
        if (pendingOutput != null) return true;
        for (Boolean t : pendingToggle) if (t != null) return true;
        return false;
    }

    /** Applies what can be applied now; returns true when it changed the player (the caller redraws). */
    boolean step() {
        PlayerBridge b = host.bridge();
        if (b == null || b.n[PlayerBridge.N_STATE] != PlayerBridge.STATE_STOPPED || b.n[PlayerBridge.N_LOADING] != 0) return false;
        boolean changed = false;
        if (pendingOutput != null && b.n[PlayerBridge.N_OUTPUT_ENABLED] != 0) {
            String[] items = items(b);
            for (int i = 0; i < items.length; i++) {
                if (!items[i].equals(pendingOutput)) continue;
                pendingOutput = null;
                if (i != b.n[PlayerBridge.N_OUTPUT_SELECTED]) { host.selectOutput(i); changed = true; b = host.bridge(); }
                break;
            }
        }
        for (int i = 0; i < TOGGLES; i++) {
            Boolean want = pendingToggle[i];
            if (want == null || !enabled(b, i)) continue;
            pendingToggle[i] = null;
            if (state(b, i) != want) { flip(b, i); changed = true; }
        }
        if (changed) host.refresh();
        return changed;
    }

    /** The user chose entry {@code position} of the output list (call before the choice is passed on). */
    void userChoseOutput(int position) {
        pendingOutput = null;                 // the user's own choice wins over a remembered one
        String[] items = items(host.bridge());
        if (position >= 0 && position < items.length) prefs.putString(Prefs.OUTPUT_ITEM, items[position]);
    }

    /** The user flipped menu toggle {@code which}: flips it in the core, redraws and remembers the new state. */
    void userToggled(int which) {
        PlayerBridge b = host.bridge();
        flip(b, which);
        pendingToggle[which] = null;
        host.refresh();
        prefs.putBool(Prefs.MENU_TOGGLE + which, state(host.bridge(), which));
    }

    private static String[] items(PlayerBridge b) {
        String joined = b.t[PlayerBridge.T_OUTPUT_ITEMS];
        return joined == null || joined.isEmpty() ? new String[0] : joined.split("\n", -1);
    }

    static boolean state(PlayerBridge b, int which) {
        switch (which) {
            case 0: return b.n[PlayerBridge.N_APPLY_QUEUE_CHECKED] != 0;
            case 1: return b.n[PlayerBridge.N_QUEUE_AGE_CHECKED] != 0;
            case 2: return b.n[PlayerBridge.N_CHASE_CHECKED] != 0;
            case 3: return b.n[PlayerBridge.N_PRESERVE_CHECKED] != 0;
            case 4: return b.n[PlayerBridge.N_PACED_CHECKED] != 0;
            default: return b.n[PlayerBridge.N_SCALE_CHECKED] != 0;
        }
    }

    static boolean enabled(PlayerBridge b, int which) {
        switch (which) {
            case 0: return b.n[PlayerBridge.N_APPLY_QUEUE_ENABLED] != 0;
            case 1: return b.n[PlayerBridge.N_QUEUE_AGE_ENABLED] != 0;
            case 2: return b.n[PlayerBridge.N_CHASE_ENABLED] != 0;
            case 3: return b.n[PlayerBridge.N_PRESERVE_ENABLED] != 0;
            case 4: return b.n[PlayerBridge.N_PACED_ENABLED] != 0;
            default: return b.n[PlayerBridge.N_SCALE_ENABLED] != 0;
        }
    }

    private static void flip(PlayerBridge b, int which) {
        switch (which) {
            case 0: b.toggleApplyQueueLimitWithoutSlowdown(); break;
            case 1: b.toggleQueueAgeLimit(); break;
            case 2: b.toggleStateChase(); break;
            case 3: b.togglePreservePauseSeek(); break;
            case 4: b.toggleOutputPacedTimeline(); break;
            default: b.toggleScaleNoteIntervalByTraffic(); break;
        }
    }
}
