package org.midibottleneck.player;

import android.app.Activity;
import android.content.Intent;
import android.content.res.TypedArray;
import android.database.Cursor;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;

import org.midibottleneck.core.route.SynthMidiRoute;

/**
 * Settings of the built-in synthesizer (Sonivox): volume, voice limit (polyphony), reverb on or off, reverb preset and
 * reverb amount, an optional DLS sound bank, a level meter with a clip light, and the synth's status line at the bottom.
 * Plain framework views built in code, like the other screens, in the player's look ({@link Ui}, {@link Palette}).
 *
 * Every control applies at once: it calls a static setter of {@link SynthMidiRoute}, which only stores the value in the
 * native settings; the audio thread applies it before its next buffer, so changes are heard while playing and never touch
 * the synthesizer from this thread. Each change is also written to the app's preferences. If the synth is not open (not chosen as the output, or not playing) the values are kept
 * and used when it opens.
 *
 * Voice limit: the slider covers 1 to 1024; the number field accepts any value from 1 to the build maximum. A typed value
 * above the slider's range is applied and the slider is shown dimmed and pinned at its end until the user touches it.
 *
 * Sound bank: "Load DLS sound bank..." opens the system file picker (every file selectable), reads the chosen file through
 * the content resolver on a background thread and hands the finished bank to the audio thread; a document provider that
 * cannot be read at an offset is copied to the app's cache first and the copy is deleted straight after loading
 * (all of that is {@link DlsLoader}). The settings and the bank are remembered ({@link Prefs}, {@link SynthRestore}); the
 * "Recently loaded sound banks" list shows the last eight banks, a tap loads one.
 */
public class SynthSettingsActivity extends Activity {
    private static final int REQUEST_DLS = 41;
    private static final int VOICE_POSITIONS = 10000;   // thumb positions of the voice slider (value = SliderCurve of the position)
    private static final int SLIDER_VOICES = 1024;     // the voice slider covers 1 to this (or the build maximum, if smaller)

    private final Handler handler = new Handler(Looper.getMainLooper());
    private float density;
    private boolean applying;       // true while the controls are set from the stored values
    private boolean visible;

    private SeekBar volumeSlider, voiceSlider, reverbSlider;
    private TextView volumeLabel, voiceLabel, voiceHint, reverbLabel, statusText;
    private NumberBox voiceEdit;
    private CheckBox reverbCheck;
    private Spinner presetSpinner;
    private int voiceMax = 1;       // the library's maximum, read in load()
    private int sliderMax = 1;      // the largest value the slider reaches
    private boolean voiceBypass;    // the stored voice limit is above the slider's range

    private TextView soundLabel, soundError;
    private Button loadButton, builtInButton;
    private LinearLayout recentBox;
    private TextView recentTitle;
    private final java.util.Set<String> unavailable = new java.util.HashSet<String>();   // recent banks that no longer open
    private boolean lastBusy;

    private ProgressBar meterBar;
    private TextView clipLamp, clipCount;
    private int lastClipBuffers = -1;
    private long clipLitUntil;
    private boolean lampLit;
    private Drawable lampOn, lampOff;
    private Palette pal;
    private int tick;

    private int dp(float v) { return Math.round(v * density); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        density = getResources().getDisplayMetrics().density;
        pal = Ui.palette(this);
        setContentView(build());
        Ui.systemBars(this, pal, pal.surface);
        SynthRestore.applyOnce(this);
        DlsLoader.deleteStaleCopies(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        visible = true;
        load();
        checkRecent();
        handler.post(refresh);
    }

    @Override
    protected void onPause() {
        visible = false;
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    /** The level meter (ten times a second) and the status line (twice a second) while the page is open. */
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (!visible) return;
            if (tick++ % 5 == 0) {
                String status = SynthMidiRoute.liveStatus();
                statusText.setText(status != null ? status
                        : "The built-in synth is not running. Choose \"Built-in synth (Sonivox)\" as the MIDI output and press Play; "
                        + "these settings are used as soon as it starts.");
            }
            showMeter();
            if (DlsLoader.isLoading() != lastBusy) { lastBusy = DlsLoader.isLoading(); showBank(); }   // a load started elsewhere (app start)
            if (!DlsLoader.note().isEmpty()) { soundError.setText(DlsLoader.note()); DlsLoader.clearNote(); showBank(); }
            handler.postDelayed(this, 100);
        }
    };

    private void showMeter() {
        int[] m = SynthMidiRoute.meter();   // peak %, buffers with clipping, clipped samples; null when the synth is not open
        long now = System.currentTimeMillis();
        if (m == null) {
            meterBar.setProgress(0);
            lastClipBuffers = -1;
            clipCount.setText("Level meter: the synth is not running");
        } else {
            meterBar.setProgress(m[0]);
            if (lastClipBuffers >= 0 && m[1] != lastClipBuffers) clipLitUntil = now + 1000;   // lit for about a second
            lastClipBuffers = m[1];
            clipCount.setText("Peak " + m[0] + " %   Clipped buffers: " + m[1]);
        }
        boolean lit = now < clipLitUntil;
        if (lit == lampLit) return;
        lampLit = lit;
        clipLamp.setBackground(lit ? lampOn : lampOff);
        clipLamp.setTextColor(lit ? (pal.dark ? pal.background : 0xFFFFFFFF) : pal.textSecondary);
    }

    // ------------------------------------------------------------------------------------------------ layout

    private LinearLayout.LayoutParams lp(int w, int h, float weight, int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h, weight);
        p.topMargin = dp(topDp);
        return p;
    }

    private TextView text(String s, float sp, boolean bold) { return Ui.text(this, pal, s, sp, bold, pal.text); }

    /** Explanations under the controls. */
    private TextView hint(String s) { return Ui.text(this, pal, s, Ui.DETAIL, false, pal.textSecondary); }

    /** Headings of the cards (they also show the current value). */
    private TextView heading(String s) { return Ui.text(this, pal, s, Ui.HEADING, true, pal.accentText); }

    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        Ui.button(b, pal, false);
        return b;
    }

    private SeekBar slider(int max) {
        SeekBar s = new SeekBar(this);
        s.setMax(max);
        s.setPadding(dp(16), dp(14), dp(16), dp(14));   // thumb area at least 48 dp high
        Ui.seekBar(s, pal);
        return s;
    }

    /** One group of controls: a rounded card on the page background, 16 dp below the one before. */
    private Section card(LinearLayout body, String id) {
        Section c = new Section(this, pal, id);   // tapping its heading collapses the group (kept for this run only)
        body.addView(c, lp(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0, Ui.S2));
        return c;
    }

    private View build() {
        final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;
        LinearLayout root = new LinearLayout(this);
        root.setFocusable(true);                  // takes the focus when typing in the number box ends
        root.setFocusableInTouchMode(true);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(pal.background);
        final LinearLayout bar = new LinearLayout(this);
        final ScrollView scroll = new ScrollView(this);
        final LinearLayout meter = new LinearLayout(this);
        // The top bar reaches under the status bar and the meter panel under the navigation bar (same look as the main
        // screen); the insets become their padding.
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int l, t, r, b;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    l = i.left; t = i.top; r = i.right; b = i.bottom;
                } else {
                    l = insets.getSystemWindowInsetLeft(); t = insets.getSystemWindowInsetTop();
                    r = insets.getSystemWindowInsetRight(); b = insets.getSystemWindowInsetBottom();
                }
                bar.setPadding(l + dp(Ui.HALF), t, r + dp(Ui.S2), 0);
                scroll.setPadding(l, 0, r, 0);
                meter.setPadding(l + dp(Ui.S2), dp(Ui.S1), r + dp(Ui.S2), dp(Ui.S1) + b);
                return insets;
            }
        });

        // top bar
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(pal.bar);
        bar.setElevation(dp(4));
        bar.setMinimumHeight(dp(56));
        Button back = new Button(this);
        back.setText("Back");
        back.setAllCaps(false);
        back.setTextSize(Ui.BODY);
        back.setTextColor(pal.onBar);
        back.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        back.setStateListAnimator(null);
        back.setMinHeight(dp(Ui.TOUCH));
        back.setMinimumHeight(dp(Ui.TOUCH));
        back.setMinWidth(dp(Ui.TOUCH));
        back.setMinimumWidth(dp(Ui.TOUCH));
        back.setPadding(dp(12), 0, dp(12), 0);
        TypedArray a = obtainStyledAttributes(new int[] { android.R.attr.selectableItemBackgroundBorderless });
        try {
            back.setBackground(a.getDrawable(0));
        } finally {
            a.recycle();
        }
        back.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });
        bar.addView(back, lp(WRAP, WRAP, 0, 0));
        TextView title = Ui.text(this, pal, "Synth settings", Ui.TITLE, true, pal.onBar);
        title.setPadding(dp(Ui.HALF), 0, 0, 0);
        bar.addView(title, lp(0, WRAP, 1, 0));
        root.addView(bar, lp(MATCH, WRAP, 0, 0));

        // scrolling controls, one card per group
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(Ui.S2), 0, dp(Ui.S2), dp(Ui.S2));
        scroll.addView(body, new ViewGroup.LayoutParams(MATCH, WRAP));
        root.addView(scroll, lp(MATCH, 0, 1, 0));

        TextView intro = hint("For the built-in synth (Sonivox). Changes are heard at once while playing. "
                + "The settings are remembered and come back when the app is started again.");
        body.addView(intro, lp(MATCH, WRAP, 0, Ui.S2));

        // ---- volume
        Section volumeCard = card(body, "synth.volume");
        volumeLabel = heading("");
        volumeCard.setHeading(volumeLabel);
        volumeSlider = slider(SynthMidiRoute.VOLUME_MAX);
        volumeCard.addView(volumeSlider, lp(MATCH, WRAP, 0, 0));
        volumeSlider.setOnSeekBarChangeListener(new Changed() {
            @Override void changed(int value) {
                SynthMidiRoute.setVolume(value);
                SynthRestore.save(SynthSettingsActivity.this);
                showVolume(value);
            }
        });
        volumeCard.addView(hint("Lower the volume if the CLIP light comes on: below 100 % the volume is applied before the synth's final "
                + "limiter, so loud passages get room instead of being cut flat."), lp(MATCH, WRAP, 0, 0));

        // ---- voices
        Section voiceCard = card(body, "synth.voices");
        voiceLabel = heading("");
        voiceCard.setHeading(voiceLabel);
        LinearLayout voiceRow = new LinearLayout(this);
        voiceRow.setOrientation(LinearLayout.HORIZONTAL);
        voiceRow.setGravity(Gravity.CENTER_VERTICAL);
        voiceSlider = slider(VOICE_POSITIONS);   // thumb positions; the voice count follows the touch curve (SliderCurve)
        voiceRow.addView(voiceSlider, lp(0, WRAP, 1, 0));
        voiceEdit = new NumberBox(this, pal);
        voiceEdit.setGravity(Gravity.CENTER);
        voiceEdit.setTextSize(Ui.BODY);
        voiceEdit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(6)});
        voiceEdit.setContentDescription("Voice limit: tap to type a number, or drag sideways on the box to change it");
        voiceEdit.setHome(root);
        voiceEdit.setRange(1, voiceMax);
        voiceEdit.setScrub(new NumberBox.Scrub() {
            @Override public long current() { return SynthMidiRoute.settings()[1]; }
            @Override public void changed(long value) {
                voiceHint.setText("");
                SynthMidiRoute.setVoiceLimit((int) value);
                SynthRestore.save(SynthSettingsActivity.this);
                showVoices((int) value, true);
            }
        });
        voiceEdit.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                if (actionId == EditorInfo.IME_ACTION_DONE || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER)) {
                    applyTypedVoices();
                    return true;
                }
                return false;
            }
        });
        // Leaving the box in any way (Done, the keyboard's close button, a tap outside) applies what was typed.
        voiceEdit.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override public void onFocusChange(View v, boolean hasFocus) { if (!hasFocus && !applying) applyTypedVoices(); }
        });
        voiceRow.addView(voiceEdit, lp(dp(84), WRAP, 0, 0));
        Button setVoices = button("Set");
        setVoices.setMinWidth(dp(56));
        setVoices.setMinimumWidth(dp(56));
        setVoices.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { applyTypedVoices(); }
        });
        LinearLayout.LayoutParams setParams = lp(WRAP, WRAP, 0, 0);
        setParams.leftMargin = dp(Ui.S1);
        voiceRow.addView(setVoices, setParams);
        voiceCard.addView(voiceRow, lp(MATCH, WRAP, 0, Ui.HALF));
        voiceHint = Ui.text(this, pal, "", Ui.DETAIL, true, pal.warn);
        voiceCard.addView(voiceHint, lp(MATCH, WRAP, 0, 0));
        voiceSlider.setOnSeekBarChangeListener(new Changed() {
            @Override void changed(int value) {
                int voices = voicesAt(value);
                voiceHint.setText("");
                SynthMidiRoute.setVoiceLimit(voices);
                SynthRestore.save(SynthSettingsActivity.this);
                showVoices(voices, false);
            }
            @Override public void onStartTrackingTouch(SeekBar s) {
                // The slider takes over from a typed value above its range as soon as it is touched.
                if (voiceBypass && !applying) {
                    int voices = voicesAt(s.getProgress());
                    voiceHint.setText("");
                    SynthMidiRoute.setVoiceLimit(voices);
                    SynthRestore.save(SynthSettingsActivity.this);
                    showVoices(voices, false);
                }
            }
        });
        voiceCard.addView(hint("How many notes can sound at once. The synth starts with " + SynthMidiRoute.DEFAULT_VOICES
                + ". More voices need more processing; when the limit is reached, older notes are cut off. "
                + "The slider goes to " + SLIDER_VOICES + " and is finer at low values; the number box takes any value up to the build "
                + "maximum: tap it to type, or drag sideways on it to change the value."), lp(MATCH, WRAP, 0, 0));

        // ---- reverb
        Section reverbCard = card(body, "synth.reverb");
        reverbCard.setHeading(heading("Reverb"));
        reverbCheck = new CheckBox(this);
        reverbCheck.setText("Reverb on");
        reverbCheck.setTextSize(Ui.BODY);
        Ui.checkBox(reverbCheck, pal);
        reverbCard.addView(reverbCheck, lp(MATCH, WRAP, 0, 0));
        reverbCheck.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(CompoundButton c, boolean on) {
                if (applying) return;
                SynthMidiRoute.setReverbOn(on);
                SynthRestore.save(SynthSettingsActivity.this);
                reverbEnabled(on);
            }
        });

        reverbCard.addView(text("Reverb type", Ui.LABEL, true), lp(MATCH, WRAP, 0, Ui.HALF));
        presetSpinner = new Spinner(this);
        Ui.spinner(presetSpinner, pal);
        presetSpinner.setAdapter(Ui.adapter(this, SynthMidiRoute.REVERB_PRESETS, pal));
        reverbCard.addView(presetSpinner, lp(MATCH, dp(Ui.TOUCH), 0, Ui.HALF));
        presetSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                // The spinner also reports the selection that load() set; only a different one is the user's choice.
                if (!applying && pos != SynthMidiRoute.settings()[3]) {
                    SynthMidiRoute.setReverbPreset(pos);
                    SynthRestore.save(SynthSettingsActivity.this);
                }
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        reverbLabel = text("", Ui.LABEL, true);
        reverbCard.addView(reverbLabel, lp(MATCH, WRAP, 0, Ui.S2));
        reverbSlider = slider(100);
        reverbCard.addView(reverbSlider, lp(MATCH, WRAP, 0, 0));
        reverbSlider.setOnSeekBarChangeListener(new Changed() {
            @Override void changed(int value) {
                SynthMidiRoute.setReverbAmount(value);
                SynthRestore.save(SynthSettingsActivity.this);
                showReverb(value);
            }
        });

        Button reset = button("Reset to defaults");
        reset.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                SynthMidiRoute.resetSettings();
                SynthRestore.save(SynthSettingsActivity.this);
                voiceHint.setText("");
                load();
            }
        });
        body.addView(reset, lp(MATCH, WRAP, 0, Ui.S2));

        // ---- sound bank
        Section soundCard = card(body, "synth.sounds");
        soundCard.setHeading(heading("Sounds"));
        soundLabel = text("", Ui.LABEL, false);
        soundCard.addView(soundLabel, lp(MATCH, WRAP, 0, Ui.HALF));
        loadButton = button("Load DLS sound bank...");
        loadButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickBank(); }
        });
        soundCard.addView(loadButton, lp(MATCH, WRAP, 0, Ui.S1));
        builtInButton = button("Use built-in sounds");
        builtInButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                SynthMidiRoute.useBuiltInSounds();
                DlsLoader.forgetCurrent(SynthSettingsActivity.this);
                soundError.setText("");
                showBank();
            }
        });
        soundCard.addView(builtInButton, lp(MATCH, WRAP, 0, Ui.S1));
        soundError = Ui.text(this, pal, "", Ui.DETAIL, true, pal.warn);
        soundCard.addView(soundError, lp(MATCH, WRAP, 0, Ui.HALF));
        soundCard.addView(hint("A DLS bank replaces the built-in sounds for the instruments it contains; the other instruments keep the "
                + "built-in sounds. The app remembers the bank and loads it again at the next start."), lp(MATCH, WRAP, 0, Ui.HALF));
        recentTitle = text("Recently loaded sound banks", Ui.LABEL, true);
        soundCard.addView(recentTitle, lp(MATCH, WRAP, 0, Ui.S2));
        recentBox = new LinearLayout(this);
        recentBox.setOrientation(LinearLayout.VERTICAL);
        soundCard.addView(recentBox, lp(MATCH, WRAP, 0, 0));

        // level meter and status line, pinned at the bottom (like the transport bar of the main screen)
        meter.setOrientation(LinearLayout.VERTICAL);
        meter.setBackgroundColor(pal.surface);
        meter.setElevation(dp(8));
        LinearLayout meterRow = new LinearLayout(this);
        meterRow.setOrientation(LinearLayout.HORIZONTAL);
        meterRow.setGravity(Gravity.CENTER_VERTICAL);
        meterRow.addView(text("Level", Ui.LABEL, true), lp(WRAP, WRAP, 0, 0));
        meterBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        meterBar.setMax(100);
        Ui.progress(meterBar, pal);
        LinearLayout.LayoutParams barParams = lp(0, dp(16), 1, 0);
        barParams.leftMargin = dp(Ui.S1);
        barParams.rightMargin = dp(Ui.S1);
        meterRow.addView(meterBar, barParams);
        lampOn = Ui.box(this, pal.warn, pal.warn, 0, Ui.CONTROL_RADIUS);
        lampOff = Ui.box(this, pal.tile, pal.outline, 1, Ui.CONTROL_RADIUS);
        clipLamp = text("CLIP", Ui.LABEL, true);
        clipLamp.setGravity(Gravity.CENTER);
        clipLamp.setPadding(dp(10), dp(4), dp(10), dp(4));
        clipLamp.setBackground(lampOff);
        clipLamp.setTextColor(pal.textSecondary);
        meterRow.addView(clipLamp, lp(WRAP, WRAP, 0, 0));
        meter.addView(meterRow, lp(MATCH, WRAP, 0, 0));
        clipCount = text("", Ui.DETAIL, false);
        meter.addView(clipCount, lp(MATCH, WRAP, 0, 2));
        statusText = hint("");
        meter.addView(statusText, lp(MATCH, WRAP, 0, Ui.HALF));
        root.addView(meter, lp(MATCH, WRAP, 0, 0));
        return root;
    }

    /** A slider that reports only the user's own moves. */
    private abstract class Changed implements SeekBar.OnSeekBarChangeListener {
        abstract void changed(int value);
        @Override public void onProgressChanged(SeekBar s, int value, boolean fromUser) { if (fromUser && !applying) changed(value); }
        @Override public void onStartTrackingTouch(SeekBar s) {}
        @Override public void onStopTrackingTouch(SeekBar s) {}
    }

    // ------------------------------------------------------------------------------------------------ values

    private void showVolume(int percent) { volumeLabel.setText("Volume: " + percent + " %" + (percent > 100 ? " (may distort)" : "")); }

    /** Shows a voice limit that is already stored: label, number field, slider (dimmed and pinned when above its range). */
    private void showVoices(int voices, boolean updateSlider) {
        boolean above = voices > sliderMax;
        voiceBypass = above;
        boolean was = applying;
        applying = true;
        if (updateSlider) voiceSlider.setProgress(above ? VOICE_POSITIONS : positionOf(voices));
        voiceSlider.setAlpha(above ? 0.4f : 1f);
        if (!voiceEdit.hasFocus() || above || updateSlider) voiceEdit.setText(String.valueOf(voices));
        applying = was;
        voiceLabel.setText("Voice limit (polyphony): " + voices + " of " + voiceMax
                + (above ? " (typed; the slider is bypassed until you touch it)" : ""));
    }

    /** The number field: 1 to the build maximum is applied, anything else gets a one-line hint and is not applied. */
    private void applyTypedVoices() {
        String typed = voiceEdit.getText().toString().trim();
        int value = -1;
        try { value = typed.isEmpty() ? -1 : Integer.parseInt(typed); } catch (NumberFormatException e) { value = -1; }
        if (value < 1 || value > voiceMax) {
            voiceHint.setText("Type a whole number from 1 to " + voiceMax + ".");
            return;
        }
        voiceHint.setText("");
        SynthMidiRoute.setVoiceLimit(value);
        SynthRestore.save(this);
        showVoices(value, true);
        voiceEdit.finishTyping();   // keyboard away, focus to the page
    }

    /** Voice count at a thumb position of the voice slider (1 to the slider's end, on the touch curve). */
    private int voicesAt(int position) {
        return 1 + SliderCurve.toValue(position, VOICE_POSITIONS, Math.max(0, sliderMax - 1), SliderCurve.K_VOICES);
    }

    private int positionOf(int voices) {
        return SliderCurve.toPosition(Math.max(0, voices - 1), VOICE_POSITIONS, Math.max(0, sliderMax - 1), SliderCurve.K_VOICES);
    }

    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent ev) {
        NumberBox.outsideTap(this, ev);   // a tap outside the box being typed in applies the value and leaves it
        return super.dispatchTouchEvent(ev);
    }

    private void showReverb(int percent) { reverbLabel.setText("Reverb amount: " + percent + " %"); }

    private void reverbEnabled(boolean on) {
        presetSpinner.setEnabled(on);
        reverbSlider.setEnabled(on);
    }

    /** Sets every control from the stored values (they live in the native settings for as long as the app runs). */
    private void load() {
        int[] s = SynthMidiRoute.settings();   // volume %, voice limit, reverb on, preset, amount %, (build maximum of voices)
        applying = true;
        voiceMax = Math.max(1, SynthMidiRoute.maxVoices());   // from the compiled library, never a number written here
        sliderMax = Math.min(SLIDER_VOICES, voiceMax);
        voiceSlider.setMax(VOICE_POSITIONS);
        voiceEdit.setRange(1, voiceMax);
        volumeSlider.setProgress(s[0]);
        showVolume(s[0]);
        showVoices(Math.max(1, Math.min(s[1], voiceMax)), true);
        reverbCheck.setChecked(s[2] != 0);
        presetSpinner.setSelection(Math.max(0, Math.min(s[3], SynthMidiRoute.REVERB_PRESETS.length - 1)), false);
        reverbSlider.setProgress(s[4]);
        showReverb(s[4]);
        reverbEnabled(s[2] != 0);
        applying = false;
        if (!DlsLoader.note().isEmpty()) {          // e.g. the bank of the last run could not be opened at the start
            soundError.setText(DlsLoader.note());
            DlsLoader.clearNote();
        }
        showBank();
    }

    // ------------------------------------------------------------------------------------------------ DLS sound bank

    private void showBank() {
        String name = SynthMidiRoute.dlsName();
        boolean busy = DlsLoader.isLoading();
        if (busy) soundLabel.setText("Loading the sound bank...");
        else soundLabel.setText(name != null ? "Sound bank: " + name : "Sounds: built-in");
        loadButton.setEnabled(!busy);
        builtInButton.setEnabled(!busy && name != null);
        showRecent();
    }

    /** The "Recently loaded sound banks" list: most recent first; a tap loads the bank. */
    private void showRecent() {
        recentBox.removeAllViews();
        java.util.List<Prefs.Recent> list = AppPrefs.get(this).recent();
        recentTitle.setVisibility(list.isEmpty() ? View.GONE : View.VISIBLE);
        boolean busy = DlsLoader.isLoading();
        for (final Prefs.Recent r : list) {
            final boolean gone = unavailable.contains(r.uri);
            Button row = button(gone ? r.name + " (not available)" : r.name);
            row.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            row.setSingleLine(true);
            row.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
            row.setEnabled(!busy);
            if (gone) row.setTextColor(pal.disabledText);
            row.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { loadRecent(r); }
            });
            recentBox.addView(row, lp(MATCH_PARENT, WRAP_CONTENT, 0, Ui.HALF));
        }
    }

    private static final int MATCH_PARENT = ViewGroup.LayoutParams.MATCH_PARENT, WRAP_CONTENT = ViewGroup.LayoutParams.WRAP_CONTENT;

    /** Checks off the main thread which of the recent banks can still be opened, then shows the list again. */
    private void checkRecent() {
        final java.util.List<Prefs.Recent> list = AppPrefs.get(this).recent();
        if (list.isEmpty()) return;
        new Thread(new Runnable() {
            @Override public void run() {
                final java.util.Set<String> gone = new java.util.HashSet<String>();
                for (Prefs.Recent r : list) if (!DlsLoader.canOpen(SynthSettingsActivity.this, Uri.parse(r.uri))) gone.add(r.uri);
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        unavailable.clear();
                        unavailable.addAll(gone);
                        if (visible) showRecent();
                    }
                });
            }
        }, "dls-recent-check").start();
    }

    private void loadRecent(final Prefs.Recent r) {
        soundError.setText("");
        startLoad(Uri.parse(r.uri), r);
    }

    /** Loads a bank in the background; a recent entry that cannot be opened is taken out of the list with a one-line note. */
    private void startLoad(final Uri uri, final Prefs.Recent fromRecent) {
        if (DlsLoader.isLoading()) return;
        soundError.setText("");
        DlsLoader.load(this, uri, new DlsLoader.Done() {
            @Override public void done(String problem) {
                if (problem == null) {
                    unavailable.remove(uri.toString());
                } else if (fromRecent != null) {
                    DlsLoader.forgetRecent(SynthSettingsActivity.this, fromRecent.uri);
                    unavailable.remove(fromRecent.uri);
                    problem = "\"" + fromRecent.name + "\" could not be opened and was removed from the list. " + problem;
                }
                if (visible || soundError != null) soundError.setText(problem == null ? "" : problem);
                showBank();
            }
        });
        showBank();
    }

    private void pickBank() {
        if (DlsLoader.isLoading()) return;
        // Every file is selectable: no MIME type list (type */* only), because some phones grey out files when a list is given.
        // The grant is asked to be persistable so the bank can be opened again after a restart.
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_DLS);
        } catch (RuntimeException e) {
            soundError.setText("This phone has no file picker to choose a file with.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_DLS || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        DlsLoader.persist(this, uri);
        startLoad(uri, null);
    }
}
