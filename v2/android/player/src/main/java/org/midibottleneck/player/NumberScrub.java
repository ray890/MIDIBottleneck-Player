package org.midibottleneck.player;

/**
 * The value a number box takes while its owner drags sideways on it (touch version of the desktop's scrub-or-type field).
 * One step is a small unit of finger travel. A step changes the value by 2 % of its size, but at least 1, so small values
 * move one at a time and large values move faster; the longer the drag already is, the larger the steps (every 60 steps of
 * travel add one more times the base step), which is the acceleration. The result depends only on the start value and the
 * total travel from where the finger went down, so dragging back to the start gives the start value again.
 */
final class NumberScrub {
    private NumberScrub() { }

    /** The value after {@code steps} units of travel (negative = left), kept between min and max. */
    static long map(long start, long steps, long min, long max) {
        long v = Math.max(min, Math.min(max, start));
        if (steps == 0) return v;
        int dir = steps > 0 ? 1 : -1;
        long count = Math.min(Math.abs(steps), 6000);
        for (long i = 0; i < count; i++) {
            double growth = Math.abs((double) v) * 0.02 * (1.0 + i / 60.0);
            long step = Math.max(1, Math.round(growth));
            v += dir * step;
            if (v <= min) return min;
            if (v >= max) return max;
        }
        return v;
    }
}
