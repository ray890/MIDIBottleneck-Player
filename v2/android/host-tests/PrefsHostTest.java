package org.midibottleneck.player;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Plain-Java test of the remembered settings (Prefs): tolerant reads, per-key fallback, the recent DLS list. No Android
 * classes. In the player's package because Prefs is package-private. Run: java -cp OUT org.midibottleneck.player.PrefsHostTest
 */
public final class PrefsHostTest {
    private static int failures;

    private static void check(String what, boolean ok) {
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    private static Prefs over(final Map<String, String> map) {
        return new Prefs(new Prefs.Store() {
            @Override public String get(String key) { return map.get(key); }
            @Override public void put(String key, String value) { map.put(key, value); }
            @Override public void remove(String key) { map.remove(key); }
        });
    }

    public static void main(String[] args) {
        final Map<String, String> map = new HashMap<String, String>();
        Prefs f = over(map);

        check("absent int gives default", f.getInt(Prefs.SYNTH_VOLUME, 0, 200, -1) == -1);
        check("absent bool gives null", f.getBool(Prefs.SYNTH_REVERB_ON) == null);
        check("absent string gives default", f.getString(Prefs.OUTPUT_ITEM, "x").equals("x"));

        f.putInt(Prefs.SYNTH_VOLUME, 150);
        check("volume 150", f.getInt(Prefs.SYNTH_VOLUME, 0, 200, -1) == 150);
        f.putBool(Prefs.SYNTH_REVERB_ON, true);
        check("bool 1", Boolean.TRUE.equals(f.getBool(Prefs.SYNTH_REVERB_ON)));
        f.putBool(Prefs.SYNTH_REVERB_ON, false);
        check("bool 0", Boolean.FALSE.equals(f.getBool(Prefs.SYNTH_REVERB_ON)));

        // damaged values fall back per key, the good ones stay
        map.put(Prefs.SYNTH_VOLUME, "loud");
        map.put(Prefs.SYNTH_VOICES, "99999999999999999999");
        map.put(Prefs.SYNTH_REVERB_AMOUNT, "-5");
        map.put(Prefs.SYNTH_REVERB_PRESET, "2");
        map.put(Prefs.SYNTH_REVERB_ON, "maybe");
        check("text in an int key", f.getInt(Prefs.SYNTH_VOLUME, 0, 200, -1) == -1);
        check("overflowing number", f.getInt(Prefs.SYNTH_VOICES, 1, 1024, -1) == -1);
        check("out of range", f.getInt(Prefs.SYNTH_REVERB_AMOUNT, 0, 100, -1) == -1);
        check("the good key next to damaged ones", f.getInt(Prefs.SYNTH_REVERB_PRESET, 0, 3, -1) == 2);
        check("damaged bool", f.getBool(Prefs.SYNTH_REVERB_ON) == null);

        // a store that throws never stops the app
        Prefs bad = new Prefs(new Prefs.Store() {
            @Override public String get(String key) { throw new ClassCastException("wrong type"); }
            @Override public void put(String key, String value) { throw new IllegalStateException("disk full"); }
            @Override public void remove(String key) { throw new IllegalStateException("disk full"); }
        });
        check("throwing store read", bad.getInt("a", 0, 5, 3) == 3);
        check("throwing store list", bad.recent().isEmpty());
        bad.putInt("a", 1);
        bad.remove("a");
        bad.addRecent("content://x", "n");

        // recent list: newest first, no duplicates, at most 8, damaged lines skipped
        for (int i = 0; i < 10; i++) f.addRecent("content://x/" + i, "bank " + i + "\twith\ttabs\nand newline \\ slash");
        List<Prefs.Recent> r = f.recent();
        check("recent capped at 8", r.size() == 8);
        check("recent newest first", r.get(0).uri.equals("content://x/9") && r.get(7).uri.equals("content://x/2"));
        check("name round trip", r.get(0).name.equals("bank 9\twith\ttabs\nand newline \\ slash"));
        List<Prefs.Recent> dropped = f.addRecent("content://x/5", "again");
        r = f.recent();
        check("moved to front, still 8, nothing dropped", r.size() == 8 && r.get(0).uri.equals("content://x/5")
                && r.get(0).name.equals("again") && dropped.isEmpty());
        int n5 = 0;
        for (Prefs.Recent e : r) if (e.uri.equals("content://x/5")) n5++;
        check("no duplicate", n5 == 1);
        dropped = f.addRecent("content://x/new", "new");
        check("the oldest is dropped and reported", dropped.size() == 1 && dropped.get(0).uri.equals("content://x/2"));
        f.removeRecent("content://x/9");
        check("removed", f.recent().size() == 7);
        map.put(Prefs.DLS_RECENT, "garbage without tab\ncontent://ok\tgood name\n\n\tno uri");
        r = f.recent();
        check("damaged lines skipped", r.size() == 1 && r.get(0).uri.equals("content://ok") && r.get(0).name.equals("good name"));
        map.remove(Prefs.DLS_RECENT);
        check("no list", f.recent().isEmpty());

        if (failures == 0) System.out.println("PrefsHostTest: PASS");
        else { System.out.println("PrefsHostTest: " + failures + " FAILED"); System.exit(1); }
    }
}
