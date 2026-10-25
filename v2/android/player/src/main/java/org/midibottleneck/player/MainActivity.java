package org.midibottleneck.player;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.hardware.usb.UsbManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.AdapterView;

import org.midibottleneck.core.PlayerBridge;

/**
 * The player's main screen: a thin layer over the shared PlayerController (through PlayerBridge). Every control shows a
 * value of the controller's view and sends the user's action back; nothing is decided here. Built in code from plain
 * framework views (no layout files, no fixed pixel sizes: dp/sp, weights, a ScrollView), portrait first.
 *
 * Top to bottom: top bar (name, menu button) / File and output / Processing model / Statistics, the last three in a
 * scrolling area; the transport (position bar, time, back 5 s, Play or Pause, Stop, forward 5 s) is pinned at the bottom
 * where the thumb rests. The menu button opens a small menu with the six toggles of the desktop player's system menu,
 * "Synth settings..." and Help (see {@link #showMenu}).
 */
public class MainActivity extends Activity {
    private static final int REQUEST_OPEN = 1;
    private static final int REQUEST_NOTIFICATIONS = 2;
    private static final int SLIDER_POSITIONS = 10000;   // thumb positions of the processing slider; the core's scale is 0..10000 too

    private PlayerHost host;
    private View homeView;                // the screen's root: takes the focus when typing in a number box ends
    private Remembered remembered;
    private boolean rememberedPosted;     // a Remembered.step() is waiting on the main thread
    private PlayerBridge b;
    private PlayerBridge.RefreshPacing pacer;   // the adaptive refresh rule (see PlayerBridge.RefreshPacing)
    private boolean frameArmed;          // a Choreographer frame callback is waiting
    private boolean inFrame;             // the update of a frame callback is running (do not arm again before it ends)
    private boolean pacerActive;         // live updates are running (a run starts when playing or loading starts)
    private boolean statsPhase;          // which half of the statistics cells the next half-statistics update draws
    private int statsMask = 0xFF;        // statistics cells (bit per cell) that draw() sets now
    private long statisticsNs;           // time draw() spent setting the statistics cells (for the cost split)
    private boolean applying;           // true while the screen is being set from the view (ignore control callbacks)
    private boolean visible;
    private boolean decisionOpen;
    private boolean dragging;
    private float density;
    private Palette pal;                // colours for the system's light or dark setting (Ui.palette)

    // controls
    private Button openAnyButton, openAppButton;
    private Button openButton, rescanButton, playButton, stopButton, backButton, forwardButton, dinButton, resetButton;
    private TextView fileText, fileInfo, loadStatus, timeText, pressureText, serviceLabel, serviceUnit;
    private TextView queueEvents, overflowLabel, rateLabel;
    private ProgressBar loadProgress;
    private Spinner outputSpinner, rateSpinner, overflowSpinner;
    private SeekBar timeline, rateSlider;
    private CheckBox queueCheck;
    private NumberBox queueValue, processingValue;
    private TextView statDebugNote;
    private boolean statDebug;            // the hidden layout test of the statistics is running
    private int statDebugTick;
    private LinearLayout serviceRow, queueRow, loadBox;
    private final TextView[] statValue = new TextView[PlayerBridge.STAT_CELLS];
    private View menuButton;
    private PopupWindow menu;
    private String outputItems = null, overflowItems = null, rateItems = null;
    private int lastOutput = -1, lastOverflow = -1, lastRate = -1;

    private static final String[] STAT_CAPTIONS = { "Playback", "Timeline / output", "Queue now / maximum", "Events sent / dropped",
            "Output rate", "Effective speed", "Lag current / maximum", "Maximum rate" };
    private static final int[] STAT_TEXT = { PlayerBridge.T_STAT_PLAYBACK, PlayerBridge.T_STAT_TIMELINE, PlayerBridge.T_STAT_QUEUE,
            PlayerBridge.T_STAT_EVENTS, PlayerBridge.T_STAT_OUTPUT_RATE, PlayerBridge.T_STAT_SPEED, PlayerBridge.T_STAT_LAG,
            PlayerBridge.T_STAT_MAX_RATE };
    private static final String[] TOGGLE_TEXT = { "Apply queue limit without slowdown", "Queue age limit",
            "Chase MIDI state on Play and seek", "Preserve playback queue on Pause and seek", "Output-paced timeline",
            "Scale note interval by traffic" };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        density = getResources().getDisplayMetrics().density;
        pal = Ui.palette(this);
        SynthRestore.applyOnce(this);               // remembered synth settings and sound bank, once per run
        DlsLoader.deleteStaleCopies(this);
        host = PlayerHost.get(this);
        remembered = Remembered.get(this, host);    // remembered output and menu toggles
        b = host.bridge();
        float hz = 60f;
        try { hz = getWindowManager().getDefaultDisplay().getRefreshRate(); } catch (RuntimeException ignored) { }
        pacer = new PlayerBridge.RefreshPacing(Runtime.getRuntime().availableProcessors(), hz);
        setContentView(build());
        Ui.systemBars(this, pal, pal.surface);
        if (savedInstanceState == null) openFromIntent(getIntent());   // started by another app with a MIDI file
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(getIntent().getAction())) host.onUsbAttached();   // started by plugging in a USB MIDI device
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, REQUEST_NOTIFICATIONS);
        }
    }

    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent ev) {
        NumberBox.outsideTap(this, ev);   // a tap outside the box being typed in applies the value and leaves it
        return super.dispatchTouchEvent(ev);
    }

    @Override
    protected void onResume() {
        super.onResume();
        visible = true;
        b = host.bridge();
        host.setScreen(new Runnable() { @Override public void run() { b = host.bridge(); draw(); } });
        host.refresh();   // draws, and arms the frame callback when playing or loading
    }

    @Override
    protected void onPause() {
        visible = false;
        if (statDebug) { statDebug = false; statDebugNote.setVisibility(View.GONE); }   // the layout test ends with the screen
        dismissMenu();
        host.setScreen(null);
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        frameArmed = false;
        stopLiveUpdates();
        super.onPause();
    }

    // ------------------------------------------------------------------------------------------------ layout test

    private static final int[] DEBUG_SEQUENCE = { 7, 9, 10, 99, 100, 101, 999, 1000, 9, 10, 100, 99 };

    private final Runnable statDebugRunner = new Runnable() {
        @Override public void run() {
            if (!statDebug) return;
            statDebugTick++;
            int len = DEBUG_SEQUENCE.length;
            for (int i = 0; i < statValue.length; i++) {
                int v = DEBUG_SEQUENCE[(statDebugTick + i) % len], w = DEBUG_SEQUENCE[(statDebugTick * 2 + i * 5 + 3) % len];
                String s = i < 2 ? v + " % of " + (w * 100) + " ms, " + w + " %" : v + " % / " + w + " events";
                setIfChanged(statValue[i], s);
            }
            if (statValue[0] != null) statValue[0].postDelayed(this, 50);
        }
    };

    private void toggleStatDebug() {
        statDebug = !statDebug;
        statDebugNote.setVisibility(statDebug ? View.VISIBLE : View.GONE);
        if (statDebug) statValue[0].post(statDebugRunner);
        else host.refresh();   // the real values again
    }

    // ------------------------------------------------------------------------------------------------ live updates

    /**
     * Live updates while playing or loading, paced by the display like the desktop's DisplayPacer (the rule is spelled out
     * in {@link PlayerBridge.RefreshPacing}): one Choreographer callback is armed at a time and armed again only when the
     * update of the previous one has finished, so a busy main thread skips display frames instead of queueing updates; the
     * adaptive tier lowers the statistics work and then the rate when the updates cost more than their budget. Other
     * states (stopped, or paused when the core needs no periodic refresh) update on events only.
     */
    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override public void doFrame(long frameTimeNanos) {
            frameArmed = false;
            if (!visible || b == null) { stopLiveUpdates(); return; }
            inFrame = true;
            try {
                if (pacer.onFrame(frameTimeNanos)) {
                    statsMask = pacer.halfStatistics() ? (statsPhase ? 0xF0 : 0x0F) : 0xFF;   // cells 0-3, then cells 4-7
                    statsPhase = !statsPhase;
                    statisticsNs = 0;
                    final long start = System.nanoTime();
                    host.refresh();
                    final long total = System.nanoTime() - start;
                    if (statsMask != 0xFF && b.n[PlayerBridge.N_NEEDS_REFRESH] == 0) {
                        statsMask = 0xFF;   // the run just ended on a half update: draw the other cells now (final values)
                        draw();
                    }
                    pacer.onUpdateDone(total - statisticsNs, statisticsNs, frameTimeNanos, b.n[PlayerBridge.N_CURRENT_LAG_US]);
                }
            } finally {
                inFrame = false;
                statsMask = 0xFF;
            }
            armLiveUpdates();
        }
    };

    /** Arms the next frame callback when live updates are needed and none is waiting or running. */
    private void armLiveUpdates() {
        if (inFrame || frameArmed || !visible || b == null) return;
        if (b.n[PlayerBridge.N_NEEDS_REFRESH] == 0) { stopLiveUpdates(); return; }
        if (!pacerActive) { pacerActive = true; pacer.restart(); pacer.resetCounters(); }   // counters describe the latest run
        frameArmed = true;
        Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    /** The run of live updates ended (stopped, or the screen left): one line of counters goes to the log. */
    private void stopLiveUpdates() {
        if (!pacerActive) return;
        pacerActive = false;
        Log.i("MIDIBN-pace", pacer.summary());
    }

    // ------------------------------------------------------------------------------------------------ layout

    private int dp(float v) { return Math.round(v * density); }

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    /** Layout params with a left margin in dp. */
    private LinearLayout.LayoutParams lpLeft(int w, int h, float weight, int left) {
        return lpSide(w, h, weight, left, 0);
    }

    /** Layout params with left and right margins in dp. */
    private LinearLayout.LayoutParams lpSide(int w, int h, float weight, int left, int right) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h, weight);
        p.leftMargin = dp(left);
        p.rightMargin = dp(right);
        return p;
    }

    private LinearLayout.LayoutParams lp(int w, int h, float weight, int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h, weight);
        p.topMargin = dp(top);
        return p;
    }

    private TextView text(String s, float sp, boolean bold) {
        return Ui.text(this, pal, s, sp, bold, pal.text);
    }

    /** Small secondary text (captions, labels). */
    private TextView muted(String s, float sp, boolean bold) {
        return Ui.text(this, pal, s, sp, bold, pal.textSecondary);
    }

    /** Section heading inside a card. */
    private TextView heading(String s) {
        TextView t = Ui.text(this, pal, s, Ui.HEADING, true, pal.accentText);
        t.setPadding(0, 0, 0, dp(Ui.S1));
        return t;
    }

    /** A section card: rounded surface, 16 dp padding; its children are laid out top to bottom. */
    private Section card(String id) {
        return new Section(this, pal, id);   // tapping its heading collapses the group (kept for this run only)
    }

    private Button button(String s) { return button(s, false); }

    /** primary: filled accent button (Open, Play); otherwise a tonal secondary button. */
    private Button button(String s, boolean primary) {
        Button bt = new Button(this);
        bt.setText(s);
        Ui.button(bt, pal, primary);
        return bt;
    }

    private View build() {
        final LinearLayout root = column();
        root.setFocusable(true);                  // takes the focus when typing in a number box ends
        root.setFocusableInTouchMode(true);
        homeView = root;
        root.setBackgroundColor(pal.background);
        // The bars reach under the system bars (top bar colour under the status bar, transport colour under the navigation
        // bar); the insets become their padding, and the body gets the side insets (landscape, cut-outs).
        final LinearLayout headerBox = column();
        final ScrollView scroll = new ScrollView(this);
        final LinearLayout transport = column();
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int l, t, r, bt;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars()
                            | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                    l = i.left; t = i.top; r = i.right; bt = i.bottom;
                } else {
                    l = insets.getSystemWindowInsetLeft(); t = insets.getSystemWindowInsetTop();
                    r = insets.getSystemWindowInsetRight(); bt = insets.getSystemWindowInsetBottom();
                }
                headerBox.setPadding(l, t, r, 0);
                scroll.setPadding(l, 0, r, 0);
                transport.setPadding(l + dp(Ui.S2), dp(Ui.S1), r + dp(Ui.S2), dp(Ui.S1) + bt);
                return insets;
            }
        });

        // top bar: the app name on the left, the menu button (three bars) on the right; it stays put while the body scrolls
        headerBox.setBackgroundColor(pal.bar);
        headerBox.setElevation(dp(4));
        LinearLayout header = row();
        header.setPadding(dp(Ui.S2), 0, dp(Ui.HALF), 0);
        TextView title = Ui.text(this, pal, getString(R.string.app_name), Ui.TITLE, true, pal.onBar);
        title.setSingleLine(true);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        // Debug read-out, shown nowhere by default: a long press on the app name shows the counters of the latest run of
        // live updates (frames, updates, skipped frames, update cost, tier). The same line goes to the log (tag MIDIBN-pace).
        title.setOnLongClickListener(new View.OnLongClickListener() {
            @Override public boolean onLongClick(View v) {
                new AlertDialog.Builder(MainActivity.this).setTitle("Live update counters")
                        .setMessage(pacer.summary()).setPositiveButton(android.R.string.ok, null).show();
                return true;
            }
        });
        header.addView(title, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1, 0));
        menuButton = new MenuBarsButton(this, pal.onBar);
        menuButton.setContentDescription("Menu");
        menuButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { showMenu(); }
        });
        header.addView(menuButton, lp(dp(48), dp(48), 0, 0));
        header.setMinimumHeight(dp(56));
        headerBox.addView(header, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));
        root.addView(headerBox, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));

        // scrolling area: one card per section
        scroll.setFillViewport(false);
        LinearLayout body = column();
        body.setPadding(dp(Ui.S2), dp(Ui.S2), dp(Ui.S2), dp(Ui.S2));
        scroll.addView(body, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, lp(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1, 0));

        final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;

        // ---- File and output
        final Section fileCard = card("main.file");
        body.addView(fileCard, lp(MATCH, WRAP, 0, 0));
        fileCard.setHeading(heading("File and output"));
        openButton = button("Open MIDI...", true);
        openButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (b.n[PlayerBridge.N_LOADING] != 0) { b.cancelLoad(); host.refresh(); return; }
                // Folders and MIDI files only (by type). Providers label .mid files with many different types (on a Sony
                // phone none of the usual MIDI types matched), so the two buttons below are the way out.
                pick(FilePicker.midi());
            }
        });
        fileCard.addView(openButton, lp(MATCH, WRAP, 0, 0));
        LinearLayout moreOpen = row();
        openAnyButton = button("Open any file...");
        openAnyButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pick(FilePicker.any()); }
        });
        openAppButton = button("Other app...");
        openAppButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pick(FilePicker.otherApp()); }
        });
        moreOpen.addView(openAnyButton, lpSide(0, WRAP, 1, 0, 4));
        moreOpen.addView(openAppButton, lpSide(0, WRAP, 1, 4, 0));
        fileCard.addView(moreOpen, lp(MATCH, WRAP, 0, 8));
        fileText = text("No file loaded", Ui.BODY, true);
        fileCard.addView(fileText, lp(MATCH, WRAP, 0, 12));
        fileInfo = muted("", Ui.DETAIL, false);
        fileCard.addView(fileInfo, lp(MATCH, WRAP, 0, 4));
        loadBox = column();
        loadProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        loadProgress.setMax(1000);
        Ui.progress(loadProgress, pal);
        loadBox.addView(loadProgress, lp(MATCH, WRAP, 0, 8));
        loadStatus = muted("", Ui.DETAIL, false);
        loadBox.addView(loadStatus, lp(MATCH, WRAP, 0, 0));
        fileCard.addView(loadBox, lp(MATCH, WRAP, 0, 0));

        fileCard.addView(muted("MIDI output", Ui.LABEL, true), lp(MATCH, WRAP, 0, 16));
        LinearLayout outRow = row();
        outputSpinner = new Spinner(this);
        Ui.spinner(outputSpinner, pal);
        outRow.addView(outputSpinner, lp(0, dp(48), 1, 0));
        rescanButton = button("Rescan");
        rescanButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { host.rescanOutputs(); b = host.bridge(); }
        });
        outRow.addView(rescanButton, lpLeft(WRAP, WRAP, 0, 8));
        fileCard.addView(outRow, lp(MATCH, WRAP, 0, 0));
        outputSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (applying || pos == b.n[PlayerBridge.N_OUTPUT_SELECTED]) return;
                remembered.userChoseOutput(pos);
                host.selectOutput(pos);
                host.refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        // ---- Processing model
        final Section procCard = card("main.processing");
        body.addView(procCard, lp(MATCH, WRAP, 0, 16));
        procCard.setHeading(heading("Processing model"));
        rateLabel = muted("Rate model:", Ui.LABEL, true);
        procCard.addView(rateLabel, lp(MATCH, WRAP, 0, 0));
        rateSpinner = new Spinner(this);
        Ui.spinner(rateSpinner, pal);
        procCard.addView(rateSpinner, lp(MATCH, dp(48), 0, 4));
        rateSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (applying || pos == b.n[PlayerBridge.N_RATE_CHOICE]) return;
                b.selectRateChoice(pos);
                host.refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        serviceRow = row();
        serviceLabel = text("", Ui.LABEL, false);
        serviceRow.addView(serviceLabel, lp(WRAP, WRAP, 0, 0));
        processingValue = number(0, 100000000L);
        serviceRow.addView(processingValue, lpSide(0, WRAP, 1, 4, 4));
        serviceUnit = text("", Ui.LABEL, false);
        serviceRow.addView(serviceUnit, lp(WRAP, WRAP, 0, 0));
        dinButton = button("DIN");
        dinButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { b.dinPreset(); host.refresh(); }
        });
        serviceRow.addView(dinButton, lpLeft(WRAP, WRAP, 0, 8));
        procCard.addView(serviceRow, lp(MATCH, WRAP, 0, 12));
        wireNumber(processingValue, false);
        rateSlider = new SeekBar(this);
        Ui.seekBar(rateSlider, pal);
        rateSlider.setMax(10000);
        rateSlider.setPadding(dp(8), dp(12), dp(8), dp(12));
        procCard.addView(rateSlider, lp(MATCH, WRAP, 0, 4));
        rateSlider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int value, boolean fromUser) {
                // The thumb position goes through the touch curve (SliderCurve); the core gets its own slider scale.
                if (fromUser && !applying) { b.setSlider(SliderCurve.toValue(value, SLIDER_POSITIONS, SLIDER_POSITIONS, SliderCurve.K_PROCESSING)); host.refresh(); }
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        queueRow = row();
        queueCheck = new CheckBox(this);
        Ui.checkBox(queueCheck, pal);
        queueCheck.setText("Queue length limit:");
        queueRow.addView(queueCheck, lp(WRAP, dp(48), 0, 0));
        queueValue = number(1, 1000000L);
        queueRow.addView(queueValue, lpSide(0, WRAP, 1, 4, 4));
        queueEvents = text("events", Ui.LABEL, false);
        queueRow.addView(queueEvents, lp(WRAP, WRAP, 0, 0));
        procCard.addView(queueRow, lp(MATCH, WRAP, 0, 8));
        queueCheck.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(CompoundButton c, boolean on) {
                if (applying || on == (b.n[PlayerBridge.N_QUEUE_CHECKED] != 0)) return;
                b.setQueueLimitChecked(on);
                host.refresh();
            }
        });
        wireNumber(queueValue, true);
        overflowLabel = muted("Overflow:", Ui.LABEL, true);
        procCard.addView(overflowLabel, lp(MATCH, WRAP, 0, 12));
        overflowSpinner = new Spinner(this);
        Ui.spinner(overflowSpinner, pal);
        procCard.addView(overflowSpinner, lp(MATCH, dp(48), 0, 4));
        overflowSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (applying || pos == b.n[PlayerBridge.N_OVERFLOW_SELECTED]) return;
                b.setOverflow(pos);
                host.refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        // ---- Statistics
        final Section statCard = card("main.statistics");
        body.addView(statCard, lp(MATCH, WRAP, 0, 16));
        statCard.setHeading(heading("Statistics"));
        // Hidden layout test: a long press on this heading cycles every value through lengths that change fast (9 %, 10 %,
        // 100 %, 1000 ...) so one can check on the phone that nothing around a value moves; a second long press stops it.
        statCard.setOnHeadingLongClick(new Runnable() { @Override public void run() { toggleStatDebug(); } });
        statDebugNote = muted("Layout test running: the values below are made up. Long-press the heading to stop.", Ui.DETAIL, true);
        statDebugNote.setVisibility(View.GONE);
        statCard.addView(statDebugNote, lp(MATCH, WRAP, 0, 0));
        // The cells of the desktop Analysis window's Playback statistics, in its order, then the maximum rate. Each cell is a
        // small caption over its value; the first two cells take a whole row (the timeline pair is the widest value), the rest
        // two to a row. Values wrap instead of scrolling sideways.
        for (int i = 0; i < PlayerBridge.STAT_CELLS; ) {
            final boolean whole = i < 2;
            LinearLayout r = new LinearLayout(this);
            r.setOrientation(LinearLayout.HORIZONTAL);
            for (int k = 0; k < (whole ? 1 : 2); k++, i++) {
                LinearLayout cell = column();
                cell.setBackground(Ui.tile(this, pal));
                cell.setPadding(dp(12), dp(Ui.S1), dp(12), dp(Ui.S1));
                cell.addView(muted(STAT_CAPTIONS[i], Ui.CAPTION, false));
                statValue[i] = text("", Ui.VALUE - 1, true);
                SteadyText.steady(statValue[i]);        // equal-width digits ...
                SteadyText.holdLines(statValue[i]);     // ... and a height that never shrinks back
                cell.addView(statValue[i], lp(ViewGroup.LayoutParams.MATCH_PARENT, WRAP, 0, 2));
                LinearLayout.LayoutParams cp = lp(0, WRAP, 1, 0);
                if (k == 1) cp.leftMargin = dp(Ui.S1);
                r.addView(cell, cp);
            }
            statCard.addView(r, lp(MATCH, WRAP, 0, 8));
        }
        pressureText = text("", Ui.DETAIL, false);
        pressureText.setTypeface(Typeface.MONOSPACE);
        pressureText.setFontFeatureSettings("tnum");
        statCard.addView(pressureText, lp(MATCH, WRAP, 0, 8));
        resetButton = button("Reset statistics");
        resetButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { b.resetStatistics(); host.refresh(); }
        });
        statCard.addView(resetButton, lp(MATCH, WRAP, 0, 16));

        // ---- transport, pinned at the bottom (its padding follows the insets, see the listener above)
        transport.setBackgroundColor(pal.surface);
        transport.setElevation(dp(8));
        transport.setPadding(dp(Ui.S2), dp(Ui.S1), dp(Ui.S2), dp(Ui.S1));
        timeline = new SeekBar(this);
        Ui.seekBar(timeline, pal);
        timeline.setPadding(dp(8), dp(8), dp(8), dp(4));
        transport.addView(timeline, lp(MATCH, WRAP, 0, 0));
        timeline.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int value, boolean fromUser) {
                if (fromUser) timeText.setText(time(value * 1000L) + " / " + time(b.n[PlayerBridge.N_DURATION_US]));
            }
            @Override public void onStartTrackingTouch(SeekBar s) { dragging = true; b.setTimelineDragging(true); }
            @Override public void onStopTrackingTouch(SeekBar s) {
                dragging = false;
                b.setTimelineDragging(false);
                b.seekToMicros(s.getProgress() * 1000L);
                host.refresh();
            }
        });
        timeText = text("00:00.000 / 00:00.000", Ui.LABEL, false);
        timeText.setTypeface(Typeface.MONOSPACE);
        timeText.setGravity(Gravity.CENTER_HORIZONTAL);
        transport.addView(timeText, lp(MATCH, WRAP, 0, 0));
        LinearLayout buttons = row();
        backButton = button("-5 s");
        playButton = button("Play", true);
        playButton.setTextSize(Ui.TITLE - 2);
        stopButton = button("Stop");
        forwardButton = button("+5 s");
        buttons.addView(backButton, lpSide(0, dp(56), 1, 0, 4));
        buttons.addView(playButton, lpSide(0, dp(56), 1.6f, 4, 4));
        buttons.addView(stopButton, lpSide(0, dp(56), 1, 4, 4));
        buttons.addView(forwardButton, lpSide(0, dp(56), 1, 4, 0));
        transport.addView(buttons, lp(MATCH, WRAP, 0, 4));
        backButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { b.seekByMicros(-5000000L); host.refresh(); }
        });
        forwardButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { b.seekByMicros(5000000L); host.refresh(); }
        });
        playButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { host.play(); }   // never waits for the output to open (P13-USB)
        });
        stopButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { host.stop(); }
        });
        root.addView(transport, lp(MATCH, WRAP, 0, 0));
        return root;
    }

    // ------------------------------------------------------------------------------------------------ menu

    /** The menu button: three bars drawn by hand (no font or image dependence), 48 dp touch target. */
    private static final class MenuBarsButton extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float density;

        MenuBarsButton(Context context, int color) {
            super(context);
            density = context.getResources().getDisplayMetrics().density;
            paint.setColor(color);
            setClickable(true);
            setFocusable(true);
            android.content.res.TypedArray a = context.obtainStyledAttributes(new int[] { android.R.attr.selectableItemBackgroundBorderless });
            try {
                android.graphics.drawable.Drawable d = a.getDrawable(0);
                if (d != null) setBackground(d);
            } finally {
                a.recycle();
            }
        }

        @Override protected void onDraw(Canvas canvas) {
            final float w = 24 * density, h = 3 * density, gap = 5 * density, r = h / 2;
            final float left = (getWidth() - w) / 2f;
            final float top = (getHeight() - (3 * h + 2 * gap)) / 2f;
            for (int i = 0; i < 3; i++) {
                final float y = top + i * (h + gap);
                canvas.drawRoundRect(new RectF(left, y, left + w, y + h), r, r, paint);
            }
        }
    }

    private void dismissMenu() {
        if (menu != null) {
            menu.dismiss();
            menu = null;
        }
    }

    /**
     * Opens the menu under the menu button: the six toggles of the desktop player's system menu (checked and enabled as the
     * player core says right now, read again every time the menu opens), then "Synth settings..." and "Help". Built from plain
     * views in a PopupWindow (long texts wrap, rows are at least 48 dp high); a tap on a row does its action and closes the menu.
     */
    private void showMenu() {
        if (b == null || menuButton == null) return;
        if (menu != null) { dismissMenu(); return; }
        b = host.bridge();
        final LinearLayout column = column();
        column.setPadding(dp(4), dp(4), dp(4), dp(4));
        for (int i = 0; i < TOGGLE_TEXT.length; i++) {
            final int which = i;
            CheckBox row = new CheckBox(this);
            Ui.checkBox(row, pal);
            row.setText(TOGGLE_TEXT[i]);
            row.setTextSize(Ui.BODY);
            row.setChecked(toggleState(i));
            row.setEnabled(toggleEnabled(i));
            row.setPadding(dp(8), 0, dp(8), 0);
            row.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    dismissMenu();
                    remembered.userToggled(which);   // flips it in the core, redraws, remembers the new state
                }
            });
            column.addView(row, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));
        }
        View divider = new View(this);
        divider.setBackgroundColor(pal.outline);
        column.addView(divider, lp(ViewGroup.LayoutParams.MATCH_PARENT, dp(1), 0, 4));
        column.addView(menuRow("Synth settings...", new Runnable() {
            @Override public void run() { startActivity(new Intent(MainActivity.this, SynthSettingsActivity.class)); }
        }), lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 4));
        column.addView(menuRow("Help", new Runnable() {
            @Override public void run() { startActivity(new Intent(MainActivity.this, HelpActivity.class)); }
        }), lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));
        column.addView(menuRow("About and licences", new Runnable() {
            @Override public void run() { startActivity(new Intent(MainActivity.this, AboutActivity.class)); }
        }), lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, 0));

        ScrollView scroller = new ScrollView(this);
        scroller.addView(column, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        GradientDrawable frame = new GradientDrawable();
        frame.setColor(pal.surface);
        frame.setCornerRadius(dp(Ui.CARD_RADIUS));
        frame.setStroke(dp(1), pal.inputBorder);
        scroller.setBackground(frame);
        scroller.setClipToOutline(true);   // rows and their ripples stay inside the rounded corners

        final int screenWidth = getResources().getDisplayMetrics().widthPixels;
        final int width = Math.min(screenWidth - dp(16), dp(360));
        column.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        int[] at = new int[2];
        menuButton.getLocationInWindow(at);
        View decor = getWindow().getDecorView();
        final int room = Math.max(dp(120), decor.getHeight() - (at[1] + menuButton.getHeight()) - dp(12));
        final int height = Math.min(column.getMeasuredHeight() + dp(2), room);

        PopupWindow popup = new PopupWindow(scroller, width, height, true);   // focusable: Back and a tap outside close it
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setOutsideTouchable(true);
        popup.setElevation(dp(8));
        popup.setOnDismissListener(new PopupWindow.OnDismissListener() {
            @Override public void onDismiss() { menu = null; }
        });
        menu = popup;
        popup.showAsDropDown(menuButton, 0, 0, Gravity.END);
    }

    private View menuRow(String label, final Runnable action) {
        TextView row = text(label, Ui.BODY, false);
        row.setMinHeight(dp(48));
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(16), 0, dp(16), 0);
        row.setClickable(true);
        android.content.res.TypedArray a = obtainStyledAttributes(new int[] { android.R.attr.selectableItemBackground });
        try {
            android.graphics.drawable.Drawable d = a.getDrawable(0);
            if (d != null) row.setBackground(d);
        } finally {
            a.recycle();
        }
        row.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                dismissMenu();
                action.run();
            }
        });
        return row;
    }

    /** A number box: tap to type, drag sideways to change the value (see {@link NumberBox}). */
    private NumberBox number(long min, long max) {
        NumberBox e = new NumberBox(this, pal);
        e.setGravity(Gravity.END);
        e.setMinWidth(dp(72));
        e.setRange(min, max);
        e.setHome(homeView);
        return e;
    }

    /**
     * A numeric field applies its value when the user presses Done, closes the keyboard, taps outside the box or leaves the
     * field in any other way (all of them end typing, and ending typing is losing the focus); a sideways drag changes the
     * value at once.
     */
    private void wireNumber(final NumberBox e, final boolean queue) {
        e.setScrub(new NumberBox.Scrub() {
            @Override public long current() { return b.n[queue ? PlayerBridge.N_QUEUE_VALUE : PlayerBridge.N_PROC_VALUE]; }
            @Override public void changed(long value) {
                if (queue) b.setQueueValue(value); else b.setProcessingValue(value);
                host.refresh();
            }
        });
        e.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override public boolean onEditorAction(TextView v, int action, android.view.KeyEvent ev) {
                if (action == EditorInfo.IME_ACTION_DONE) { e.finishTyping(); return true; }
                return false;
            }
        });
        e.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override public void onFocusChange(View v, boolean hasFocus) { if (!hasFocus) commit(e, queue); }
        });
    }

    private void commit(EditText e, boolean queue) {
        if (applying) return;
        long value;
        try { value = Long.parseLong(e.getText().toString().trim()); } catch (NumberFormatException ex) { host.refresh(); return; }
        long current = b.n[queue ? PlayerBridge.N_QUEUE_VALUE : PlayerBridge.N_PROC_VALUE];
        if (value == current) return;
        if (queue) b.setQueueValue(value); else b.setProcessingValue(value);
        host.refresh();
    }

    private boolean toggleState(int which) {
        switch (which) {
            case 0: return b.n[PlayerBridge.N_APPLY_QUEUE_CHECKED] != 0;
            case 1: return b.n[PlayerBridge.N_QUEUE_AGE_CHECKED] != 0;
            case 2: return b.n[PlayerBridge.N_CHASE_CHECKED] != 0;
            case 3: return b.n[PlayerBridge.N_PRESERVE_CHECKED] != 0;
            case 4: return b.n[PlayerBridge.N_PACED_CHECKED] != 0;
            default: return b.n[PlayerBridge.N_SCALE_CHECKED] != 0;
        }
    }

    private boolean toggleEnabled(int which) {
        switch (which) {
            case 0: return b.n[PlayerBridge.N_APPLY_QUEUE_ENABLED] != 0;
            case 1: return b.n[PlayerBridge.N_QUEUE_AGE_ENABLED] != 0;
            case 2: return b.n[PlayerBridge.N_CHASE_ENABLED] != 0;
            case 3: return b.n[PlayerBridge.N_PRESERVE_ENABLED] != 0;
            case 4: return b.n[PlayerBridge.N_PACED_ENABLED] != 0;
            default: return b.n[PlayerBridge.N_SCALE_ENABLED] != 0;
        }
    }

    // ------------------------------------------------------------------------------------------------ drawing

    static String time(long us) {
        long ms = Math.max(0, us) / 1000;
        long total = ms / 1000;
        long h = total / 3600, m = (total / 60) % 60, s = total % 60;
        if (h > 0) return String.format(java.util.Locale.US, "%d:%02d:%02d.%03d", h, m, s, ms % 1000);
        return String.format(java.util.Locale.US, "%02d:%02d.%03d", m, s, ms % 1000);
    }

    private void setItems(Spinner spinner, String joined, int selected) {
        String[] items = joined.isEmpty() ? new String[0] : joined.split("\n", -1);
        ArrayAdapter<String> adapter = Ui.adapter(this, items, pal);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        if (selected >= 0 && selected < items.length) spinner.setSelection(selected, false);
    }

    private void setNumber(EditText e, long value) {
        if (e.hasFocus()) return;             // never overwrite what the user is typing
        String s = Long.toString(value);
        if (!e.getText().toString().equals(s)) e.setText(s);
    }

    private void draw() {
        if (b == null) return;
        applying = true;
        final long[] n = b.n;
        final String[] t = b.t;
        openButton.setText(n[PlayerBridge.N_LOADING] != 0 ? "Cancel loading" : t[PlayerBridge.T_OPEN]);
        openAnyButton.setEnabled(n[PlayerBridge.N_LOADING] == 0);
        openAppButton.setEnabled(n[PlayerBridge.N_LOADING] == 0);
        fileText.setText(t[PlayerBridge.T_FILE]);
        fileInfo.setText(t[PlayerBridge.T_FILE_INFO]);
        fileInfo.setVisibility(t[PlayerBridge.T_FILE_INFO].isEmpty() ? View.GONE : View.VISIBLE);
        loadBox.setVisibility(n[PlayerBridge.N_LOADING] != 0 ? View.VISIBLE : View.GONE);
        loadProgress.setProgress((int) n[PlayerBridge.N_LOAD_OVERALL]);
        loadStatus.setText(t[PlayerBridge.T_LOAD_STATUS]);

        // output
        String oi = t[PlayerBridge.T_OUTPUT_ITEMS];
        int os = (int) n[PlayerBridge.N_OUTPUT_SELECTED];
        if (!oi.equals(outputItems)) { outputItems = oi; setItems(outputSpinner, oi, os); lastOutput = os; }
        else if (os != lastOutput) { outputSpinner.setSelection(os, false); lastOutput = os; }
        outputSpinner.setEnabled(n[PlayerBridge.N_OUTPUT_ENABLED] != 0);
        rescanButton.setEnabled(n[PlayerBridge.N_STATE] == PlayerBridge.STATE_STOPPED && n[PlayerBridge.N_LOADING] == 0);

        // processing model
        rateLabel.setText(t[PlayerBridge.T_RATE_LABEL]);
        String ri = t[PlayerBridge.T_RATE_ITEMS];
        int rc = (int) n[PlayerBridge.N_RATE_CHOICE];
        if (!ri.equals(rateItems)) { rateItems = ri; setItems(rateSpinner, ri, rc); lastRate = rc; }
        else if (rc != lastRate) { rateSpinner.setSelection(rc, false); lastRate = rc; }
        boolean serviceVisible = n[PlayerBridge.N_PROC_VISIBLE] != 0 && !t[PlayerBridge.T_SERVICE_LABEL].isEmpty();
        serviceRow.setVisibility(serviceVisible ? View.VISIBLE : View.GONE);
        serviceLabel.setText(t[PlayerBridge.T_SERVICE_LABEL] + " ");
        setNumber(processingValue, n[PlayerBridge.N_PROC_VALUE]);
        processingValue.setEnabled(n[PlayerBridge.N_PROC_ENABLED] != 0);
        serviceUnit.setText(" " + t[PlayerBridge.T_SERVICE_UNIT]);
        serviceUnit.setVisibility(n[PlayerBridge.N_UNIT_VISIBLE] != 0 ? View.VISIBLE : View.GONE);
        dinButton.setVisibility(n[PlayerBridge.N_DIN_VISIBLE] != 0 ? View.VISIBLE : View.GONE);
        dinButton.setEnabled(n[PlayerBridge.N_DIN_ENABLED] != 0);
        rateSlider.setVisibility(serviceVisible ? View.VISIBLE : View.GONE);
        // (the thumb moves only when its own position no longer gives the core's value: no jitter under the finger)
        if (SliderCurve.toValue(rateSlider.getProgress(), SLIDER_POSITIONS, SLIDER_POSITIONS, SliderCurve.K_PROCESSING) != n[PlayerBridge.N_SLIDER_VALUE])
            rateSlider.setProgress(SliderCurve.toPosition((int) n[PlayerBridge.N_SLIDER_VALUE], SLIDER_POSITIONS, SLIDER_POSITIONS, SliderCurve.K_PROCESSING));
        rateSlider.setEnabled(n[PlayerBridge.N_SLIDER_ENABLED] != 0);

        queueCheck.setText(t[PlayerBridge.T_QUEUE_CHECK]);
        queueCheck.setChecked(n[PlayerBridge.N_QUEUE_CHECKED] != 0);
        queueCheck.setEnabled(n[PlayerBridge.N_QUEUE_ENABLED] != 0);
        setNumber(queueValue, n[PlayerBridge.N_QUEUE_VALUE]);
        queueValue.setEnabled(n[PlayerBridge.N_QUEUE_VALUE_ENABLED] != 0);
        queueEvents.setText(" " + t[PlayerBridge.T_EVENTS_LABEL]);
        overflowLabel.setText(t[PlayerBridge.T_OVERFLOW_LABEL]);
        String ov = t[PlayerBridge.T_OVERFLOW_ITEMS];
        int osel = (int) n[PlayerBridge.N_OVERFLOW_SELECTED];
        if (!ov.equals(overflowItems)) { overflowItems = ov; setItems(overflowSpinner, ov, osel); lastOverflow = osel; }
        else if (osel != lastOverflow) { overflowSpinner.setSelection(osel, false); lastOverflow = osel; }
        overflowSpinner.setEnabled(n[PlayerBridge.N_OVERFLOW_ENABLED] != 0);

        // statistics
        // (the cost of this block is measured apart: the adaptive tier draws half of the cells on one update, half on the next)
        final long statisticsStart = System.nanoTime();
        for (int i = 0; i < PlayerBridge.STAT_CELLS; i++)
            if ((statsMask & (1 << i)) != 0 && !statDebug) setIfChanged(statValue[i], t[STAT_TEXT[i]]);
        boolean limited = n[PlayerBridge.N_QUEUE_LIMITED] != 0;
        pressureText.setVisibility(limited ? View.VISIBLE : View.GONE);
        setIfChanged(pressureText, t[PlayerBridge.T_PRESSURE]);
        pressureText.setTextColor(n[PlayerBridge.N_OVERFLOW_PULSE] != 0 ? pal.warn : pal.text);
        statisticsNs += System.nanoTime() - statisticsStart;

        // transport
        playButton.setText(t[PlayerBridge.T_PLAY]);
        playButton.setEnabled(n[PlayerBridge.N_PLAY_ENABLED] != 0);
        String opening = host.openingText();   // the output is still opening: Play starts when it is ready
        if (opening != null) setIfChanged(statValue[0], opening);
        stopButton.setEnabled(opening != null || n[PlayerBridge.N_STOP_ENABLED] != 0);
        boolean seek = n[PlayerBridge.N_SEEK_ENABLED] != 0;
        backButton.setEnabled(seek);
        forwardButton.setEnabled(seek);
        timeline.setEnabled(seek);
        if (!dragging) {
            long durMs = Math.max(1, n[PlayerBridge.N_DURATION_US] / 1000);
            timeline.setMax((int) Math.min(Integer.MAX_VALUE, durMs));
            timeline.setProgress((int) Math.min(Integer.MAX_VALUE, n[PlayerBridge.N_POSITION_US] / 1000));
            setIfChanged(timeText, time(n[PlayerBridge.N_POSITION_US]) + " / " + time(n[PlayerBridge.N_DURATION_US]));
        }
        applying = false;
        showPending();
        if (remembered.pending() && !rememberedPosted && n[PlayerBridge.N_STATE] == PlayerBridge.STATE_STOPPED) {
            rememberedPosted = true;   // remembered output / toggles still waiting (a device can appear a moment after the start)
            getWindow().getDecorView().post(new Runnable() {
                @Override public void run() { rememberedPosted = false; if (visible) remembered.step(); }
            });
        }
        armLiveUpdates();   // playing or loading: the display-paced updates run (a no-op while one is armed or running)
    }

    /** Sets a text only when it differs (a repeated setText still costs a layout pass). */
    private static void setIfChanged(TextView view, String text) {
        if (text == null) text = "";
        if (!text.contentEquals(view.getText())) view.setText(text);
    }

    /** Messages and the large-file decision the controller has queued. */
    private void showPending() {
        if (!visible) return;
        String[] messages = b.takeMessages();
        for (int i = 0; messages != null && i + 2 < messages.length; i += 3) {
            AlertDialog.Builder d = new AlertDialog.Builder(this);
            d.setTitle(messages[i + 1]);
            d.setMessage(messages[i + 2]);
            d.setPositiveButton(android.R.string.ok, null);
            d.show();
        }
        String notice = host.takeNotice();   // a failed open: plain sentence, then Details
        if (notice != null) {
            new AlertDialog.Builder(this).setTitle("Unable to start playback").setMessage(notice)
                    .setPositiveButton(android.R.string.ok, null).show();
        }
        if (!decisionOpen) {
            String[] decision = b.largeFileDecision();
            if (decision != null) {
                decisionOpen = true;
                AlertDialog.Builder d = new AlertDialog.Builder(this);
                d.setTitle(decision[0]);
                d.setMessage(decision[1] + "\n\n" + decision[2]);
                d.setCancelable(false);
                d.setPositiveButton("Continue", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface di, int w) { decisionOpen = false; b.answerLargeFile(true); host.refresh(); }
                });
                d.setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface di, int w) { decisionOpen = false; b.answerLargeFile(false); host.refresh(); }
                });
                d.show();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_OPEN && resultCode == RESULT_OK && data != null && data.getData() != null) openChosen(data.getData());
    }

    /** Opens a chosen or shared file; a failure gets the plain message. */
    private void openChosen(Uri uri) {
        String error = host.openUri(uri);
        if (error != null) {
            new AlertDialog.Builder(this).setTitle("Unable to open the file").setMessage(error)
                    .setPositiveButton(android.R.string.ok, null).show();
        }
    }

    /** Starts a picker; a phone without one gets a plain message. */
    private void pick(Intent picker) {
        try {
            startActivityForResult(picker, REQUEST_OPEN);
        } catch (RuntimeException e) {
            new AlertDialog.Builder(this).setTitle("No file picker").setMessage("This phone has no app that can choose a file.")
                    .setPositiveButton(android.R.string.ok, null).show();
        }
    }

    /**
     * A MIDI file handed over by another app (open or share): ACTION_VIEW with the file in the data, ACTION_SEND with it in
     * EXTRA_STREAM. Opens it like a chosen file. Returns true when the intent carried a file.
     */
    private boolean openFromIntent(Intent intent) {
        if (intent == null) return false;
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) uri = intent.getData();
        else if (Intent.ACTION_SEND.equals(intent.getAction())) {
            try { uri = intent.getParcelableExtra(Intent.EXTRA_STREAM); } catch (RuntimeException ignored) { }
        }
        if (uri == null) return false;
        intent.setAction(Intent.ACTION_MAIN);   // handled: a rotation or a return to the app does not open it again
        openChosen(uri);
        return true;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) host.onUsbAttached();
        else openFromIntent(intent);
    }
}
