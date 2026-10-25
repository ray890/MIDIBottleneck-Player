package org.midibottleneck.player;

import android.app.Activity;
import android.content.Context;
import android.graphics.Rect;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

/**
 * A number field for touch with the desktop's "drag to scrub, or type" behaviour: a tap without moving opens the
 * keyboard for typing; a sideways drag on the box changes the value (see {@link NumberScrub}) and never opens the keyboard.
 * While typing, Done, the keyboard's Back/close button and a tap anywhere outside the box all apply the typed value and
 * leave the box (focus goes to the screen's home view, the keyboard is hidden). The owner supplies the value to start a
 * drag from and receives each new value; applying the typed text stays with the owner's focus-change listener.
 */
final class NumberBox extends EditText {
    interface Scrub {
        long current();
        void changed(long value);
    }

    private Scrub scrub;
    private long min, max = Long.MAX_VALUE;
    private View home;                 // takes the focus when typing ends
    private float downX, downY, originX;
    private long startValue, lastValue;
    private boolean down, scrubbing;
    private final float unit;          // finger travel per step, in pixels
    private final int slop;

    NumberBox(Context c, Palette p) {
        super(c);
        setInputType(InputType.TYPE_CLASS_NUMBER);
        setImeOptions(EditorInfo.IME_ACTION_DONE);
        setSingleLine(true);
        Ui.edit(this, p);
        unit = 2 * c.getResources().getDisplayMetrics().density;
        slop = ViewConfiguration.get(c).getScaledTouchSlop();
    }

    void setRange(long min, long max) { this.min = min; this.max = max; }

    void setScrub(Scrub scrub) { this.scrub = scrub; }

    /** The view that takes the focus when typing ends (the screen's root, which must be focusable in touch mode). */
    void setHome(View home) { this.home = home; }

    @Override public boolean onTouchEvent(MotionEvent ev) {
        if (!isEnabled() || scrub == null || hasFocus()) return super.onTouchEvent(ev);   // typing: ordinary text handling
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                down = true;
                scrubbing = false;
                downX = ev.getX();
                downY = ev.getY();
                startValue = lastValue = scrub.current();
                return true;
            case MotionEvent.ACTION_MOVE:
                if (!down) return true;
                if (!scrubbing && Math.abs(ev.getX() - downX) > slop && Math.abs(ev.getX() - downX) > Math.abs(ev.getY() - downY)) {
                    scrubbing = true;
                    originX = ev.getX();
                    if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(true);   // sideways drag is ours, not the scroll view's
                }
                if (scrubbing) {
                    long steps = (long) ((ev.getX() - originX) / unit);
                    long v = NumberScrub.map(startValue, steps, min, max);
                    if (v != lastValue) { lastValue = v; scrub.changed(v); }
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (down && !scrubbing) {
                    down = false;
                    startTyping();
                }
                down = false;
                scrubbing = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                down = false;
                scrubbing = false;
                return true;
            default:
                return true;
        }
    }

    private void startTyping() {
        requestFocus();
        selectAll();
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
    }

    /** Ends typing: the owner's focus listener applies the text, the keyboard goes away and the focus moves to the home view. */
    void finishTyping() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (home != null) home.requestFocus(); else clearFocus();   // losing the focus applies the value
        if (imm != null) imm.hideSoftInputFromWindow(getWindowToken(), 0);
    }

    /** The keyboard's Back (close) button: apply and leave. */
    @Override public boolean onKeyPreIme(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP && hasFocus()) {
            finishTyping();
            return true;
        }
        return super.onKeyPreIme(keyCode, event);
    }

    /**
     * Call at the start of an activity's dispatchTouchEvent: a finger going down anywhere outside the box that is being
     * typed in applies its value and leaves it (the touch itself still goes on to the view that was tapped).
     */
    static void outsideTap(Activity a, MotionEvent ev) {
        if (ev.getActionMasked() != MotionEvent.ACTION_DOWN) return;
        View f = a.getCurrentFocus();
        if (!(f instanceof NumberBox)) return;
        Rect r = new Rect();
        f.getGlobalVisibleRect(r);
        if (!r.contains((int) ev.getRawX(), (int) ev.getRawY())) ((NumberBox) f).finishTyping();
    }
}
