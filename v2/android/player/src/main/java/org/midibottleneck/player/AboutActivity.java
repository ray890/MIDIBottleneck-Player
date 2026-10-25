package org.midibottleneck.player;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * "About and licences": the player's own licence notice and text (GPL-3.0-or-later), then every library that is inside
 * the app with its licence: Sonivox (Apache 2.0, its NOTICE and licence text come from the asset sonivox-NOTICE.txt of the
 * core module) and the C++ runtime of the Android NDK that is linked into the native code. The GPL text is the repository's
 * LICENSE file, copied into res/raw. Each group collapses with a tap on its heading.
 */
public class AboutActivity extends Activity {
    private static final int MATCH = ViewGroup.LayoutParams.MATCH_PARENT, WRAP = ViewGroup.LayoutParams.WRAP_CONTENT;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Palette pal = Ui.palette(this);
        final float density = getResources().getDisplayMetrics().density;
        final int pad = Ui.dp(this, Ui.S2);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(pal.background);

        final LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(pal.bar);
        bar.setElevation(4 * density);
        bar.setMinimumHeight(Ui.dp(this, 56));
        Button back = new Button(this);
        back.setText("Back");
        back.setAllCaps(false);
        back.setTextSize(Ui.BODY);
        back.setTextColor(pal.onBar);
        back.setStateListAnimator(null);
        back.setBackground(null);
        back.setMinWidth(Ui.dp(this, Ui.TOUCH));
        back.setMinHeight(Ui.dp(this, Ui.TOUCH));
        back.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });
        bar.addView(back, new LinearLayout.LayoutParams(WRAP, WRAP));
        TextView title = Ui.text(this, pal, "About and licences", Ui.TITLE, true, pal.onBar);
        bar.addView(title, new LinearLayout.LayoutParams(0, WRAP, 1));
        root.addView(bar, new LinearLayout.LayoutParams(MATCH, WRAP));

        final ScrollView scroll = new ScrollView(this);
        final LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(body, new ViewGroup.LayoutParams(MATCH, WRAP));
        root.addView(scroll, new LinearLayout.LayoutParams(MATCH, 0, 1));

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
                bar.setPadding(l + Ui.dp(AboutActivity.this, Ui.HALF), t, r, 0);
                body.setPadding(l + pad, pad, r + pad, b + pad);
                return insets;
            }
        });

        String version = "";
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            if (info.versionName != null) version = " " + info.versionName;
        } catch (Exception ignored) { }

        addGroup(body, pal, "about.player", "MIDIBottleneck Player" + version + " (Android)",
                "MIDIBottleneck Player" + version + "\n"
                + "Copyright © 2026 Ray890\n\n"
                + "This program is free software: you can redistribute it and/or modify it under the terms of the GNU General "
                + "Public License as published by the Free Software Foundation, either version 3 of the License, or (at your "
                + "option) any later version.\n\n"
                + "This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the "
                + "implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License "
                + "for more details.\n\n"
                + "The source code is at https://github.com/ray890/MIDIBottleneck-Player\n\n"
                + "The full licence text is in the next group.");
        addGroup(body, pal, "about.gpl", "GNU General Public License, version 3", readRaw(R.raw.license_gpl3), true);
        addGroup(body, pal, "about.libs", "What is inside the app",
                "Libraries that are part of this app:\n\n"
                + "1. Sonivox EAS, the software synthesizer of the Android Open Source Project, with its built-in sound set. "
                + "Licence: Apache License 2.0. Copyright (c) 2004-2006 Sonic Network Inc. It makes the \"Built-in synth (Sonivox)\" "
                + "output. Its notice and the licence text are in the next group.\n\n"
                + "2. libc++, the C++ standard library of the Android NDK (LLVM project), linked into the app's native code. "
                + "Licence: Apache License 2.0 with LLVM Exceptions (the Apache 2.0 text is printed in the last group; the "
                + "LLVM Exceptions relax the notice requirements for the compiled form). "
                + "Source: https://libcxx.llvm.org\n\n"
                + "Not part of the app: the Android system libraries it calls (libc, libm, liblog, the native MIDI library), which "
                + "belong to Android itself. The app uses no other third-party library, no Android support library, and no sound "
                + "bank of its own other than Sonivox's: a DLS sound bank you load is your own file and is not copied anywhere.");
        addGroup(body, pal, "about.sonivox", "Sonivox EAS: notice and Apache License 2.0", readAsset("sonivox-NOTICE.txt"), true);
        Ui.systemBars(this, pal, pal.background);
        setContentView(root);
    }

    private void addGroup(LinearLayout body, Palette pal, String id, String heading, String text) {
        addGroup(body, pal, id, heading, text, false);
    }

    private void addGroup(LinearLayout body, Palette pal, String id, String heading, String text, boolean startCollapsedFirstTime) {
        boolean firstTime = !Section.known(id);
        Section s = new Section(this, pal, id);
        TextView h = Ui.text(this, pal, heading, Ui.HEADING, true, pal.accentText);
        s.setHeading(h);
        TextView t = Ui.text(this, pal, text, Ui.DETAIL, false, pal.text);
        t.setTextIsSelectable(true);
        t.setLineSpacing(2 * getResources().getDisplayMetrics().density, 1f);
        s.addView(t, new LinearLayout.LayoutParams(MATCH, WRAP));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(MATCH, WRAP);
        p.topMargin = body.getChildCount() == 0 ? 0 : Ui.dp(this, Ui.S2);
        body.addView(s, p);
        if (startCollapsedFirstTime && firstTime) s.setCollapsed(true);   // the long texts start closed
    }

    private String readRaw(int id) {
        try {
            return read(getResources().openRawResource(id));
        } catch (RuntimeException e) {
            return "The licence text could not be read.";
        }
    }

    private String readAsset(String name) {
        try {
            return read(getAssets().open(name));
        } catch (IOException e) {
            return "The text could not be read.";
        }
    }

    private static String read(InputStream in) {
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            return new String(out.toByteArray(), "UTF-8");
        } catch (IOException e) {
            return "The text could not be read.";
        } finally {
            try { in.close(); } catch (IOException ignored) { }
        }
    }
}
