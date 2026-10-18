// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

class ThunkManager;
class DynarmicHost;

// Monotonic nanoseconds; kept separate so contract tests exercise the real
// scheduler without OS wake-up noise. period must come from SetBridgeFrameLimit.
struct BridgeFrameDeadline {
    int64_t period_ns = 0;
    int64_t next_ns = 0;
    int64_t last_call_ns = 0;
    int64_t Deadline(int64_t period, int64_t now);
    void Commit(int64_t now);
};

// Event-driven UI update. Returns the cached target period in nanoseconds.
int64_t SetBridgeFrameLimit(int cap, float actual_hz);

void RegisterAndroidThunks(ThunkManager& manager, DynarmicHost& host);
// Call after all guest workers stop, before FakeJNI and guest memory teardown.
void ResetAndroidThunks(DynarmicHost& host);
void ResetBridgeGlStorage(DynarmicHost& host);
void ForgetBridgeGlContext(DynarmicHost& host, void* context);
void TrackBridgeGlContext(DynarmicHost& host, void* context, void* shared_context);

// Bounded display-size observations after a successful swap.
void ObserveBridgeGlSwap(DynarmicHost&, void* display, void* surface, uint32_t lr, uint32_t sp, bool ok);
