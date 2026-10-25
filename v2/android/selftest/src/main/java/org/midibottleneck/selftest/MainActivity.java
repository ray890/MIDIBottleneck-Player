package org.midibottleneck.selftest;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.midibottleneck.core.route.MidiRoute;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * MIDIBottleneck Self-test: a test tool for developers, separate from the player. One screen: run the automatic suite,
 * then the two optional checks that need the user (send check to a USB MIDI device, file-picker check). Results are
 * written to a run folder and copied to Downloads/MIDIBottleneck self-test/ after every stage.
 */
public class MainActivity extends Activity {
    private static final int REQUEST_PICK_FILE = 41;
    private static final int REQUEST_STORAGE = 42;

    /** State that survives the activity being re-created while a run is in progress. */
    private static final class Session {
        static Results results;
        static Exporter exporter;
        static boolean running;
        static int pauses;
        static final StringBuilder log = new StringBuilder();
        static String exportNote = "";
    }

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView logView, overallView, folderView;
    private ScrollView logScroll;
    private Button fullButton, quickButton, sendButton, fileButton;
    private Runnable afterPermission;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // the suite measures timing: keep the phone awake
        buildScreen();
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (Session.results != null) Session.results.setListener(this::appendLine);
        refresh();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (Session.running) Session.pauses++;
    }

    // ---- screen -----------------------------------------------------------------------------------------------------

    private void buildScreen() {
        float d = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * d);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);

        TextView title = text(getString(R.string.app_name), 24, true);
        column.addView(title);
        column.addView(text("A test tool for developers. This is NOT the MIDIBottleneck Player: it plays nothing and keeps no settings.", 14, false));
        column.addView(text(Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), "
                + (Build.VERSION.SDK_INT >= 29 && getPackageName().indexOf("android6") < 0 ? "main build" : "Android 6+ build")
                + ", version " + versionName(), 13, false));

        fullButton = button("Run the full self-test (several minutes)", v -> start(false));
        quickButton = button("Run a quick self-test (shorter)", v -> start(true));
        sendButton = button("Optional: send check to a USB MIDI device...", v -> chooseDevice());
        fileButton = button("Optional: file-picker check (choose any MIDI file)...", v -> pickFile());
        column.addView(fullButton);
        column.addView(quickButton);
        column.addView(sendButton);
        column.addView(fileButton);

        overallView = text("", 22, true);
        column.addView(overallView);
        folderView = text("", 13, false);
        column.addView(folderView);

        logView = text("", 12, false);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setTextIsSelectable(true);
        column.addView(logView);

        logScroll = new ScrollView(this);
        logScroll.addView(column);
        logScroll.setPadding(pad, pad, pad, pad);
        logScroll.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            @SuppressWarnings("deprecation")
            public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                view.setPadding(pad + insets.getSystemWindowInsetLeft(), pad + insets.getSystemWindowInsetTop(),
                        pad + insets.getSystemWindowInsetRight(), pad + insets.getSystemWindowInsetBottom());
                return insets;
            }
        });
        setContentView(logScroll);
    }

    private TextView text(String s, int sp, boolean bold) {
        TextView view = new TextView(this);
        view.setText(s);
        view.setTextSize(sp);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setGravity(Gravity.START);
        int p = (int) (4 * getResources().getDisplayMetrics().density);
        view.setPadding(0, p, 0, p);
        return view;
    }

    private Button button(String label, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(listener);
        return b;
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "?"; }
    }

    private void refresh() {
        boolean busy = Session.running;
        fullButton.setEnabled(!busy);
        quickButton.setEnabled(!busy);
        sendButton.setEnabled(!busy && Session.results != null);
        fileButton.setEnabled(!busy && Session.results != null);
        logView.setText(Session.log.toString());
        if (Session.results != null) {
            overallView.setText(busy ? "Running..." : Session.results.overall().replace("OVERALL", "Result:"));
            folderView.setText(Session.exportNote.isEmpty() ? "" : Session.exportNote);
        }
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void appendLine(String line) {
        synchronized (Session.log) { Session.log.append(line).append('\n'); }
        ui.post(this::refresh);
    }

    // ---- running ----------------------------------------------------------------------------------------------------

    private boolean needStoragePermission() {
        return Build.VERSION.SDK_INT >= 23 && Build.VERSION.SDK_INT <= 28
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED;
    }

    private void withPermission(Runnable action) {
        if (needStoragePermission()) {
            afterPermission = action;
            requestPermissions(new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE }, REQUEST_STORAGE);
        } else {
            action.run();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQUEST_STORAGE && afterPermission != null) {
            Runnable action = afterPermission;
            afterPermission = null;
            action.run();   // if it was refused, the export reports it and the results stay in the app's own folder
        }
    }

    private File baseDir() {
        File external = getExternalFilesDir(null);
        File base = new File(external != null ? external : getFilesDir(), "results");
        base.mkdirs();
        return base;
    }

    private void start(boolean quick) {
        withPermission(() -> {
            if (Session.running) return;
            Session.running = true;
            Session.pauses = 0;
            synchronized (Session.log) { Session.log.setLength(0); }
            Session.exportNote = "";
            String header = "MIDIBottleneck Self-test " + versionName() + " (" + getPackageName() + ")\n"
                    + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                    + "ABIs: " + java.util.Arrays.toString(Build.SUPPORTED_ABIS) + "\nRun: " + (quick ? "quick" : "full");
            Session.results = new Results(baseDir(), header);
            Session.results.setListener(this::appendLine);
            Session.exporter = new Exporter(this);
            refresh();
            final Results results = Session.results;
            final boolean q = quick;
            new Thread(() -> {
                try {
                    new Suite(getApplicationContext(), results, q, () -> Session.pauses).run();
                } catch (Throwable t) {
                    results.add("Self-test run", Results.Status.FAIL, "unexpected error: " + t);
                }
                finish(results);
            }, "selftest-suite").start();
        });
    }

    /** Writes the summary, copies the run folder to Downloads and re-enables the buttons. */
    private void finish(Results results) {
        results.writeSummary();
        String error = Session.exporter.publish(results.dir, results.runName);
        if (error == null) {
            Session.exportNote = "Results copied to " + Session.exporter.describeTarget(results.runName)
                    + "\n(copy that folder to the PC; the file summary.txt ends with " + results.overall() + ")";
        } else {
            Session.exportNote = "Could not copy to Downloads:\n" + error + "\nThe results are in the app's folder: " + results.dir.getPath();
            results.log("EXPORT PROBLEM: " + error);
        }
        results.log(results.overall());
        results.log(Session.exportNote);
        Session.running = false;
        ui.post(this::refresh);
    }

    // ---- send check -------------------------------------------------------------------------------------------------

    private void chooseDevice() {
        final RouteAccess.Found found = RouteAccess.find(this);
        Set<String> names = new LinkedHashSet<>();
        for (MidiRoute route : found.routes) {
            if (!route.isAvailable()) continue;
            try {
                String[] list = route.listDevices();
                if (list != null) for (String n : list) names.add(n);
            } catch (Throwable ignored) { }
        }
        if (names.isEmpty()) {
            new AlertDialog.Builder(this).setTitle("No MIDI output device")
                    .setMessage("No MIDI output device was found. Plug in the USB MIDI device (with its adapter or cable), then try again."
                            + (found.note.isEmpty() ? "" : "\n\n" + found.note))
                    .setPositiveButton("OK", null).show();
            Session.results.setNotRun("send check to a USB MIDI device (button on the main screen)", true);
            return;
        }
        final List<String> list = new ArrayList<>(names);
        new AlertDialog.Builder(this).setTitle("Send check to which device?")
                .setItems(list.toArray(new String[0]), (dialog, which) -> confirmSend(found.routes, list.get(which)))
                .setNegativeButton("Skip", null).show();
    }

    private void confirmSend(MidiRoute[] routes, String device) {
        new AlertDialog.Builder(this).setTitle("Send check")
                .setMessage("Sends about 20 seconds of harmless messages (silent controller messages, a device inquiry, then reset controllers) to \"" + device
                        + "\" through each route this build has. Make sure the device is not connected to anything you do not want to touch.")
                .setPositiveButton("Send", (dialog, which) -> runSend(routes, device))
                .setNegativeButton("Cancel", null).show();
    }

    private void runSend(MidiRoute[] routes, String device) {
        if (Session.running) return;
        Session.running = true;
        final Results results = Session.results;
        refresh();
        new Thread(() -> {
            try {
                JavaCapability.raiseCurrentThread();
                results.log("Send check to the chosen device...");
                SendCheck.run(results, routes, device, 3);
                results.setNotRun("send check to a USB MIDI device (button on the main screen)", false);
            } catch (Throwable t) {
                results.add("Send check", Results.Status.FAIL, "unexpected error: " + t);
            }
            finish(results);
        }, "selftest-send").start();
    }

    // ---- file picker ------------------------------------------------------------------------------------------------

    private void pickFile() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQUEST_PICK_FILE);
        } catch (Throwable t) {
            Session.results.add("File-picker check", Results.Status.FAIL, "the system file picker could not be opened: " + t);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_FILE) return;
        final Results results = Session.results;
        if (results == null) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            results.log("File-picker check: no file chosen (skipped).");
            return;
        }
        final Uri uri = data.getData();
        if (Session.running) return;
        Session.running = true;
        refresh();
        new Thread(() -> {
            try {
                fileCheck(results, uri);
                results.setNotRun("file-picker check (button on the main screen)", false);
            } catch (Throwable t) {
                results.add("File-picker check", Results.Status.FAIL, "unexpected error: " + t);
            }
            finish(results);
        }, "selftest-file").start();
    }

    private void fileCheck(Results results, Uri uri) {
        File copy = new File(getFilesDir(), "picked-file.bin");
        long start = System.nanoTime();
        long bytes = 0;
        try (InputStream in = getContentResolver().openInputStream(uri); FileOutputStream out = new FileOutputStream(copy)) {
            if (in == null) { results.add("File-picker check", Results.Status.FAIL, "the content resolver gave no stream"); return; }
            byte[] buffer = new byte[64 * 1024];
            int n;
            while ((n = in.read(buffer)) > 0) { out.write(buffer, 0, n); bytes += n; }
        } catch (Throwable t) {
            results.add("File-picker check", Results.Status.FAIL, "reading through the content resolver failed: " + t);
            return;
        }
        double readMs = (System.nanoTime() - start) / 1e6;
        if (Native.load() != null) { results.add("File-picker check", Results.Status.FAIL, Native.load()); return; }
        String answer = Native.loadMidi(copy.getPath());
        String read = String.format(Locale.US, "read %d bytes in %.0f ms", bytes, readMs);
        StringBuilder t = new StringBuilder("FILE-PICKER CHECK (user file 1)\n" + read + "\nCore loader answer: " + answer + "\n");
        results.write("file-check.txt", t.toString());
        if (answer.startsWith("OK")) {
            results.add("File-picker check", Results.Status.PASS, read + "; loaded: " + answer.substring(3));
        } else {
            results.add("File-picker check", Results.Status.INFO,
                    read + "; the core loader refused the file (" + answer + "); expected if it is not a MIDI file, otherwise report it");
        }
        copy.delete();
    }
}
