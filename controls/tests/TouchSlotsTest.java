// SPDX-License-Identifier: GPL-3.0-or-later
package org.libsdl.app;

import java.util.ArrayList;
import java.util.List;

public final class TouchSlotsTest {
    private static void check(boolean ok, String label) {
        if (!ok) throw new AssertionError(label);
    }

    public static void main(String[] args) {
        List<String> events = new ArrayList<>();
        TouchSlots slots = new TouchSlots((device, slot, action, x, y, pressure) ->
                events.add(device + ":" + slot + ":" + action));

        check(slots.down(10, 7, 0.2f, 0.3f, 1.0f) == 0, "first slot");
        check(slots.down(10, 8, 0.4f, 0.5f, 1.0f) == 1, "second slot");
        check(slots.down(20, 7, 0.6f, 0.7f, 1.0f) == 2, "separate device");
        check(slots.down(10, 7, 0.2f, 0.3f, 1.0f) == 0, "duplicate down");
        check(events.equals(List.of("10:0:0", "10:1:5", "20:2:0")), "down actions");
        events.clear();

        slots.move(10, 7, 0.5f, 0.6f, 0.7f);
        slots.move(10, 99, 0.5f, 0.6f, 0.7f);
        slots.cancelDevice(10);
        slots.cancelDevice(10);
        slots.up(10, 7, 0.5f, 0.6f);
        check(events.equals(List.of("10:0:2", "10:0:6", "10:1:1")), "move and cancel");
        events.clear();

        slots.releaseAll();
        slots.releaseAll();
        check(events.equals(List.of("20:2:1")), "release once per active finger");
        events.clear();

        for (int i = 0; i < 11; i++) {
            check(slots.down(30, i, 0.0f, 0.0f, 1.0f) == i, "bounded slot allocation");
        }
        check(slots.down(30, 99, 0.0f, 0.0f, 1.0f) == -1, "capacity rejection");
        check(events.size() == 11, "rejected down emits no event");
        slots.up(30, 5, 0.0f, 0.0f);
        check(slots.down(30, 99, 0.0f, 0.0f, 1.0f) == 5, "released slot reuse");
        slots.releaseAll();
        int released = events.size();
        slots.move(30, 99, 0.0f, 0.0f, 1.0f);
        check(events.size() == released, "released pointer cannot move");
        check(TouchSlots.normalize(50.0f, 100) == 0.5f, "coordinate normalization");
        System.out.println("TouchSlotsTest passed");
    }
}
