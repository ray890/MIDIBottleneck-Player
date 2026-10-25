package org.midibottleneck.selftest;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/** The results of one self-test run: the checks, the progress log and the text files in the run folder. */
final class Results {
    enum Status { PASS, FAIL, INFO, SKIP }

    static final class Check {
        final String name;
        final Status status;
        final String detail;
        Check(String name, Status status, String detail) { this.name = name; this.status = status; this.detail = detail; }
    }

    interface Listener { void onLine(String line); }

    final File dir;
    final String runName;
    private final List<Check> checks = new ArrayList<>();
    private final List<String> notRun = new ArrayList<>();
    private final String header;
    private Listener listener;

    Results(File baseDir, String header) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
        String model = android.os.Build.MODEL == null ? "phone" : android.os.Build.MODEL.replaceAll("[^A-Za-z0-9]+", "-");
        this.runName = stamp + "_" + model;
        this.dir = new File(baseDir, runName);
        this.dir.mkdirs();
        new File(dir, "tests").mkdirs();
        this.header = header;
    }

    synchronized void setListener(Listener listener) { this.listener = listener; }

    /** A line of progress: shown on the screen and appended to progress.txt. */
    synchronized void log(String line) {
        append("progress.txt", line + "\n");
        if (listener != null) listener.onLine(line);
    }

    synchronized void add(String name, Status status, String detail) {
        checks.add(new Check(name, status, detail));
        log(status + "  " + name + (detail == null || detail.isEmpty() ? "" : "  -  " + detail));
        writeSummary();
    }

    synchronized void setNotRun(String item, boolean notRunNow) {
        notRun.remove(item);
        if (notRunNow) notRun.add(item);
        writeSummary();
    }

    synchronized boolean hasFailure() {
        for (Check c : checks) if (c.status == Status.FAIL) return true;
        return false;
    }

    synchronized String overall() {
        boolean pass = false;
        for (Check c : checks) {
            if (c.status == Status.FAIL) return "OVERALL FAIL";
            if (c.status == Status.PASS) pass = true;
        }
        return pass ? "OVERALL PASS" : "OVERALL FAIL";
    }

    synchronized String summaryText() {
        StringBuilder text = new StringBuilder();
        text.append(header).append("\n\n");
        int pass = 0, fail = 0, info = 0, skip = 0;
        for (Check c : checks) {
            switch (c.status) {
                case PASS: pass++; break;
                case FAIL: fail++; break;
                case INFO: info++; break;
                default: skip++; break;
            }
        }
        for (Check c : checks) {
            text.append(String.format(Locale.US, "%-5s %s", c.status, c.name));
            if (c.detail != null && !c.detail.isEmpty()) text.append("  -  ").append(c.detail);
            text.append('\n');
        }
        text.append('\n').append(pass).append(" passed, ").append(fail).append(" failed, ").append(info)
                .append(" information, ").append(skip).append(" skipped.\n");
        for (String item : notRun) text.append("Not run yet (optional, needs you): ").append(item).append('\n');
        text.append(overall()).append('\n');   // the last line of the file
        return text.toString();
    }

    synchronized void writeSummary() { write("summary.txt", summaryText()); }

    File file(String relativeName) { return new File(dir, relativeName); }

    synchronized void write(String relativeName, String text) {
        try (FileOutputStream out = new FileOutputStream(file(relativeName), false)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // reported when the export finds the file missing
        }
    }

    synchronized void append(String relativeName, String text) {
        try (FileOutputStream out = new FileOutputStream(file(relativeName), true)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // ignored
        }
    }
}
