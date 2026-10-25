package org.midibottleneck.player;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.AdapterView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import org.midibottleneck.core.PlayerBridge;

/**
 * Help as a scrollable page. The text is the shared offline Help of the core (same eight topics as the desktop
 * players) in its Android wording (HelpPlatform::Android), shown through its neutral markup (# heading, ## section,
 * - bullet, 1. step, **bold**).
 */
public class HelpActivity extends Activity {
    private TextView content;
    private Palette pal;
    private ScrollView scroll;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        pal = Ui.palette(this);
        final float density = getResources().getDisplayMetrics().density;
        final int pad = Math.round(Ui.S2 * density), half = Math.round(Ui.S1 * density);
        final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(pal.background);
        // The top bar reaches under the status bar (same look as the main screen); the insets become its padding.
        final LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setBackgroundColor(pal.bar);
        bar.setElevation(4 * density);
        TextView title = Ui.text(this, pal, "Help", Ui.TITLE, true, pal.onBar);
        title.setGravity(Gravity.CENTER_VERTICAL);
        title.setMinHeight(Math.round(56 * density));
        title.setPadding(pad, 0, pad, 0);
        bar.addView(title, new LinearLayout.LayoutParams(MATCH, WRAP));
        root.addView(bar, new LinearLayout.LayoutParams(MATCH, WRAP));
        final LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
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
                bar.setPadding(l, t, r, 0);
                page.setPadding(l + pad, pad, r + pad, b + pad);
                return insets;
            }
        });

        int count = PlayerBridge.helpTopicCount();
        String[] titles = new String[count];
        for (int i = 0; i < count; i++) titles[i] = PlayerBridge.helpTitle(i);
        Spinner topics = new Spinner(this);
        Ui.spinner(topics, pal);
        topics.setAdapter(Ui.adapter(this, titles, pal));
        page.addView(topics, new LinearLayout.LayoutParams(MATCH, Math.round(Ui.TOUCH * density)));

        // the text sits in a card, like the sections of the main screen
        scroll = new ScrollView(this);
        scroll.setBackground(Ui.card(this, pal));
        scroll.setClipToOutline(true);
        content = new TextView(this);
        content.setTextSize(Ui.BODY);
        content.setTextColor(pal.text);
        content.setLineSpacing(3 * density, 1f);
        content.setPadding(pad, pad, pad, pad);
        content.setTextIsSelectable(true);
        scroll.addView(content, new ViewGroup.LayoutParams(MATCH, WRAP));
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(MATCH, 0, 1);
        sp.topMargin = half + half;
        page.addView(scroll, sp);
        root.addView(page, new LinearLayout.LayoutParams(MATCH, 0, 1));
        setContentView(root);
        Ui.systemBars(this, pal, pal.background);

        topics.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { show(pos); }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        show(0);
    }

    private void show(int topic) {
        String markup = PlayerBridge.helpText(topic);
        content.setText(render(markup, pal.accentText));
        scroll.scrollTo(0, 0);
    }

    /** Draws the neutral help markup with plain spans. */
    static CharSequence render(String markup, int headingColor) {
        SpannableStringBuilder out = new SpannableStringBuilder();
        for (String raw : markup.split("\n")) {
            String line = raw.endsWith("\r") ? raw.substring(0, raw.length() - 1) : raw;
            if (line.trim().isEmpty()) continue;
            int start = out.length();
            boolean bold = false;
            float size = 1f;
            String body = line;
            if (line.startsWith("## ")) { body = line.substring(3); bold = true; size = 1.15f; }
            else if (line.startsWith("# ")) { body = line.substring(2); bold = true; size = 1.35f; }
            else if (line.startsWith("- ")) { body = "• " + line.substring(2); }
            if (!out.toString().isEmpty() && (size > 1f)) out.append('\n');
            start = out.length();
            appendBold(out, body, bold);
            if (size > 1f) {
                out.setSpan(new RelativeSizeSpan(size), start, out.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                out.setSpan(new ForegroundColorSpan(headingColor), start, out.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
            out.append('\n');
        }
        return out;
    }

    private static void appendBold(SpannableStringBuilder out, String text, boolean allBold) {
        int i = 0;
        boolean bold = allBold;
        while (i < text.length()) {
            int mark = text.indexOf("**", i);
            if (mark < 0 || text.indexOf("**", mark + 2) < 0 && !bold) mark = -1;   // an unclosed ** is literal text
            String piece = mark < 0 ? text.substring(i) : text.substring(i, mark);
            int s = out.length();
            out.append(piece);
            if (bold) out.setSpan(new StyleSpan(Typeface.BOLD), s, out.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            if (mark < 0) break;
            if (!allBold) bold = !bold;
            i = mark + 2;
        }
    }
}
