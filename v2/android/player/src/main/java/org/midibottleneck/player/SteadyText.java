package org.midibottleneck.player;

import android.graphics.Typeface;
import android.view.View;
import android.widget.TextView;

/**
 * Keeps a fast-changing value from moving anything else. Three measures together:
 * (1) every character of the value has the same width (a monospaced typeface, plus the "tnum" tabular-digits font feature
 *     for fonts that have it), so 9% and 10% differ only by the one extra character at the end;
 * (2) the value sits in a cell of fixed width (the statistics cells are weighted columns, never wrap-content), left aligned,
 *     so a longer value grows into the free space of its own cell and never pushes a neighbour;
 * (3) a value that once needed two lines keeps two lines ("high-water mark" of the line count, for the run of the app), so
 *     the height of the cell, and with it everything below, changes at most once per cell and never flickers back and forth.
 */
final class SteadyText {
    private SteadyText() { }

    /** Fixed-width digits and characters for a value that changes often. */
    static void steady(TextView t) {
        t.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        t.setFontFeatureSettings("tnum");
    }

    /** The view never gets fewer lines than it needed so far. */
    static void holdLines(final TextView t) {
        t.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View v, int l, int tp, int r, int b, int ol, int ot, int or, int ob) {
                int lines = t.getLineCount();
                if (lines > t.getMinLines()) t.setMinLines(lines);   // one more layout pass, once
            }
        });
    }
}
