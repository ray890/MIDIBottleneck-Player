package org.midibottleneck.player;

import android.content.Context;
import android.content.SharedPreferences;

/** The {@link Prefs} of the app, stored in the SharedPreferences file "midibn_player". */
final class AppPrefs {
    private AppPrefs() { }

    private static Prefs instance;

    static synchronized Prefs get(Context context) {
        if (instance == null) {
            final SharedPreferences sp = context.getApplicationContext().getSharedPreferences("midibn_player", Context.MODE_PRIVATE);
            instance = new Prefs(new Prefs.Store() {
                @Override public String get(String key) {
                    try { return sp.getString(key, null); } catch (RuntimeException e) { return null; }   // another type stored: ClassCastException
                }
                @Override public void put(String key, String value) { sp.edit().putString(key, value).apply(); }
                @Override public void remove(String key) { sp.edit().remove(key).apply(); }
            });
        }
        return instance;
    }
}
