package org.midibottleneck.core;

import android.app.Activity;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** The minimal U11 screen shared by both apps: a title and lines of text, kept clear of the system bars. */
public final class InfoScreen {
    private InfoScreen() {}

    public static void show(Activity activity, String title, String... lines) {
        float density = activity.getResources().getDisplayMetrics().density;
        int pad = (int) (16 * density);

        LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.START);

        TextView heading = new TextView(activity);
        heading.setText(title);
        heading.setTextSize(22);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        heading.setPadding(0, 0, 0, pad);
        column.addView(heading);

        for (String line : lines) {
            TextView text = new TextView(activity);
            text.setText(line);
            text.setTextSize(16);
            text.setPadding(0, 0, 0, pad / 2);
            column.addView(text);
        }

        ScrollView scroll = new ScrollView(activity);
        scroll.addView(column);
        scroll.setPadding(pad, pad, pad, pad);
        scroll.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            @SuppressWarnings("deprecation")
            public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                view.setPadding(pad + insets.getSystemWindowInsetLeft(), pad + insets.getSystemWindowInsetTop(),
                        pad + insets.getSystemWindowInsetRight(), pad + insets.getSystemWindowInsetBottom());
                return insets;
            }
        });
        activity.setContentView(scroll);
    }
}
