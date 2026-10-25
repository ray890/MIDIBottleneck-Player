package org.midibottleneck.player;

/**
 * The touch curve of the long sliders (processing-model slider on the main screen, voice limit on the Synth settings page).
 * A finger moves a slider's thumb linearly along the track; the value that belongs to the position follows an exponential
 * curve instead of a straight line:
 *
 *     value = valueMax * (e^(k*x) - 1) / (e^k - 1),     x = position / positionMax, 0..1
 *
 * so the first part of the track covers few values (fine control at low values) and the curve still ends exactly at the
 * maximum. With k = 2.5 (processing) the bottom of the track is 4.5 times finer than a straight slider and the top is 2.7
 * times coarser; with k = 4 (voices) the bottom is 13 times finer. k = 0 would be a straight line (not used). The inverse
 * puts the thumb where a given value belongs.
 *
 * The processing-model slider already ran on the player core's own scale (v1's: linear up to 5000 us, then logarithmic);
 * this curve sits in front of it on Android only: the core still receives its own slider position (0..10000), so what the
 * value means is unchanged and the typed value, DIN preset and desktop behaviour are not touched.
 */
final class SliderCurve {
    private SliderCurve() { }

    static final double K_PROCESSING = 2.5, K_VOICES = 4.0;

    /** Thumb position (0..positionMax) to value (0..valueMax). */
    static int toValue(int position, int positionMax, int valueMax, double k) {
        if (positionMax <= 0 || valueMax <= 0) return 0;
        double x = Math.max(0, Math.min(1, position / (double) positionMax));
        double y = (Math.exp(k * x) - 1) / (Math.exp(k) - 1);
        return (int) Math.max(0, Math.min(valueMax, Math.round(valueMax * y)));
    }

    /** Value (0..valueMax) to thumb position (0..positionMax); the inverse of {@link #toValue}. */
    static int toPosition(int value, int positionMax, int valueMax, double k) {
        if (positionMax <= 0 || valueMax <= 0) return 0;
        double y = Math.max(0, Math.min(1, value / (double) valueMax));
        double x = Math.log(1 + y * (Math.exp(k) - 1)) / k;
        return (int) Math.max(0, Math.min(positionMax, Math.round(positionMax * x)));
    }
}
