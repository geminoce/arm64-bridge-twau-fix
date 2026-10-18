// SPDX-License-Identifier: GPL-3.0-or-later
package com.telltalegames.fables100;

import android.app.Activity;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

/** Window refresh preference and actual display readback for the bridge render pacer. */
public final class DisplayCadence implements DisplayManager.DisplayListener {
    private static final String TAG = "RestorationDisplay";
    private static volatile DisplayCadence current;
    private static volatile int selectedCap = 60;
    private final Activity activity;
    private final DisplayManager manager;
    private int lastDisplay = -1;
    private int lastMode = -1;
    private int lastRequest = -1;
    private int lastCap = -1;
    private float lastHz = Float.NaN;
    private float lastDesiredHz = Float.NaN;
    private boolean warnedNativeUnavailable;

    private DisplayCadence(Activity activity) {
        this.activity = activity;
        this.manager = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
    }

    // Named JNI export: Java_com_telltalegames_fables100_DisplayCadence_nativeSetFrameLimit.
    // Profile 0 means dynamic display cadence. Native code must validate both arguments.
    private static native void nativeSetFrameLimit(int cap, float actualHz);

    public static void setFrameLimit(String profile) {
        selectedCap = parseProfile(profile);
        DisplayCadence active = current;
        if (active != null) active.requestRefresh("limit");
    }

    /** Only these four complete internal URLs bypass the original VIEW-intent path. */
    public static boolean handleFrameLimitUrl(String url) {
        String profile;
        if ("twau-restoration://fps-limit/30".equals(url)) profile = "30";
        else if ("twau-restoration://fps-limit/45".equals(url)) profile = "45";
        else if ("twau-restoration://fps-limit/60".equals(url)) profile = "60";
        else if ("twau-restoration://fps-limit/display".equals(url)) profile = "display";
        else return false;
        setFrameLimit(profile);
        return true;
    }

    public static void attach(Activity activity) {
        if (current != null) current.close();
        current = null;
        DisplayCadence next = new DisplayCadence(activity);
        try {
            if (next.manager == null) throw new IllegalStateException("DisplayManager unavailable");
            next.manager.registerDisplayListener(next, new Handler(Looper.getMainLooper()));
            current = next;
            next.refresh("attach");
        } catch (RuntimeException failure) {
            next.close();
            current = null;
            Log.w(TAG, "Display preference unavailable", failure);
        }
    }

    public static void resume(Activity activity) {
        DisplayCadence active = current;
        if (active != null && active.activity == activity) active.requestRefresh("resume");
    }

    public static void detach(Activity activity) {
        if (current != null && current.activity == activity) {
            current.close();
            current = null;
        }
    }

    private void requestRefresh(final String reason) {
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (current != DisplayCadence.this) return;
                try { refresh(reason); }
                catch (RuntimeException failure) { Log.w(TAG, "Display readback failed", failure); }
            }
        });
    }

    private void close() {
        if (manager != null) {
            try { manager.unregisterDisplayListener(this); }
            catch (RuntimeException failure) { Log.w(TAG, "Display listener cleanup", failure); }
        }
    }

    private Display display() {
        Display attached = activity.getWindow().getDecorView().getDisplay();
        return attached != null ? attached : activity.getWindowManager().getDefaultDisplay();
    }

    private void refresh(String reason) {
        Display display = display();
        int cap = selectedCap;
        if (display == null || !display.isValid()) {
            try {
                nativeSetFrameLimit(cap, Float.NaN);
                warnedNativeUnavailable = false;
            } catch (UnsatisfiedLinkError unavailable) {
                if (!warnedNativeUnavailable) {
                    Log.w(TAG, "Frame limiter unavailable; retrying on display lifecycle", unavailable);
                    warnedNativeUnavailable = true;
                }
            }
            Log.w(TAG, "Display unavailable: cap=" + cap + " reported_hz=NaN effective_target="
                    + (cap == 0 ? 60 : Math.min(cap, 60)));
            return;
        }
        Display.Mode active = display.getMode();
        Display.Mode[] modes = display.getSupportedModes();
        int[] ids = new int[modes.length];
        int[] widths = new int[modes.length];
        int[] heights = new int[modes.length];
        float[] rates = new float[modes.length];
        for (int i = 0; i < modes.length; i++) {
            ids[i] = modes[i].getModeId();
            widths[i] = modes[i].getPhysicalWidth();
            heights[i] = modes[i].getPhysicalHeight();
            rates[i] = modes[i].getRefreshRate();
        }
        int best = choose(cap, active.getModeId(), active.getPhysicalWidth(),
                active.getPhysicalHeight(), ids, widths, heights, rates);
        int requested = best >= 0 ? ids[best] : 0;
        float desiredHz = best >= 0 ? rates[best] : active.getRefreshRate();
        WindowManager.LayoutParams attributes = activity.getWindow().getAttributes();
        if (requested != 0 && attributes.preferredDisplayModeId != requested) {
            attributes.preferredDisplayModeId = requested;
            try { activity.getWindow().setAttributes(attributes); }
            catch (RuntimeException failure) {
                requested = 0;
                Log.w(TAG, "Mode request failed; trying refresh-rate preference", failure);
            }
        }
        if (requested == 0 && validRate(desiredHz)
                && (attributes.preferredDisplayModeId != 0
                    || attributes.preferredRefreshRate != desiredHz)) {
            attributes.preferredDisplayModeId = 0;
            attributes.preferredRefreshRate = desiredHz;
            try { activity.getWindow().setAttributes(attributes); }
            catch (RuntimeException failure) { Log.w(TAG, "Refresh-rate preference failed", failure); }
        }
        // A preference is a hint. Publish readback even while the requested switch is pending.
        // This is the logical-display API value, not proof of physical panel presentations.
        active = display.getMode();
        float reportedHz = display.getRefreshRate();
        if (!validRate(reportedHz)) Log.w(TAG, "Invalid display refresh readback: " + reportedHz);
        try {
            nativeSetFrameLimit(cap, reportedHz);
            warnedNativeUnavailable = false;
        } catch (UnsatisfiedLinkError unavailable) {
            if (!warnedNativeUnavailable) {
                Log.w(TAG, "Frame limiter unavailable; retrying on display lifecycle", unavailable);
                warnedNativeUnavailable = true;
            }
        }
        int displayId = display.getDisplayId();
        if (lastDisplay != displayId || lastMode != active.getModeId()
                || lastRequest != requested || lastCap != cap || lastHz != reportedHz
                || lastDesiredHz != desiredHz) {
            float pacingHz = validRate(reportedHz) ? reportedHz : 60;
            float target = cap == 0 ? pacingHz : Math.min((float) cap, pacingHz);
            Log.i(TAG, "reason=" + reason + " display=" + displayId + " cap=" + cap
                    + " requested_mode=" + requested + " requested_hz=" + desiredHz
                    + " reported_mode=" + active.getModeId() + " reported_hz=" + reportedHz
                    + " effective_target=" + target + " mode_hz=" + active.getRefreshRate()
                    + " pixels=" + active.getPhysicalWidth() + "x" + active.getPhysicalHeight());
            lastDisplay = displayId;
            lastMode = active.getModeId();
            lastRequest = requested;
            lastCap = cap;
            lastHz = reportedHz;
            lastDesiredHz = desiredHz;
        }
    }

    static int parseProfile(String profile) {
        if ("30".equals(profile)) return 30;
        if ("45".equals(profile)) return 45;
        if ("display".equals(profile)) return 0;
        return 60;
    }

    static boolean validRate(float hz) {
        return hz >= 1 && hz <= 1000 && !Float.isInfinite(hz) && !Float.isNaN(hz);
    }

    static boolean compatibleRate(float hz, int cap) {
        if (cap <= 0 || !validRate(hz)) return false;
        float ratio = hz / cap;
        int multiple = Math.round(ratio);
        // Permit fractional nominal modes such as 59.94 and 89.9 without rounding readback.
        return multiple >= 1 && Math.abs(ratio - multiple) <= 0.005f * multiple;
    }

    /** Same resolution: lowest compatible refresh for a fixed cap, fastest for display mode. */
    static int choose(int cap, int currentId, int width, int height, int[] ids,
            int[] widths, int[] heights, float[] rates) {
        if (ids == null || widths == null || heights == null || rates == null
                || ids.length != widths.length || ids.length != heights.length
                || ids.length != rates.length) throw new IllegalArgumentException("mode arrays");
        if (cap != 0 && cap != 30 && cap != 45 && cap != 60) cap = 60;
        int bestCompatible = -1;
        int fastest = -1;
        for (int i = 0; i < ids.length; i++) {
            if (ids[i] <= 0 || widths[i] != width || heights[i] != height
                    || width <= 0 || height <= 0 || !validRate(rates[i])) continue;
            boolean tieFastest = fastest >= 0 && rates[i] == rates[fastest]
                    && preferId(ids[i], ids[fastest], currentId);
            if (fastest < 0 || rates[i] > rates[fastest] || tieFastest) fastest = i;
            if (cap != 0 && compatibleRate(rates[i], cap)) {
                boolean tieCompatible = bestCompatible >= 0 && rates[i] == rates[bestCompatible]
                        && preferId(ids[i], ids[bestCompatible], currentId);
                if (bestCompatible < 0 || rates[i] < rates[bestCompatible] || tieCompatible)
                    bestCompatible = i;
            }
        }
        // Unsupported fixed cadence stays fixed in the native pacer; do not substitute 30/60.
        return bestCompatible >= 0 ? bestCompatible : fastest;
    }

    private static boolean preferId(int candidate, int previous, int currentId) {
        return previous != currentId && (candidate == currentId || candidate < previous);
    }

    @Override public void onDisplayChanged(int displayId) {
        if (current != this) return;
        try {
            Display display = display();
            if (display != null && display.getDisplayId() == displayId) refresh("change");
        } catch (RuntimeException failure) { Log.w(TAG, "Display readback failed", failure); }
    }
    @Override public void onDisplayAdded(int displayId) { onDisplayChanged(displayId); }
    @Override public void onDisplayRemoved(int displayId) { onDisplayChanged(displayId); }
}
