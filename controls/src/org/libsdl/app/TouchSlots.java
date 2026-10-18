// SPDX-License-Identifier: GPL-3.0-or-later
package org.libsdl.app;

/** Bounded legacy finger IDs; no Android or controller dependencies. */
public final class TouchSlots {
    public interface Sink {
        void touch(int device, int slot, int action, float x, float y, float pressure);
    }

    private static final int LIMIT = 11;
    private final Sink sink;
    private final boolean[] active = new boolean[LIMIT];
    private final int[] devices = new int[LIMIT];
    private final int[] pointers = new int[LIMIT];
    private final float[] xs = new float[LIMIT];
    private final float[] ys = new float[LIMIT];

    public TouchSlots(Sink sink) {
        this.sink = sink;
    }

    public static float normalize(float coordinate, int extent) {
        return coordinate / extent;
    }

    private int find(int device, int pointer) {
        for (int slot = 0; slot < LIMIT; slot++) {
            if (active[slot] && devices[slot] == device && pointers[slot] == pointer) {
                return slot;
            }
        }
        return -1;
    }

    private int deviceCount(int device) {
        int count = 0;
        for (int slot = 0; slot < LIMIT; slot++) {
            if (active[slot] && devices[slot] == device) count++;
        }
        return count;
    }

    public synchronized int down(int device, int pointer, float x, float y, float pressure) {
        int existing = find(device, pointer);
        if (existing >= 0) return existing;
        for (int slot = 0; slot < LIMIT; slot++) {
            if (!active[slot]) {
                int action = deviceCount(device) == 0 ? 0 : 5;
                devices[slot] = device;
                pointers[slot] = pointer;
                xs[slot] = x;
                ys[slot] = y;
                active[slot] = true;
                sink.touch(device, slot, action, x, y, pressure);
                return slot;
            }
        }
        return -1;
    }

    public synchronized void move(int device, int pointer, float x, float y, float pressure) {
        int slot = find(device, pointer);
        if (slot >= 0) {
            xs[slot] = x;
            ys[slot] = y;
            sink.touch(device, slot, 2, x, y, pressure);
        }
    }

    private void release(int slot, float x, float y) {
        int action = deviceCount(devices[slot]) == 1 ? 1 : 6;
        // The mapping remains active until its one native UP has been delivered.
        sink.touch(devices[slot], slot, action, x, y, 0.0f);
        active[slot] = false;
        xs[slot] = 0.0f;
        ys[slot] = 0.0f;
    }

    public synchronized void up(int device, int pointer, float x, float y) {
        int slot = find(device, pointer);
        if (slot >= 0) release(slot, x, y);
    }

    public synchronized void cancelDevice(int device) {
        for (int slot = 0; slot < LIMIT; slot++) {
            if (active[slot] && devices[slot] == device) release(slot, xs[slot], ys[slot]);
        }
    }

    public synchronized void releaseAll() {
        for (int slot = 0; slot < LIMIT; slot++) {
            if (active[slot]) release(slot, xs[slot], ys[slot]);
        }
    }
}
