package org.midibottleneck.selftest;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * Copies a run folder to Downloads/MIDIBottleneck self-test/&lt;run&gt;/ so the user can copy it to the PC.
 * Android 10 and later: MediaStore Downloads (no permission). Android 6 to 9: the public Downloads folder
 * (WRITE_EXTERNAL_STORAGE, asked at run time by the activity). Safe to call again: only changed files are written.
 */
final class Exporter {
    static final String FOLDER = "MIDIBottleneck self-test";

    private final Context context;
    private final Map<String, String> done = new HashMap<>();   // relative path -> "length:modified" already exported

    Exporter(Context context) { this.context = context.getApplicationContext(); }

    /** Where the files go, for messages. */
    String describeTarget(String runName) {
        return "Downloads/" + FOLDER + "/" + runName;
    }

    /** Returns null on success, else a message saying what failed. */
    String publish(File runDir, String runName) {
        StringBuilder problems = new StringBuilder();
        try {
            copyTree(runDir, "", runName, problems);
        } catch (Throwable t) {
            problems.append(t).append('\n');
        }
        return problems.length() == 0 ? null : problems.toString().trim();
    }

    private void copyTree(File dir, String relative, String runName, StringBuilder problems) {
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File child : children) {
            String childRelative = relative.isEmpty() ? child.getName() : relative + "/" + child.getName();
            if (child.isDirectory()) {
                copyTree(child, childRelative, runName, problems);
                continue;
            }
            String stamp = child.length() + ":" + child.lastModified();
            if (stamp.equals(done.get(childRelative))) continue;
            try {
                copyFile(child, relative, runName);
                done.put(childRelative, stamp);
            } catch (Throwable t) {
                problems.append(childRelative).append(": ").append(t).append('\n');
            }
        }
    }

    private void copyFile(File file, String subFolder, String runName) throws Exception {
        if (Build.VERSION.SDK_INT >= 29) {
            copyWithMediaStore(file, subFolder, runName);
        } else {
            File target = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                    FOLDER + "/" + runName + (subFolder.isEmpty() ? "" : "/" + subFolder));
            if (!target.isDirectory() && !target.mkdirs()) throw new java.io.IOException("cannot create " + target);
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = new java.io.FileOutputStream(new File(target, file.getName()), false)) {
                pump(in, out);
            }
        }
    }

    private void copyWithMediaStore(File file, String subFolder, String runName) throws Exception {
        ContentResolver resolver = context.getContentResolver();
        Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        String relativePath = "Download/" + FOLDER + "/" + runName + (subFolder.isEmpty() ? "" : "/" + subFolder) + "/";
        Uri existing = null;
        try (Cursor cursor = resolver.query(collection, new String[] { MediaStore.MediaColumns._ID },
                MediaStore.MediaColumns.DISPLAY_NAME + "=? AND " + MediaStore.MediaColumns.RELATIVE_PATH + "=?",
                new String[] { file.getName(), relativePath }, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                existing = android.content.ContentUris.withAppendedId(collection, cursor.getLong(0));
            }
        }
        Uri target = existing;
        if (target == null) {
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DISPLAY_NAME, file.getName());
            values.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
            values.put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath);
            target = resolver.insert(collection, values);
        }
        if (target == null) throw new java.io.IOException("MediaStore refused " + file.getName());
        try (InputStream in = new FileInputStream(file); OutputStream out = resolver.openOutputStream(target, "wt")) {
            if (out == null) throw new java.io.IOException("cannot open " + target);
            pump(in, out);
        }
    }

    private static void pump(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buffer = new byte[64 * 1024];
        int n;
        while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
    }
}
