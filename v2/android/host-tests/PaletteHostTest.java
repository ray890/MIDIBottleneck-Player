import org.midibottleneck.player.Palette;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Plain-Java check of the player's palette (no Android classes): WCAG contrast of every text/background pair in light and
 * dark mode (text 4.5:1, controls and field outlines 3:1), and that the theme-level colours in res/values/colors.xml and
 * res/values-night/colors.xml equal the Palette. Usage: PaletteHostTest [path to player/src/main/res]. Exit code 1 on a failure.
 */
public class PaletteHostTest {
    static int failures = 0;

    public static void main(String[] args) throws Exception {
        for (boolean dark : new boolean[] { false, true }) {
            Palette p = Palette.of(dark);
            System.out.println("== " + (dark ? "dark" : "light") + " palette: contrast ratios");
            for (Palette.Pair pair : p.pairs()) {
                double r = pair.ratio();
                boolean ok = r >= pair.min;
                if (!ok) failures++;
                System.out.println(String.format(java.util.Locale.US, "  %-4s %6.2f (needs %.1f)  %s", ok ? "ok" : "FAIL", r, pair.min, pair.name));
            }
        }
        if (args.length > 0) {
            Path res = Paths.get(args[0]);
            compare(res.resolve("values/colors.xml"), Palette.light(), "light");
            compare(res.resolve("values-night/colors.xml"), Palette.dark(), "dark");
        }
        System.out.println(failures == 0 ? "PALETTE HOST TEST: all passed" : "PALETTE HOST TEST: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    static void compare(Path file, Palette p, String mode) throws Exception {
        String xml = new String(Files.readAllBytes(file), StandardCharsets.UTF_8);
        Map<String, Integer> want = new HashMap<String, Integer>();
        want.put("p_background", p.background);
        want.put("p_surface", p.surface);
        want.put("p_bar", p.bar);
        want.put("p_accent", p.accent);
        want.put("p_tonal", p.tonal);
        want.put("p_text_secondary", p.textSecondary);
        Matcher m = Pattern.compile("<color name=\"(\\w+)\">#([0-9A-Fa-f]{8})</color>").matcher(xml);
        Map<String, Integer> got = new HashMap<String, Integer>();
        while (m.find()) got.put(m.group(1), (int) Long.parseLong(m.group(2), 16));
        for (Map.Entry<String, Integer> e : want.entrySet()) {
            Integer g = got.get(e.getKey());
            boolean ok = g != null && g.intValue() == e.getValue().intValue();
            if (!ok) failures++;
            System.out.println("  " + (ok ? "ok  " : "FAIL") + " " + mode + " " + file.getFileName() + " " + e.getKey());
        }
    }
}
