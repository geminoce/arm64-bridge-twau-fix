// SPDX-License-Identifier: GPL-3.0-or-later
package org.libsdl.app;

import android.view.MotionEvent;
import android.view.View;

/** Uses the original SDL touch JNI; this class creates no key/pad events. */
public final class TouchAdapter {
    private static final TouchSlots SLOTS = new TouchSlots(new TouchSlots.Sink() {
        @Override
        public void touch(int device, int slot, int action, float x, float y, float pressure) {
            SDLActivity.onNativeTouch(device, slot, action, x, y, pressure);
        }
    });

    private TouchAdapter() {}

    public static void releaseAll() {
        SLOTS.releaseAll();
    }

    public static boolean onTouch(View view, MotionEvent event) {
        int device = event.getDeviceId();
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            SLOTS.cancelDevice(device);
            return true;
        }
        if (SDLActivity.mIsPaused || !SDLActivity.mHasFocus || !SDLActivity.mIsSurfaceReady) {
            SLOTS.releaseAll();
            return true;
        }
        if (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_POINTER_DOWN
                && action != MotionEvent.ACTION_MOVE && action != MotionEvent.ACTION_UP
                && action != MotionEvent.ACTION_POINTER_UP) {
            return true;
        }
        int width = view.getWidth();
        int height = view.getHeight();
        if (width <= 0 || height <= 0) {
            SLOTS.cancelDevice(device);
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN) SLOTS.cancelDevice(device);
        if (action == MotionEvent.ACTION_MOVE) {
            for (int index = 0; index < event.getPointerCount(); index++) {
                SLOTS.move(device, event.getPointerId(index),
                        TouchSlots.normalize(event.getX(index), width),
                        TouchSlots.normalize(event.getY(index), height), Math.min(event.getPressure(index), 1.0f));
            }
        } else {
            int index = event.getActionIndex();
            if (index < 0 || index >= event.getPointerCount()) {
                SLOTS.cancelDevice(device);
                return true;
            }
            int pointer = event.getPointerId(index);
            float x = TouchSlots.normalize(event.getX(index), width);
            float y = TouchSlots.normalize(event.getY(index), height);
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                SLOTS.down(device, pointer, x, y, Math.min(event.getPressure(index), 1.0f));
            } else {
                SLOTS.up(device, pointer, x, y);
                // ACTION_UP ends the whole device gesture, including malformed leftovers.
                if (action == MotionEvent.ACTION_UP) SLOTS.cancelDevice(device);
            }
        }
        return true;
    }
}
