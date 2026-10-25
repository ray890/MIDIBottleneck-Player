package org.midibottleneck.selftest;

import android.content.Context;
import android.os.Build;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The automatic part of the self-test: native test programs, capability reports, device list, load check, timing. */
final class Suite {
    private static final String H = "v2/harness/";

    /** One native test program. */
    private static final class Program {
        final String id, library, title;
        final String[] args;
        final int timeoutSeconds;
        final boolean longRun;      // skipped by the quick run
        Program(String id, String library, String title, int timeoutSeconds, boolean longRun, String... args) {
            this.id = id; this.library = library; this.title = title; this.timeoutSeconds = timeoutSeconds;
            this.longRun = longRun; this.args = args;
        }
    }

    private static List<Program> programs(boolean quick) {
        List<Program> list = new ArrayList<>();
        list.add(new Program("unit", "libst_unit.so", "Core unit tests", 600, false));
        list.add(new Program("ui", "libst_ui.so", "Main window model tests", 900, false, H + "expected/ui"));
        list.add(new Program("channels", "libst_channels.so", "Channels window model tests", 900, false, H + "expected/channels-ui"));
        list.add(new Program("app", "libst_app.so", "Application tests", 900, false, H + "fixtures"));
        list.add(new Program("analysis", "libst_analysis.so", "Analysis window tests", 900, false, H + "fixtures", H + "expected/analysis-ui"));
        list.add(new Program("routing", "libst_routing.so", "Routing window tests", 900, false, H + "fixtures", H + "expected/routing-ui",
                H + "expected-v2-ui/routing-ui/explained-states.txt"));
        list.add(new Program("engine", "libst_engine.so", "Engine tests (virtual clock)", 900, false));
        list.add(new Program("platform", "libst_platform.so", "Platform layer tests", 300, false));
        if (quick) {
            list.add(new Program("parity", "libst_parity.so", "v2 baseline (parity tier, without the engine runs)", 900, false,
                    "--fixtures", H + "fixtures", "--expected", H + "expected", "--baseline", H + "expected-v2",
                    "--scenarios", H + "engine-scenarios.txt", "--no-engine"));
        } else {
            list.add(new Program("parity", "libst_parity.so", "v2 baseline (parity tier, with the engine runs)", 3000, true,
                    "--fixtures", H + "fixtures", "--expected", H + "expected", "--baseline", H + "expected-v2",
                    "--scenarios", H + "engine-scenarios.txt"));
            list.add(new Program("harness", "libst_harness.so", "Engine harness (3,956 engine runs against expected results)", 3000, true,
                    "--fixtures", H + "fixtures", "--scenarios", H + "engine-scenarios.txt", "--expected", H + "expected/engine",
                    "--rules", H + "expected/differences-from-v1.txt", "--baseline", H + "expected-v2/engine"));
        }
        return list;
    }

    private static final Pattern SUMMARY_LINE = Pattern.compile("^(RESULT|PASS:|FAIL:|UI TESTS|CHANNELS APP TESTS|midibn-analysis-app-tests:|PARITY|OVERALL|[A-Za-z -]*TESTS?\\b).*");

    private final Context context;
    private final Results results;
    private final boolean quick;
    private final DriftCheck.BackgroundCounter background;
    private File dataDir;

    Suite(Context context, Results results, boolean quick, DriftCheck.BackgroundCounter background) {
        this.context = context.getApplicationContext();
        this.results = results;
        this.quick = quick;
        this.background = background;
    }

    File dataDir() { return dataDir; }

    void run() {
        long started = System.nanoTime();
        results.log("Self-test starts (" + (quick ? "quick" : "full") + " run). Keep the screen on and this app in front.");
        results.setNotRun("send check to a USB MIDI device (button on the main screen)", true);
        results.setNotRun("file-picker check (button on the main screen)", true);

        String nativeError = Native.load();
        if (nativeError != null) results.add("Native self-test library", Results.Status.FAIL, nativeError);
        else results.add("Native self-test library", Results.Status.PASS, "libselftest_jni.so loaded");

        boolean dataOk = nativeError == null && unpackData();
        if (nativeError == null) {
            programsAndCapability(dataOk);
        }
        JavaCapability.run(context, results, quick);
        MidiReport.run(context, results);
        loadCheck(dataOk);
        DriftCheck.run(results, quick ? 4 : 15, background);

        results.log(String.format(Locale.US, "Automatic part finished in %.0f s.", (System.nanoTime() - started) / 1e9));
    }

    // ---- data -------------------------------------------------------------------------------------------------------

    private boolean unpackData() {
        results.log("Preparing the test data (fixtures and expected results)...");
        dataDir = new File(context.getFilesDir(), "data");
        File tmp = new File(context.getFilesDir(), "tmp");
        tmp.mkdirs();
        try {
            long assetSize;
            try (android.content.res.AssetFileDescriptor fd = context.getAssets().openFd("selftest-data.zip")) { assetSize = fd.getLength(); }
            String stamp = assetSize + ":" + context.getPackageManager().getPackageInfo(context.getPackageName(), 0).lastUpdateTime;
            File stampFile = new File(dataDir, ".stamp");
            if (stampFile.isFile() && stamp.equals(readAll(stampFile).trim())) {
                results.add("Test data", Results.Status.PASS, "already unpacked (" + countFiles(dataDir) + " files)");
                return true;
            }
            deleteTree(dataDir);
            dataDir.mkdirs();
            int files = 0;
            long bytes = 0;
            byte[] buffer = new byte[64 * 1024];
            String root = dataDir.getCanonicalPath() + File.separator;
            try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("selftest-data.zip"))) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    File target = new File(dataDir, entry.getName());
                    if (!target.getCanonicalPath().startsWith(root)) throw new IOException("bad entry " + entry.getName());
                    if (entry.isDirectory()) { target.mkdirs(); continue; }
                    target.getParentFile().mkdirs();
                    try (FileOutputStream out = new FileOutputStream(target)) {
                        int n;
                        while ((n = zip.read(buffer)) > 0) { out.write(buffer, 0, n); bytes += n; }
                    }
                    files++;
                }
            }
            try (FileOutputStream out = new FileOutputStream(stampFile)) { out.write(stamp.getBytes(StandardCharsets.UTF_8)); }
            results.add("Test data", Results.Status.PASS, files + " files, " + bytes / 1048576 + " MB unpacked");
            return true;
        } catch (Throwable t) {
            results.add("Test data", Results.Status.FAIL, "could not unpack: " + t);
            return false;
        }
    }

    // ---- native programs --------------------------------------------------------------------------------------------

    private void programsAndCapability(boolean dataOk) {
        File tmp = new File(context.getFilesDir(), "tmp");
        File testDir = results.file("tests");

        // Capability report (native part).
        File capOut = results.file("capability-native.txt");
        results.log("Native capability report...");
        int rc = Native.runProgram("libst_capability.so", quick ? new String[] { "--quick", "--machine" } : new String[] { "--machine" },
                context.getFilesDir().getPath(), capOut.getPath(), tmp.getPath(), 300);
        results.add("Capability report (native part)", rc == 0 ? Results.Status.INFO : Results.Status.FAIL,
                rc == 0 ? "written to capability-native.txt" : describeExit(rc) + "; see capability-native.txt");

        for (Program p : programs(quick)) {
            String name = p.title;
            if (!dataOk) { results.add(name, Results.Status.SKIP, "test data not available"); continue; }
            if (p.id.equals("platform")) {
                if (Build.VERSION.SDK_INT < 26) { results.add(name, Results.Status.SKIP, "needs Android 8 (API 26) or later"); continue; }
                if (!new File("/tmp").canWrite()) {
                    results.add(name, Results.Status.SKIP, "this test writes to /tmp, which apps do not have on Android; the test source needs a TMPDIR option (reported to the developers)");
                    continue;
                }
            }
            File out = new File(testDir, p.id + ".txt");
            results.log("Running: " + name + " ...");
            long begin = System.nanoTime();
            int exit = Native.runProgram(p.library, p.args, dataDir.getPath(), out.getPath(), tmp.getPath(), p.timeoutSeconds);
            double seconds = (System.nanoTime() - begin) / 1e9;
            String last = lastSummaryLine(out);
            String detail = String.format(Locale.US, "%.0f s", seconds) + (last.isEmpty() ? "" : "; " + last) + "; see tests/" + p.id + ".txt";
            if (exit == 0) results.add(name, Results.Status.PASS, detail);
            else results.add(name, Results.Status.FAIL, describeExit(exit) + "; " + detail);
        }
    }

    private static String describeExit(int rc) {
        if (rc == -1) return "the test process could not be started";
        if (rc == -2) return "time limit exceeded (process stopped)";
        if (rc >= 128) return "crashed (signal " + (rc - 128) + ")";
        if (rc == 125 || rc == 126) return "the test library did not load";
        return "exit code " + rc;
    }

    private static String lastSummaryLine(File file) {
        String last = "", lastAny = "";
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.trim().isEmpty()) continue;
                lastAny = line;
                if (SUMMARY_LINE.matcher(line).matches()) last = line;
            }
        } catch (IOException ignored) { }
        String line = last.isEmpty() ? lastAny : last;
        return line.length() > 160 ? line.substring(0, 160) : line;
    }

    // ---- load check through the JNI library -------------------------------------------------------------------------

    private void loadCheck(boolean dataOk) {
        if (!dataOk) { results.add("Load a MIDI fixture through the app", Results.Status.SKIP, "test data not available"); return; }
        File fixtures = new File(dataDir, "v2/harness/fixtures");
        File[] list = fixtures.listFiles();
        File chosen = new File(fixtures, "f1-v1-test-midi.mid");
        if (!chosen.isFile()) {
            chosen = null;
            if (list != null) {
                java.util.Arrays.sort(list);
                for (File f : list) {
                    String n = f.getName().toLowerCase(Locale.US);
                    if (n.endsWith(".mid") && !n.startsWith("err-") && f.length() > 200) { chosen = f; break; }
                }
            }
        }
        if (chosen == null) { results.add("Load a MIDI fixture through the app", Results.Status.SKIP, "no .mid fixture found"); return; }
        String answer = Native.loadMidi(chosen.getPath());
        if (answer.startsWith("OK")) results.add("Load a MIDI fixture through the app", Results.Status.PASS, "fixture 1: " + answer.substring(3));
        else results.add("Load a MIDI fixture through the app", Results.Status.FAIL, answer);
    }

    // ---- helpers ----------------------------------------------------------------------------------------------------

    private static String readAll(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int) Math.min(file.length(), 4096)];
            int n = in.read(bytes);
            return new String(bytes, 0, Math.max(n, 0), StandardCharsets.UTF_8);
        }
    }

    private static int countFiles(File dir) {
        int count = 0;
        File[] children = dir.listFiles();
        if (children == null) return 0;
        for (File c : children) count += c.isDirectory() ? countFiles(c) : 1;
        return count;
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        file.delete();
    }
}
