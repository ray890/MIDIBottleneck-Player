package org.midibottleneck.player;

import android.content.Intent;

/**
 * The three ways to choose a MIDI file:
 * (1) {@link #midi}: the system picker with the MIDI types, so only folders and MIDI files are offered (Android filters by
 *     type, not by file extension, so a file whose provider labels it with an odd type is not offered here);
 * (2) {@link #any}: the system picker with every file selectable (the fallback for odd types; the loader rejects files that
 *     are not MIDI with a plain message);
 * (3) {@link #otherApp}: a chooser of every installed app that can hand over a file (file managers such as File Manager+).
 * The extensions the player reads are .mid, .midi, .rmi (RIFF MIDI) and .kar / .smf (standard MIDI files under other names);
 * they are listed in the manifest's intent filters for files opened from other apps.
 */
final class FilePicker {
    private FilePicker() { }

    /** MIME types that providers use for MIDI files. */
    static final String[] MIDI_TYPES = {
        "audio/midi", "audio/x-midi", "audio/mid", "audio/x-mid", "audio/sp-midi", "audio/rmf", "audio/x-rmf",
        "application/x-midi", "application/midi", "audio/x-karaoke", "audio/kar", "audio/rmi", "audio/x-rmi"
    };

    /** File name extensions the player reads (lower case, with the dot). */
    static final String[] EXTENSIONS = { ".mid", ".midi", ".rmi", ".kar", ".smf" };

    static boolean hasMidiExtension(String name) {
        if (name == null) return false;
        String n = name.toLowerCase(java.util.Locale.ROOT);
        for (String e : EXTENSIONS) if (n.endsWith(e)) return true;
        return false;
    }

    /** System picker, MIDI types only. */
    static Intent midi() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, MIDI_TYPES);
        return i;
    }

    /** System picker, every file selectable. */
    static Intent any() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        return i;
    }

    /** Chooser of the apps that can provide a file (the system's own file picker is among them). */
    static Intent otherApp() {
        Intent i = new Intent(Intent.ACTION_GET_CONTENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        return Intent.createChooser(i, "Open a MIDI file with");
    }
}
