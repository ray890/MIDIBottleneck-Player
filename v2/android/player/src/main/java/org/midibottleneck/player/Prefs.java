package org.midibottleneck.player;

import java.util.ArrayList;
import java.util.List;

/**
 * The player's remembered settings, as plain logic without Android classes (so a plain-Java test can run it). The values
 * live in a {@link Store} (SharedPreferences in the app, see {@link AppPrefs}); every value is kept as text and read back
 * tolerantly: a key that is missing, of another type, damaged or out of range gives the caller's default for that key
 * alone and never stops the app.
 *
 * Remembered (key, default when absent):
 *   output.item          text of the chosen MIDI output entry, restored only when that entry is in the list; "" = normal default
 *   synth.volume         0..200, default 100          synth.voices   1..build maximum, default 64
 *   synth.reverbOn       0/1, default 0               synth.reverbPreset 0..3, default 1     synth.reverbAmount 0..100, default 85
 *   synth.dls.uri, synth.dls.name   the loaded DLS sound bank; "" = built-in sounds
 *   menu.t0 .. menu.t5   the six toggles of the menu; absent = the player's own default
 *   dls.recent           up to {@link #RECENT_MAX} recently loaded DLS sound banks, most recent first
 * Not remembered: the loaded MIDI file, the window/scroll state, which groups are collapsed.
 */
final class Prefs {
    interface Store {
        /** The stored text, or null when there is none (or it cannot be read). Must not throw. */
        String get(String key);
        void put(String key, String value);
        void remove(String key);
    }

    static final String OUTPUT_ITEM = "output.item";
    static final String SYNTH_VOLUME = "synth.volume", SYNTH_VOICES = "synth.voices", SYNTH_REVERB_ON = "synth.reverbOn",
            SYNTH_REVERB_PRESET = "synth.reverbPreset", SYNTH_REVERB_AMOUNT = "synth.reverbAmount",
            SYNTH_DLS_URI = "synth.dls.uri", SYNTH_DLS_NAME = "synth.dls.name";
    static final String MENU_TOGGLE = "menu.t";          // + 0..5
    static final String DLS_RECENT = "dls.recent";
    static final int RECENT_MAX = 8;

    /** One recently loaded sound bank. */
    static final class Recent {
        final String uri, name;
        Recent(String uri, String name) { this.uri = uri; this.name = name; }
        @Override public boolean equals(Object o) { return o instanceof Recent && ((Recent) o).uri.equals(uri); }
        @Override public int hashCode() { return uri.hashCode(); }
    }

    private final Store store;

    Prefs(Store store) { this.store = store; }

    // ---- tolerant reading

    String getString(String key, String def) {
        String s = read(key);
        return s == null ? def : s;
    }

    /** An integer in min..max; missing, damaged or out-of-range text gives {@code def}. */
    int getInt(String key, int min, int max, int def) {
        String s = read(key);
        if (s == null) return def;
        try {
            long v = Long.parseLong(s.trim());
            if (v < min || v > max) return def;      // out of range counts as damaged: the default, not the nearest end
            return (int) v;
        } catch (NumberFormatException e) {
            return def;
        }
    }

    /** True, false, or null when there is no usable value. */
    Boolean getBool(String key) {
        String s = read(key);
        if (s == null) return null;
        s = s.trim();
        if (s.equals("1")) return Boolean.TRUE;
        if (s.equals("0")) return Boolean.FALSE;
        return null;
    }

    private String read(String key) {
        try { return store.get(key); } catch (RuntimeException e) { return null; }
    }

    // ---- writing (a failing store never throws into the app)

    void putString(String key, String value) {
        try { store.put(key, value == null ? "" : value); } catch (RuntimeException ignored) { }
    }

    void putInt(String key, int value) { putString(key, Integer.toString(value)); }

    void putBool(String key, boolean value) { putString(key, value ? "1" : "0"); }

    void remove(String key) {
        try { store.remove(key); } catch (RuntimeException ignored) { }
    }

    // ---- recent DLS sound banks

    List<Recent> recent() { return decode(read(DLS_RECENT)); }

    /** Puts a bank first (moving it up when it is already listed) and cuts the list at {@link #RECENT_MAX}. Returns the dropped entries. */
    List<Recent> addRecent(String uri, String name) {
        List<Recent> list = recent();
        Recent r = new Recent(uri, name == null || name.isEmpty() ? "sound bank" : name);
        list.remove(r);
        list.add(0, r);
        List<Recent> dropped = new ArrayList<Recent>();
        while (list.size() > RECENT_MAX) dropped.add(list.remove(list.size() - 1));
        putString(DLS_RECENT, encode(list));
        return dropped;
    }

    void removeRecent(String uri) {
        List<Recent> list = recent();
        list.remove(new Recent(uri, ""));
        putString(DLS_RECENT, encode(list));
    }

    // Entry format: one line per entry, "uri TAB name"; backslash, tab, newline and CR in the parts are escaped.
    static String encode(List<Recent> list) {
        StringBuilder sb = new StringBuilder();
        for (Recent r : list) {
            if (sb.length() > 0) sb.append('\n');
            sb.append(esc(r.uri)).append('\t').append(esc(r.name));
        }
        return sb.toString();
    }

    static List<Recent> decode(String text) {
        List<Recent> out = new ArrayList<Recent>();
        if (text == null || text.isEmpty()) return out;
        for (String line : text.split("\n", -1)) {
            int tab = line.indexOf('\t');
            if (tab <= 0) continue;                         // damaged line: skipped, the others stay
            String uri = unesc(line.substring(0, tab)), name = unesc(line.substring(tab + 1));
            if (uri.isEmpty()) continue;
            Recent r = new Recent(uri, name.isEmpty() ? "sound bank" : name);
            if (!out.contains(r) && out.size() < RECENT_MAX) out.add(r);
        }
        return out;
    }

    private static String esc(String s) {
        return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r");
    }

    private static String unesc(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char n = s.charAt(++i);
                sb.append(n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n);
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }
}
