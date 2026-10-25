package org.midibottleneck.player;

/**
 * The colours of the player in one place, light and dark. Pure Java (no Android classes) so that the host tests can
 * compute the WCAG contrast of every text/background pair ({@link #pairs()}) and compare the theme-level colours with
 * {@code res/values/colors.xml} and {@code res/values-night/colors.xml}. Colours are 0xAARRGGBB ints.
 *
 * Roles: {@code background} the window, {@code surface} cards, menu, transport bar, {@code tile} the statistics cells,
 * {@code tonal} the fill of secondary buttons, {@code accent} primary buttons / checked controls / slider, {@code bar} the top
 * bar (also under the status bar), {@code inputBorder} the outline of fields and spinners (3:1 against the surface).
 */
public final class Palette {
    public final boolean dark;
    public final int background, surface, tile, tonal, tonalPressed;
    public final int text, textSecondary, disabledText, disabledFill;
    public final int accent, accentPressed, onAccent, accentText;
    public final int bar, onBar;
    public final int inputBorder, outline, track, warn;

    private Palette(boolean dark, int background, int surface, int tile, int tonal, int tonalPressed, int text, int textSecondary,
                    int disabledText, int disabledFill, int accent, int accentPressed, int onAccent, int accentText,
                    int bar, int onBar, int inputBorder, int outline, int track, int warn) {
        this.dark = dark;
        this.background = background; this.surface = surface; this.tile = tile; this.tonal = tonal; this.tonalPressed = tonalPressed;
        this.text = text; this.textSecondary = textSecondary; this.disabledText = disabledText; this.disabledFill = disabledFill;
        this.accent = accent; this.accentPressed = accentPressed; this.onAccent = onAccent; this.accentText = accentText;
        this.bar = bar; this.onBar = onBar; this.inputBorder = inputBorder; this.outline = outline; this.track = track; this.warn = warn;
    }

    private static final Palette LIGHT = new Palette(false,
            0xFFEFF2F6, 0xFFFFFFFF, 0xFFF1F4F8, 0xFFDCE6F1, 0xFFC9D8E8,
            0xFF1B1F24, 0xFF4A5560, 0xFF7D8791, 0xFFE9EDF1,
            0xFF1A578F, 0xFF12456F, 0xFFFFFFFF, 0xFF1A578F,
            0xFF1A578F, 0xFFFFFFFF,
            0xFF76828F, 0xFFD5DCE4, 0xFFC5CFDA, 0xFFB3261E);

    private static final Palette DARK = new Palette(true,
            0xFF0F1318, 0xFF1A2029, 0xFF222A35, 0xFF2B3644, 0xFF38475A,
            0xFFE7ECF2, 0xFFA9B4C0, 0xFF6B7580, 0xFF20262E,
            0xFF8DB8E6, 0xFFB3D1F0, 0xFF0A1B2E, 0xFF8DB8E6,
            0xFF1C2A3A, 0xFFE7ECF2,
            0xFF7F8B98, 0xFF2C3541, 0xFF3A4656, 0xFFFF8B80);

    public static Palette light() { return LIGHT; }
    public static Palette dark() { return DARK; }
    public static Palette of(boolean dark) { return dark ? DARK : LIGHT; }

    /** One text/background (or control/background) combination that must keep a minimum contrast ratio. */
    public static final class Pair {
        public final String name;
        public final int fg, bg;
        public final double min;
        Pair(String name, int fg, int bg, double min) { this.name = name; this.fg = fg; this.bg = bg; this.min = min; }
        public double ratio() { return contrast(fg, bg); }
    }

    /** Text pairs need 4.5:1 (WCAG AA); controls and field outlines need 3:1 (WCAG 1.4.11). Disabled text is exempt. */
    public Pair[] pairs() {
        return new Pair[] {
            new Pair("text on background", text, background, 4.5),
            new Pair("text on surface", text, surface, 4.5),
            new Pair("text on tile", text, tile, 4.5),
            new Pair("secondary text on background", textSecondary, background, 4.5),
            new Pair("secondary text on surface", textSecondary, surface, 4.5),
            new Pair("secondary text on tile", textSecondary, tile, 4.5),
            new Pair("heading (accent text) on surface", accentText, surface, 4.5),
            new Pair("heading (accent text) on background", accentText, background, 4.5),
            new Pair("primary button text on accent", onAccent, accent, 4.5),
            new Pair("primary button text on accent, pressed", onAccent, accentPressed, 4.5),
            new Pair("secondary button text on tonal", accentText, tonal, 4.5),
            new Pair("secondary button text on tonal, pressed", accentText, tonalPressed, 4.5),
            new Pair("text on tonal (theme button text)", text, tonal, 4.5),
            new Pair("top bar text on bar", onBar, bar, 4.5),
            new Pair("warning text on surface", warn, surface, 4.5),
            new Pair("field outline on surface", inputBorder, surface, 3.0),
            new Pair("accent control on surface", accent, surface, 3.0),
            new Pair("secondary text (unchecked box) on surface", textSecondary, surface, 3.0),
        };
    }

    // ---- WCAG 2.x contrast
    private static double channel(int c) {
        double s = c / 255.0;
        return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
    }

    public static double luminance(int argb) {
        return 0.2126 * channel((argb >> 16) & 0xFF) + 0.7152 * channel((argb >> 8) & 0xFF) + 0.0722 * channel(argb & 0xFF);
    }

    public static double contrast(int a, int b) {
        double la = luminance(a), lb = luminance(b);
        double hi = Math.max(la, lb), lo = Math.min(la, lb);
        return (hi + 0.05) / (lo + 0.05);
    }
}
