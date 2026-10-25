package org.midibottleneck.player;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.view.View;
import android.view.Window;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

/**
 * The player's design system in code, next to {@link Palette}: spacing on an 8 dp grid (4 dp half step), corner radii, text
 * sizes, and helpers that dress plain framework views (no AndroidX, no Material library) with the palette, including their
 * pressed, focused and disabled states. Used by MainActivity, HelpActivity and SynthSettingsActivity; the app-wide theme in res/values/styles.xml
 * carries the same colours for every other screen.
 */
final class Ui {
    private Ui() { }

    // ---- spacing (dp)
    static final int HALF = 4, S1 = 8, S2 = 16, S3 = 24;
    // ---- shapes (dp)
    static final int CARD_RADIUS = 12, CONTROL_RADIUS = 8;
    static final int TOUCH = 48;                 // minimum touch target
    // ---- text (sp)
    static final float TITLE = 20, HEADING = 16, BODY = 16, LABEL = 14, VALUE = 15, DETAIL = 13, CAPTION = 12;

    static int dp(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    /** The palette for the system's current light or dark setting. */
    static Palette palette(Context c) {
        int night = c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return Palette.of(night == Configuration.UI_MODE_NIGHT_YES);
    }

    static ColorStateList colors(int normal, int disabled) {
        return new ColorStateList(new int[][] { { -android.R.attr.state_enabled }, {} }, new int[] { disabled, normal });
    }

    static GradientDrawable box(Context c, int fill, int stroke, float strokeDp, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(c, radiusDp));
        if (strokeDp > 0) g.setStroke(Math.max(1, dp(c, strokeDp)), stroke);
        return g;
    }

    /** Section card: surface colour, hairline outline, large corners. */
    static Drawable card(Context c, Palette p) { return box(c, p.surface, p.outline, 1, CARD_RADIUS); }

    /** Cell of the statistics: a quiet tile inside a card. */
    static Drawable tile(Context c, Palette p) { return box(c, p.tile, p.tile, 0, CONTROL_RADIUS); }

    // ---- text
    static TextView text(Context c, Palette p, String s, float sp, boolean bold, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    // ---- buttons
    /** Primary: filled with the accent colour. Secondary (primary == false): tonal fill with accent-coloured text. */
    static void button(Button b, Palette p, boolean primary) {
        Context c = b.getContext();
        int fill = primary ? p.accent : p.tonal, pressed = primary ? p.accentPressed : p.tonalPressed;
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] { -android.R.attr.state_enabled }, box(c, p.disabledFill, p.outline, 1, CONTROL_RADIUS));
        s.addState(new int[] { android.R.attr.state_pressed }, box(c, pressed, pressed, 0, CONTROL_RADIUS));
        s.addState(new int[] { android.R.attr.state_focused }, box(c, fill, primary ? p.onAccent : p.accentText, 2, CONTROL_RADIUS));
        s.addState(new int[] {}, box(c, fill, fill, 0, CONTROL_RADIUS));
        b.setBackground(s);
        b.setTextColor(colors(primary ? p.onAccent : p.accentText, p.disabledText));
        b.setAllCaps(false);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setStateListAnimator(null);
        b.setElevation(0);
        b.setMinHeight(dp(c, TOUCH));
        b.setMinimumHeight(dp(c, TOUCH));
        b.setMinWidth(dp(c, TOUCH));
        b.setMinimumWidth(dp(c, TOUCH));
        b.setPadding(dp(c, S2), 0, dp(c, S2), 0);
    }

    // ---- fields
    static void edit(EditText e, Palette p) {
        Context c = e.getContext();
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] { -android.R.attr.state_enabled }, box(c, p.disabledFill, p.outline, 1, CONTROL_RADIUS));
        s.addState(new int[] { android.R.attr.state_focused }, box(c, p.surface, p.accent, 2, CONTROL_RADIUS));
        s.addState(new int[] {}, box(c, p.surface, p.inputBorder, 1, CONTROL_RADIUS));
        e.setBackground(s);
        e.setTextColor(colors(p.text, p.disabledText));
        e.setHighlightColor(0x663D7FBF);
        e.setMinHeight(dp(c, TOUCH));
        e.setMinimumHeight(dp(c, TOUCH));
        e.setPadding(dp(c, 12), dp(c, S1), dp(c, 12), dp(c, S1));
    }

    static void checkBox(CheckBox cb, Palette p) {
        cb.setButtonTintList(new ColorStateList(
                new int[][] { { -android.R.attr.state_enabled }, { android.R.attr.state_checked }, {} },
                new int[] { p.disabledText, p.accent, p.textSecondary }));
        cb.setTextColor(colors(p.text, p.disabledText));
        cb.setMinHeight(dp(cb.getContext(), TOUCH));
        cb.setMinimumHeight(dp(cb.getContext(), TOUCH));
    }

    static void seekBar(SeekBar s, Palette p) {
        s.setProgressTintList(colors(p.accent, p.disabledText));
        s.setProgressBackgroundTintList(ColorStateList.valueOf(p.track));
        s.setThumbTintList(colors(p.accent, p.disabledText));
    }

    static void progress(ProgressBar bar, Palette p) {
        bar.setProgressTintList(ColorStateList.valueOf(p.accent));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(p.track));
    }

    /** Spinner as a rounded field with a chevron (the framework's own arrow background is replaced). */
    static void spinner(Spinner sp, Palette p) {
        Context c = sp.getContext();
        sp.setBackground(new SpinnerBackground(c, p));
        sp.setPadding(dp(c, 12), 0, dp(c, 36), 0);
        sp.setMinimumHeight(dp(c, TOUCH));
    }

    /** Items of a spinner in the palette's text colour (the drop-down list keeps the theme's own popup colours). */
    static ArrayAdapter<String> adapter(final Context c, String[] items, final Palette p) {
        return new ArrayAdapter<String>(c, android.R.layout.simple_spinner_item, items) {
            @Override public View getView(int position, View convertView, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(p.text);
                t.setTextSize(BODY);
                t.setPadding(0, 0, 0, 0);
                t.setSingleLine(true);
                t.setEllipsize(android.text.TextUtils.TruncateAt.END);
                return t;
            }
            @Override public View getDropDownView(int position, View convertView, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getDropDownView(position, convertView, parent);
                t.setTextSize(BODY);
                t.setMinHeight(dp(c, TOUCH));
                t.setGravity(android.view.Gravity.CENTER_VERTICAL);
                return t;
            }
        };
    }

    // ---- system bars
    /** Status and navigation bar colours (before Android 15; later versions draw the app behind them) and icon contrast. */
    static void systemBars(Activity a, Palette p, int navColor) {
        try {
            Window w = a.getWindow();
            w.setStatusBarColor(p.bar);
            w.setNavigationBarColor(navColor);
            boolean navLight = Palette.luminance(navColor) > 0.4;
            if (Build.VERSION.SDK_INT >= 30) {
                android.view.WindowInsetsController ic = w.getInsetsController();
                if (ic != null) {
                    ic.setSystemBarsAppearance(0, android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS);   // bar is dark
                    ic.setSystemBarsAppearance(navLight ? android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS : 0,
                            android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
                }
            } else {
                View d = w.getDecorView();
                int f = d.getSystemUiVisibility() & ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
                if (Build.VERSION.SDK_INT >= 26) f = navLight ? (f | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR) : (f & ~View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
                d.setSystemUiVisibility(f);
            }
        } catch (RuntimeException ignored) { }
    }

    /** Rounded outlined field background with a chevron on the right; follows the enabled state. */
    private static final class SpinnerBackground extends Drawable {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                chevron = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float radius, stroke, density;
        private final Palette p;
        private boolean enabled = true, pressed;

        SpinnerBackground(Context c, Palette p) {
            this.p = p;
            density = c.getResources().getDisplayMetrics().density;
            radius = CONTROL_RADIUS * density;
            stroke = Math.max(1f, density);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(stroke);
            chevron.setStyle(Paint.Style.STROKE);
            chevron.setStrokeWidth(2 * density);
            chevron.setStrokeCap(Paint.Cap.ROUND);
            chevron.setStrokeJoin(Paint.Join.ROUND);
        }

        @Override public boolean isStateful() { return true; }

        @Override protected boolean onStateChange(int[] state) {
            boolean en = false, pr = false;
            for (int s : state) {
                if (s == android.R.attr.state_enabled) en = true;
                if (s == android.R.attr.state_pressed) pr = true;
            }
            boolean changed = en != enabled || pr != pressed;
            enabled = en;
            pressed = pr;
            if (changed) invalidateSelf();
            return changed;
        }

        @Override public void draw(Canvas canvas) {
            Rect r = getBounds();
            RectF box = new RectF(r.left + stroke / 2, r.top + stroke / 2, r.right - stroke / 2, r.bottom - stroke / 2);
            fill.setColor(!enabled ? p.disabledFill : pressed ? p.tile : p.surface);
            canvas.drawRoundRect(box, radius, radius, fill);
            line.setColor(enabled ? p.inputBorder : p.outline);
            canvas.drawRoundRect(box, radius, radius, line);
            chevron.setColor(enabled ? p.textSecondary : p.disabledText);
            float cx = r.right - 20 * density, cy = r.centerY(), w = 5 * density, h = 2.5f * density;
            Path path = new Path();
            path.moveTo(cx - w, cy - h);
            path.lineTo(cx, cy + h);
            path.lineTo(cx + w, cy - h);
            canvas.drawPath(path, chevron);
        }

        @Override public void setAlpha(int alpha) { }
        @Override public void setColorFilter(ColorFilter cf) { }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }
}
