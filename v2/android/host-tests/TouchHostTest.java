package org.midibottleneck.player;

/**
 * Plain-Java test of the touch helpers: the slider curve (SliderCurve) and the drag-to-change rule of the number boxes
 * (NumberScrub). Run: java -cp OUT org.midibottleneck.player.TouchHostTest
 */
public final class TouchHostTest {
    private static int failures;

    private static void check(String what, boolean ok) {
        if (!ok) { failures++; System.out.println("FAIL: " + what); }
    }

    public static void main(String[] args) {
        // ---- slider curve: ends, order, fine at the bottom, inverse
        double[] ks = { SliderCurve.K_PROCESSING, SliderCurve.K_VOICES };
        int[] maxima = { 10000, 1023 };
        for (int c = 0; c < ks.length; c++) {
            double k = ks[c];
            int vmax = maxima[c];
            check("k=" + k + " bottom", SliderCurve.toValue(0, 10000, vmax, k) == 0);
            check("k=" + k + " top reaches the maximum", SliderCurve.toValue(10000, 10000, vmax, k) == vmax);
            int prev = -1;
            boolean monotonic = true;
            for (int p = 0; p <= 10000; p++) {
                int v = SliderCurve.toValue(p, 10000, vmax, k);
                if (v < prev) monotonic = false;
                prev = v;
            }
            check("k=" + k + " never goes down", monotonic);
            check("k=" + k + " finer than a straight slider at 10 %", SliderCurve.toValue(1000, 10000, vmax, k) < vmax / 10);
            check("k=" + k + " inverse at the ends", SliderCurve.toPosition(0, 10000, vmax, k) == 0
                    && SliderCurve.toPosition(vmax, 10000, vmax, k) == 10000);
            int worst = 0;
            for (int v = 0; v <= vmax; v++) {
                int back = SliderCurve.toValue(SliderCurve.toPosition(v, 10000, vmax, k), 10000, vmax, k);
                worst = Math.max(worst, Math.abs(back - v));
            }
            // where one position is worth more than one value (the coarse top of the 10000 scale) the round trip is off by
            // less than a position's worth of values; where it is finer (voices, the bottom) it is exact.
            int allowed = (int) Math.ceil(vmax * k * Math.exp(k) / (Math.exp(k) - 1) / 10000.0);
            check("k=" + k + " round trip within " + allowed + " (worst " + worst + ")", worst <= allowed);
        }
        check("150 voices region: first 10 % of the track stays under 60 voices", 1 + SliderCurve.toValue(1000, 10000, 1023, SliderCurve.K_VOICES) < 60);
        check("degenerate range", SliderCurve.toValue(5000, 10000, 0, 4) == 0 && SliderCurve.toPosition(3, 10000, 0, 4) == 0);

        // ---- number drag
        check("no travel keeps the value", NumberScrub.map(1234, 0, 0, 1000000) == 1234);
        check("small value moves by one", NumberScrub.map(10, 1, 0, 1000000) == 11 && NumberScrub.map(10, -1, 0, 1000000) == 9);
        check("zero can be left", NumberScrub.map(0, 3, 0, 1000000) == 3);
        check("lower end holds", NumberScrub.map(5, -100, 1, 1000000) == 1);
        check("upper end holds", NumberScrub.map(900000, 5000, 0, 1000000) == 1000000);
        check("start outside the range is pulled in", NumberScrub.map(5000000, 0, 0, 1000000) == 1000000);
        long prev = Long.MIN_VALUE;
        boolean monotonic = true;
        for (int s = -400; s <= 400; s++) {
            long v = NumberScrub.map(5000, s, 0, 1000000);
            if (v < prev) monotonic = false;
            prev = v;
        }
        check("more travel never gives a smaller value", monotonic);
        long slow = NumberScrub.map(100000, 30, 0, 100000000L) - 100000;
        long fast = NumberScrub.map(100000, 240, 0, 100000000L);
        check("larger values move faster and long drags accelerate", slow > 30 && fast > 100000L * 4);
        check("the same travel always gives the same value", NumberScrub.map(777, 55, 0, 99999) == NumberScrub.map(777, 55, 0, 99999));
        check("a very long drag ends in bounded time", NumberScrub.map(1, Long.MAX_VALUE / 2, 0, 1000000) == 1000000);

        if (failures == 0) System.out.println("TouchHostTest: PASS");
        else { System.out.println("TouchHostTest: " + failures + " FAILED"); System.exit(1); }
    }
}
