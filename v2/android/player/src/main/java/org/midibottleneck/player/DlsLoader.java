package org.midibottleneck.player;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import org.midibottleneck.core.route.SynthMidiRoute;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

/**
 * Loading a DLS sound bank for the built-in synth and remembering it: shared by the Synth settings page (the user's
 * choice, a tap in the recent list) and the start of the app (the bank of the last run). Reading happens on a background
 * thread; the result comes back on the main thread. A bank that came from a content URI is kept readable with a persisted
 * URI permission, and every bank that loaded is put first in the recent list.
 */
final class DlsLoader {
    private DlsLoader() { }

    static final String CACHE_PREFIX = "dls-load-";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile boolean loading;
    private static volatile String note = "";       // one line for the Synth settings page (e.g. the remembered bank could not be opened)

    interface Done { void done(String problem); }   // main thread; problem null on success

    static boolean isLoading() { return loading; }

    /** The pending one-line note, kept until {@link #clearNote}. */
    static String note() { return note; }

    static void clearNote() { note = ""; }

    static void setNote(String text) { note = text == null ? "" : text; }

    /** Keeps a content URI readable after a restart (works for ACTION_OPEN_DOCUMENT results). Never throws. */
    static void persist(Context context, Uri uri) {
        try {
            context.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (RuntimeException ignored) {
            // not a persistable grant (another kind of provider): the bank still works now and may fail to reopen later
        }
    }

    private static void release(Context context, Uri uri) {
        try {
            context.getContentResolver().releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (RuntimeException ignored) { }
    }

    /** True when the document can be opened for reading right now (background thread). */
    static boolean canOpen(Context context, Uri uri) {
        try {
            ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) return false;
            pfd.close();
            return true;
        } catch (IOException e) {
            return false;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /**
     * Loads the bank on a background thread and remembers it when it loads. {@code done} is called on the main thread.
     * Only one load runs at a time.
     */
    static void load(final Context context, final Uri uri, final Done done) {
        final Context app = context.getApplicationContext();
        synchronized (DlsLoader.class) {
            if (loading) {
                if (done != null) done.done("A sound bank is already loading.");
                return;
            }
            loading = true;
        }
        new Thread(new Runnable() {
            @Override public void run() {
                String problem;
                String name = displayName(app, uri);
                try {
                    problem = loadBank(app, uri, name);
                } catch (RuntimeException e) {
                    problem = "The file could not be read.";
                }
                if (problem == null) remember(app, uri, name);
                final String result = problem;
                MAIN.post(new Runnable() {
                    @Override public void run() {
                        loading = false;
                        if (done != null) done.done(result);
                    }
                });
            }
        }, "dls-load").start();
    }

    private static void remember(Context app, Uri uri, String name) {
        Prefs prefs = AppPrefs.get(app);
        prefs.putString(Prefs.SYNTH_DLS_URI, uri.toString());
        prefs.putString(Prefs.SYNTH_DLS_NAME, name);
        List<Prefs.Recent> dropped = prefs.addRecent(uri.toString(), name);
        for (Prefs.Recent r : dropped) {
            if (!r.uri.equals(uri.toString())) release(app, Uri.parse(r.uri));
        }
    }

    /** The user went back to the built-in sounds. */
    static void forgetCurrent(Context app) {
        Prefs prefs = AppPrefs.get(app);
        prefs.remove(Prefs.SYNTH_DLS_URI);
        prefs.remove(Prefs.SYNTH_DLS_NAME);
    }

    /** Takes an entry out of the recent list (its permission is released unless it is the current bank). */
    static void forgetRecent(Context app, String uri) {
        Prefs prefs = AppPrefs.get(app);
        prefs.removeRecent(uri);
        if (!uri.equals(prefs.getString(Prefs.SYNTH_DLS_URI, ""))) release(app, Uri.parse(uri));
    }

    /** Background thread. Returns null on success or one plain sentence. */
    private static String loadBank(Context context, Uri uri, String name) {
        File copy = null;
        try {
            ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) return "The file could not be opened.";
            String result;
            try {
                result = SynthMidiRoute.loadDls(pfd.getFd(), name);
            } finally {
                try { pfd.close(); } catch (IOException ignored) { }
            }
            if (!SynthMidiRoute.DLS_NOT_SEEKABLE.equals(result)) return result;

            // The provider gave a stream that cannot be read at offsets: copy it to the cache, load the copy, delete it.
            copy = File.createTempFile(CACHE_PREFIX, ".tmp", context.getCacheDir());
            InputStream in = context.getContentResolver().openInputStream(uri);
            if (in == null) return "The file could not be opened.";
            long total = 0;
            try {
                OutputStream out = new FileOutputStream(copy);
                try {
                    byte[] buffer = new byte[64 * 1024];
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        total += n;
                        if (total > SynthMidiRoute.DLS_MAX_BYTES) return "This sound bank is too large to load on this phone.";
                        out.write(buffer, 0, n);
                    }
                } finally {
                    out.close();
                }
            } finally {
                in.close();
            }
            ParcelFileDescriptor local = ParcelFileDescriptor.open(copy, ParcelFileDescriptor.MODE_READ_ONLY);
            try {
                result = SynthMidiRoute.loadDls(local.getFd(), name);
            } finally {
                try { local.close(); } catch (IOException ignored) { }
            }
            return SynthMidiRoute.DLS_NOT_SEEKABLE.equals(result) ? "The file could not be read." : result;
        } catch (FileNotFoundException e) {
            return "The file could not be opened.";
        } catch (SecurityException e) {
            return "This app is not allowed to read that file.";
        } catch (IOException e) {
            return "The file could not be read.";
        } catch (OutOfMemoryError e) {
            return "This sound bank is too large to load on this phone.";
        } finally {
            if (copy != null) //noinspection ResultOfMethodCallIgnored
                copy.delete();
        }
    }

    static String displayName(Context context, Uri uri) {
        Cursor c = null;
        try {
            c = context.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && !n.isEmpty()) return n;
            }
        } catch (RuntimeException ignored) {
        } finally {
            if (c != null) c.close();
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : "sound bank";
    }

    /** Copies left in the cache by a load that was cut short by the process ending. */
    static void deleteStaleCopies(Context context) {
        File[] files = context.getCacheDir().listFiles();
        if (files == null) return;
        for (File f : files) //noinspection ResultOfMethodCallIgnored
            if (f.getName().startsWith(CACHE_PREFIX)) f.delete();
    }
}
