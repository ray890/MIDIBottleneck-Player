package org.midibottleneck.player;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/**
 * A card whose heading can be tapped to collapse and expand the group: a header row (the heading text and a chevron drawn
 * in code, pointing down when open and right when collapsed) over the content. Views added with addView go into the
 * content, so a card is filled like any other LinearLayout. The collapsed state is kept for the run of the app only (a
 * static map by id), never written to the preferences.
 */
final class Section extends LinearLayout {
    private static final Map<String, Boolean> COLLAPSED = new HashMap<String, Boolean>();

    private final String id;
    private final LinearLayout header, content;
    private final Chevron chevron;
    private TextView heading;
    private boolean collapsed;

    Section(Context c, Palette p, String id) {
        super(c);
        this.id = id;
        setOrientation(VERTICAL);
        setBackground(Ui.card(c, p));
        setPadding(Ui.dp(c, Ui.S2), Ui.dp(c, Ui.S1), Ui.dp(c, Ui.S2), Ui.dp(c, Ui.S2));
        header = new LinearLayout(c);
        header.setOrientation(HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setMinimumHeight(Ui.dp(c, Ui.TOUCH));
        header.setClickable(true);
        header.setFocusable(true);
        android.content.res.TypedArray a = c.obtainStyledAttributes(new int[] { android.R.attr.selectableItemBackground });
        try {
            android.graphics.drawable.Drawable d = a.getDrawable(0);
            if (d != null) header.setBackground(d);
        } finally {
            a.recycle();
        }
        chevron = new Chevron(c, p.accentText);
        content = new LinearLayout(c);
        content.setOrientation(VERTICAL);
        super.addView(header, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        super.addView(content, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        header.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { setCollapsed(!collapsed); }
        });
        Boolean was = COLLAPSED.get(id);
        setCollapsed(was != null && was);
    }

    /** True when a section with this id has been shown (and so has a state) in this run of the app. */
    static boolean known(String id) { return COLLAPSED.containsKey(id); }

    /** Sets the heading (its text may change later, e.g. "Volume: 100 %"). */
    void setHeading(TextView h) {
        if (heading != null) header.removeView(heading);
        heading = h;
        h.setPadding(0, 0, 0, 0);
        header.addView(h, 0, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        if (chevron.getParent() == null) {
            header.addView(chevron, new LinearLayout.LayoutParams(Ui.dp(getContext(), 32), Ui.dp(getContext(), 32)));
        }
        describe();
    }

    boolean isCollapsed() { return collapsed; }

    /** A long press on the heading row (used for a hidden debug switch). */
    void setOnHeadingLongClick(final Runnable action) {
        header.setOnLongClickListener(new View.OnLongClickListener() {
            @Override public boolean onLongClick(View v) { action.run(); return true; }
        });
    }

    void setCollapsed(boolean value) {
        collapsed = value;
        COLLAPSED.put(id, value);
        content.setVisibility(value ? GONE : VISIBLE);
        chevron.setOpen(!value);
        describe();
    }

    private void describe() {
        CharSequence text = heading != null ? heading.getText() : id;
        header.setContentDescription((collapsed ? "Expand " : "Collapse ") + text);
    }

    @Override public void addView(View child, int index, ViewGroup.LayoutParams params) {
        if (content != null && child != header && child != content) content.addView(child, index, params);
        else super.addView(child, index, params);
    }

    @Override public void addView(View child, ViewGroup.LayoutParams params) { addView(child, -1, params); }

    @Override public void addView(View child) { addView(child, -1, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)); }

    /** Down-pointing chevron when the group is open, right-pointing when collapsed. */
    private static final class Chevron extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float density;
        private boolean open = true;

        Chevron(Context c, int color) {
            super(c);
            density = c.getResources().getDisplayMetrics().density;
            paint.setColor(color);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2.5f * density);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        }

        void setOpen(boolean value) { open = value; invalidate(); }

        @Override protected void onDraw(Canvas canvas) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f, w = 6 * density, h = 3.5f * density;
            Path path = new Path();
            if (open) {
                path.moveTo(cx - w, cy - h);
                path.lineTo(cx, cy + h);
                path.lineTo(cx + w, cy - h);
            } else {
                path.moveTo(cx - h, cy - w);
                path.lineTo(cx + h, cy);
                path.lineTo(cx - h, cy + w);
            }
            canvas.drawPath(path, paint);
        }
    }
}
