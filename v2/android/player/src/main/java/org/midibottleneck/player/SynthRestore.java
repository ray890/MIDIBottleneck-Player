package org.midibottleneck.player;

import android.content.Context;
import android.net.Uri;

import org.midibottleneck.core.route.SynthMidiRoute;

/** Puts the remembered synth settings (and DLS sound bank) back once per run of the app, and saves them when they change. */
final class SynthRestore {
    private SynthRestore() { }

    private static boolean done;

    /** Once per process; later calls do nothing. Every key falls back to the default on its own when damaged. */
    static synchronized void applyOnce(Context context) {
        if (done) return;
        done = true;
        final Context app = context.getApplicationContext();
        Prefs p = AppPrefs.get(app);
        try {
            int volume = p.getInt(Prefs.SYNTH_VOLUME, 0, SynthMidiRoute.VOLUME_MAX, -1);
            if (volume >= 0) SynthMidiRoute.setVolume(volume);
            int voices = p.getInt(Prefs.SYNTH_VOICES, 1, Math.max(1, SynthMidiRoute.maxVoices()), -1);
            if (voices >= 1) SynthMidiRoute.setVoiceLimit(voices);
            Boolean reverbOn = p.getBool(Prefs.SYNTH_REVERB_ON);
            if (reverbOn != null) SynthMidiRoute.setReverbOn(reverbOn);
            int preset = p.getInt(Prefs.SYNTH_REVERB_PRESET, 0, SynthMidiRoute.REVERB_PRESETS.length - 1, -1);
            if (preset >= 0) SynthMidiRoute.setReverbPreset(preset);
            int amount = p.getInt(Prefs.SYNTH_REVERB_AMOUNT, 0, 100, -1);
            if (amount >= 0) SynthMidiRoute.setReverbAmount(amount);
        } catch (RuntimeException ignored) {
            // the remembered values are an extra: the app starts with the defaults for whatever could not be applied
        }
        final String uri = p.getString(Prefs.SYNTH_DLS_URI, "");
        if (uri.isEmpty()) return;
        final String name = p.getString(Prefs.SYNTH_DLS_NAME, "sound bank");
        Uri parsed;
        try {
            parsed = Uri.parse(uri);
        } catch (RuntimeException e) {
            parsed = null;
        }
        if (parsed == null || parsed.getScheme() == null) {
            DlsLoader.forgetCurrent(app);
            DlsLoader.setNote("The sound bank used last time (" + name + ") could not be opened; the built-in sounds are used.");
            return;
        }
        DlsLoader.load(app, parsed, new DlsLoader.Done() {
            @Override public void done(String problem) {
                if (problem == null) return;
                DlsLoader.forgetCurrent(app);
                DlsLoader.setNote("The sound bank used last time (" + name + ") could not be opened; the built-in sounds are used.");
            }
        });
    }

    /** Writes the current synth settings (called after every change on the Synth settings page). */
    static void save(Context context) {
        Prefs p = AppPrefs.get(context);
        int[] s = SynthMidiRoute.settings();   // volume %, voice limit, reverb on, preset, amount %, build maximum
        p.putInt(Prefs.SYNTH_VOLUME, s[0]);
        p.putInt(Prefs.SYNTH_VOICES, s[1]);
        p.putBool(Prefs.SYNTH_REVERB_ON, s[2] != 0);
        p.putInt(Prefs.SYNTH_REVERB_PRESET, s[3]);
        p.putInt(Prefs.SYNTH_REVERB_AMOUNT, s[4]);
    }
}
