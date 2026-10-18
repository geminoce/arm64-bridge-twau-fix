// SPDX-License-Identifier: GPL-3.0-or-later
#include "thunks_android.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include "dynarmic_host.h"
#include "jni_fake_env.h"

#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#define LOG_TAG "Arm32Bridge_Android"
#if ARM32_BRIDGE_ENABLE_LOGGING
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

namespace {
using FrameClock = std::chrono::steady_clock;
std::atomic<int64_t> frame_period_ns{16666667}; // Default 60 fps before Java readback.
thread_local BridgeFrameDeadline frame_deadline;

void PaceBridgeSwap(EGLDisplay display, EGLSurface surface) {
    // Current-state queries do not consume EGL errors; invalid swaps still reach EGL.
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE ||
        eglGetCurrentDisplay() != display || eglGetCurrentSurface(EGL_DRAW) != surface ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    const int64_t ns = frame_period_ns.load(std::memory_order_relaxed);
    const auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            FrameClock::now().time_since_epoch()).count();
    };
    const int64_t now = now_ns();
    const int64_t deadline = frame_deadline.Deadline(ns, now);
    if (now < deadline) {
        std::this_thread::sleep_until(FrameClock::time_point(
            std::chrono::duration_cast<FrameClock::duration>(std::chrono::nanoseconds(deadline))));
    }
    frame_deadline.Commit(now_ns());
}

template <typename T>
class HandleTable {
public:
    struct Entry { T value; uint32_t refs; };
    uint32_t Add(T value, bool acquired = false) {
        if (!value) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, entry] : values_) {
            if (entry.value != value) continue;
            if (acquired) {
                if (entry.refs == std::numeric_limits<uint32_t>::max()) return 0;
                ++entry.refs;
            }
            return handle;
        }
        if (!next_) return 0;
        const uint32_t handle = next_++;
        values_[handle] = Entry{value, 1};
        return handle;
    }
    T Get(uint32_t handle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(handle);
        return it == values_.end() ? T{} : it->second.value;
    }
    T Release(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(handle);
        if (it == values_.end()) return T{};
        const T value = it->second.value;
        if (--it->second.refs == 0) values_.erase(it);
        return value;
    }
    void Remove(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.erase(handle);
    }
    std::vector<Entry> Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Entry> entries;
        for (const auto& [handle, entry] : values_) entries.push_back(entry);
        values_.clear();
        // Keep monotonic handles so a stale reference cannot alias a new object.
        return entries;
    }
private:
    mutable std::mutex mutex_;
    uint32_t next_ = 0x60000000;
    std::unordered_map<uint32_t, Entry> values_;
};

HandleTable<EGLDisplay> displays;
HandleTable<EGLConfig> configs;
HandleTable<EGLContext> contexts;
HandleTable<EGLSurface> surfaces;
HandleTable<ANativeWindow*> windows;
HandleTable<AAssetManager*> asset_managers;
HandleTable<AAsset*> assets;
HandleTable<AAssetDir*> asset_dirs;

std::mutex storage_mutex;
std::unordered_map<uint32_t, uint32_t> directory_names;
std::unordered_map<std::string, uint32_t> query_strings;
std::unordered_map<uint32_t, jobject> asset_manager_owners;
std::unordered_map<uint32_t, EGLDisplay> context_displays;
std::unordered_map<uint32_t, EGLDisplay> surface_displays;
std::unordered_map<std::thread::id, EGLContext> current_contexts;
std::unordered_set<EGLContext> pending_contexts;

void DropDestroyedContexts(DynarmicHost& host) {
    for (auto it = pending_contexts.begin(); it != pending_contexts.end();) {
        const EGLContext context = *it;
        const bool current = std::any_of(current_contexts.begin(), current_contexts.end(),
            [context](const auto& entry) { return entry.second == context; });
        if (!current && eglGetCurrentContext() != context) {
            ForgetBridgeGlContext(host, context);
            it = pending_contexts.erase(it);
        } else ++it;
    }
}

const char* GuestString(DynarmicHost& host, uint32_t address) {
    if (!GuestRangeValid(host.GetGuestMemorySize(), address, 1)) return nullptr;
    const char* value = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
    const size_t bytes = std::min<size_t>(host.GetGuestMemorySize() - address, 1024 * 1024);
    return memchr(value, 0, bytes) ? value : nullptr;
}

const EGLint* GuestEglInts(DynarmicHost& host, uint32_t address) {
    if (!address || address % alignof(EGLint)) return nullptr;
    const size_t size = host.GetGuestMemorySize();
    for (uint64_t offset = 0; offset < 2048; offset += 2 * sizeof(EGLint)) {
        const uint64_t cursor = static_cast<uint64_t>(address) + offset;
        if (cursor + sizeof(EGLint) > size || cursor > UINT32_MAX) break;
        // EGL_NONE terminates a key, not a value in a key/value pair.
        if (host.MemoryRead32(static_cast<uint32_t>(cursor)) == EGL_NONE)
            return reinterpret_cast<const EGLint*>(host.GetGuestMemoryBase() + address);
        if (cursor + 2 * sizeof(EGLint) > size) break;
    }
    LOGE("EGL rejected unterminated guest attributes at 0x%08X", address);
    return nullptr;
}

uint32_t CopyHostString(DynarmicHost& host, const char* value) {
    if (!value) return 0;
    const size_t size = strlen(value) + 1;
    const uint32_t guest = host.GuestAlloc(size, 4);
    if (guest) memcpy(host.GetGuestMemoryBase() + guest, value, size);
    return guest;
}

void SetGuestError(DynarmicHost& host, int value) {
    errno = value;
    const uint32_t address = host.CurrentGuestErrnoAddress();
    if (address) host.MemoryWrite32(address, value);
}
}

int64_t BridgeFrameDeadline::Deadline(int64_t period, int64_t now) {
    // Detect a real pause separately from accumulated ordinary frame lateness.
    // 200 ms keeps weak 5/95 ms work from being mistaken for a pause at 60 Hz.
    if (period_ns != period || now - last_call_ns > std::max<int64_t>(200000000, period * 4)) {
        period_ns = period;
        next_ns = now;
    }
    last_call_ns = now;
    return next_ns;
}

void BridgeFrameDeadline::Commit(int64_t now) {
    // An unusually late wake-up is a pause too. No replay after suspend/stalls.
    if (now - last_call_ns > std::max<int64_t>(200000000, period_ns * 4)) next_ns = now;
    last_call_ns = now;
    // Keep phase when a frame is late. Bound recovery to four target periods;
    // never queue/replay guest frames or mutate the simulation clock.
    next_ns = std::max(next_ns + period_ns, now - period_ns * 4);
}

int64_t SetBridgeFrameLimit(int cap, float actual_hz) {
    if (cap != 0 && cap != 30 && cap != 45 && cap != 60) cap = 60;
    // Keep malformed/tiny/absurd rates from creating unbounded waits or overflow.
    const double hz = std::isfinite(actual_hz) && actual_hz >= 1.0f && actual_hz <= 1000.0f
        ? static_cast<double>(actual_hz) : 60.0;
    const double target = cap == 0 ? hz : std::min(static_cast<double>(cap), hz);
    const int64_t ns = static_cast<int64_t>(std::llround(1000000000.0 / target));
    frame_period_ns.store(ns, std::memory_order_relaxed);
    return ns;
}

// The coordinator calls this after all guest workers have stopped and before
// destroying FakeJNI/the guest arena. No native or Java owner is left behind.
void ResetAndroidThunks(DynarmicHost& host) {
    frame_deadline = {};
    std::lock_guard<std::mutex> lock(storage_mutex);
    const EGLDisplay current = eglGetCurrentDisplay();
    if (current != EGL_NO_DISPLAY) eglMakeCurrent(current, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (const auto& [handle, display] : surface_displays) {
        const EGLSurface surface = surfaces.Get(handle);
        if (surface) eglDestroySurface(display, surface);
    }
    for (const auto& [handle, display] : context_displays) {
        const EGLContext context = contexts.Get(handle);
        if (context) eglDestroyContext(display, context);
    }
    surface_displays.clear(); context_displays.clear();
    current_contexts.clear(); pending_contexts.clear();
    surfaces.Clear(); contexts.Clear(); configs.Clear(); displays.Clear();
    ResetBridgeGlStorage(host);
    for (const auto& entry : windows.Clear())
        for (uint32_t i = 0; i < entry.refs; ++i) ANativeWindow_release(entry.value);
    for (const auto& entry : assets.Clear()) AAsset_close(entry.value);
    for (const auto& entry : asset_dirs.Clear()) AAssetDir_close(entry.value);
    for (const auto& [handle, address] : directory_names) host.GuestFree(address);
    directory_names.clear();
    for (const auto& [value, address] : query_strings) host.GuestFree(address);
    query_strings.clear();
    JNIEnv* env = FakeJniEnv::Instance().GetEnv();
    if (env) for (const auto& [handle, owner] : asset_manager_owners) env->DeleteGlobalRef(owner);
    asset_manager_owners.clear(); asset_managers.Clear();
}

void RegisterAndroidThunks(ThunkManager& manager, DynarmicHost& host) {
    manager.RegisterThunk("ANativeWindow_fromSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        JNIEnv* env = FakeJniEnv::Instance().GetEnv();
        jobject surface = FakeJniEnv::Instance().ResolveObject(jit.Regs()[1]);
        ANativeWindow* window = env && surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
        if (env && surface) env->DeleteLocalRef(surface);
        jit.Regs()[0] = windows.Add(window, true);
        if (window && !jit.Regs()[0]) ANativeWindow_release(window);
    });
    manager.RegisterThunk("ANativeWindow_release", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        ANativeWindow* window = windows.Release(jit.Regs()[0]);
        if (window) ANativeWindow_release(window);
    });

    manager.RegisterThunk("AAssetManager_fromJava", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        JNIEnv* env = FakeJniEnv::Instance().GetEnv();
        jobject object = FakeJniEnv::Instance().ResolveObject(jit.Regs()[1]);
        AAssetManager* native = env && object ? AAssetManager_fromJava(env, object) : nullptr;
        std::lock_guard<std::mutex> lock(storage_mutex);
        const uint32_t handle = asset_managers.Add(native);
        jit.Regs()[0] = handle;
        if (handle && !asset_manager_owners.count(handle)) {
            jobject owner = env->NewGlobalRef(object);
            if (owner) asset_manager_owners.emplace(handle, owner);
            else { asset_managers.Remove(handle); jit.Regs()[0] = 0; }
        }
        if (env && object) env->DeleteLocalRef(object);
    });
    manager.RegisterThunk("AAssetManager_open", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetManager* manager_ptr = asset_managers.Get(jit.Regs()[0]);
        const char* name = GuestString(host, jit.Regs()[1]);
        AAsset* asset = (manager_ptr && name) ? AAssetManager_open(manager_ptr, name, jit.Regs()[2]) : nullptr;
        jit.Regs()[0] = assets.Add(asset);
    });
    manager.RegisterThunk("AAssetManager_openDir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetManager* manager_ptr = asset_managers.Get(jit.Regs()[0]);
        const char* name = GuestString(host, jit.Regs()[1]);
        AAssetDir* dir = (manager_ptr && name) ? AAssetManager_openDir(manager_ptr, name) : nullptr;
        jit.Regs()[0] = asset_dirs.Add(dir);
    });
    manager.RegisterThunk("AAsset_read", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        void* buffer = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], jit.Regs()[2]);
        jit.Regs()[0] = (asset && buffer) ? static_cast<uint32_t>(AAsset_read(asset, buffer, jit.Regs()[2])) : static_cast<uint32_t>(-1);
    });
    manager.RegisterThunk("AAsset_seek", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        if (asset && AAsset_getLength64(asset) > INT32_MAX) {
            SetGuestError(host, EOVERFLOW); jit.Regs()[0] = static_cast<uint32_t>(-1); return;
        }
        const off64_t result = asset ? AAsset_seek64(asset, static_cast<int32_t>(jit.Regs()[1]), jit.Regs()[2]) : -1;
        if (result > INT32_MAX) { SetGuestError(host, EOVERFLOW); jit.Regs()[0] = static_cast<uint32_t>(-1); }
        else jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    manager.RegisterThunk("AAsset_close", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        if (asset) AAsset_close(asset);
        assets.Remove(jit.Regs()[0]);
    });
    manager.RegisterThunk("AAsset_getLength", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        const off64_t length = asset ? AAsset_getLength64(asset) : 0;
        if (length < 0 || length > INT32_MAX) { SetGuestError(host, EOVERFLOW); jit.Regs()[0] = static_cast<uint32_t>(-1); }
        else jit.Regs()[0] = static_cast<uint32_t>(length);
    });
    manager.RegisterThunk("AAsset_openFileDescriptor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        const uint32_t start_out = jit.Regs()[1], length_out = jit.Regs()[2];
        if ((start_out && !GuestRangeValid(host.GetGuestMemorySize(), start_out, 4)) ||
            (length_out && !GuestRangeValid(host.GetGuestMemorySize(), length_out, 4))) {
            SetGuestError(host, EFAULT); jit.Regs()[0] = static_cast<uint32_t>(-1); return;
        }
        off64_t start = 0, length = 0;
        const int fd = asset ? AAsset_openFileDescriptor64(asset, &start, &length) : -1;
        if (fd >= 0 && (start < 0 || length < 0 || start > INT32_MAX || length > INT32_MAX)) {
            close(fd); SetGuestError(host, EOVERFLOW); jit.Regs()[0] = static_cast<uint32_t>(-1); return;
        }
        if (fd >= 0) {
            if (start_out) host.MemoryWrite32(start_out, static_cast<uint32_t>(start));
            if (length_out) host.MemoryWrite32(length_out, static_cast<uint32_t>(length));
        }
        jit.Regs()[0] = static_cast<uint32_t>(fd);
    });
    manager.RegisterThunk("AAssetDir_getNextFileName", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(storage_mutex);
        AAssetDir* dir = asset_dirs.Get(handle);
        auto old = directory_names.find(handle);
        if (old != directory_names.end()) { host.GuestFree(old->second); directory_names.erase(old); }
        const uint32_t address = dir ? CopyHostString(host, AAssetDir_getNextFileName(dir)) : 0;
        if (address) directory_names.emplace(handle, address);
        jit.Regs()[0] = address;
    });
    manager.RegisterThunk("AAssetDir_rewind", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(storage_mutex);
        const uint32_t handle = jit.Regs()[0];
        AAssetDir* dir = asset_dirs.Get(handle);
        if (dir) AAssetDir_rewind(dir);
        auto old = directory_names.find(handle);
        if (old != directory_names.end()) { host.GuestFree(old->second); directory_names.erase(old); }
    });
    manager.RegisterThunk("AAssetDir_close", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(storage_mutex);
        const uint32_t handle = jit.Regs()[0];
        AAssetDir* dir = asset_dirs.Get(handle);
        if (dir) AAssetDir_close(dir);
        asset_dirs.Remove(handle);
        auto old = directory_names.find(handle);
        if (old != directory_names.end()) { host.GuestFree(old->second); directory_names.erase(old); }
    });

    manager.RegisterThunk("eglBindAPI", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglBindAPI(jit.Regs()[0]); });
    manager.RegisterThunk("eglQueryAPI", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglQueryAPI(); });
    manager.RegisterThunk("eglGetDisplay", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        // Android's guest EGLNativeDisplayType admits EGL_DEFAULT_DISPLAY only.
        // An opaque host pointer must never be reconstructed from a guest word.
        jit.Regs()[0] = jit.Regs()[0] == 0 ? displays.Add(eglGetDisplay(EGL_DEFAULT_DISPLAY)) : 0;
    });
    manager.RegisterThunk("eglInitialize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        EGLint* major = jit.Regs()[1] ? static_cast<EGLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], sizeof(EGLint))) : nullptr;
        EGLint* minor = jit.Regs()[2] ? static_cast<EGLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], sizeof(EGLint))) : nullptr;
        if ((jit.Regs()[1] && !major) || (jit.Regs()[2] && !minor)) { jit.Regs()[0] = EGL_FALSE; return; }
        jit.Regs()[0] = eglInitialize(display, major, minor);
    });
    manager.RegisterThunk("eglChooseConfig", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[1]);
        const uint32_t out = jit.Regs()[2];
        const EGLint capacity = static_cast<EGLint>(jit.Regs()[3]);
        const uint32_t count_out = host.MemoryRead32(jit.Regs()[13]);
        if ((jit.Regs()[1] && !attributes) || capacity < 0 ||
            !GuestRangeValid(host.GetGuestMemorySize(), count_out, sizeof(EGLint)) ||
            (out && !GuestRangeValid(host.GetGuestMemorySize(), out, static_cast<size_t>(capacity) * sizeof(uint32_t)))) {
            jit.Regs()[0] = EGL_FALSE;
            return;
        }
        EGLint count = 0;
        EGLBoolean ok = eglChooseConfig(display, attributes, nullptr, 0, &count);
        if (ok && out && capacity > 0 && count > 0) {
            const EGLint limit = std::min(count, capacity);
            std::vector<EGLConfig> found(limit);
            ok = eglChooseConfig(display, attributes, found.data(), limit, &count);
            if (ok) for (EGLint i = 0; i < std::min(count, limit); ++i) host.MemoryWrite32(out + i * 4, configs.Add(found[i]));
        } else if (out && capacity == 0) {
            count = 0;
        }
        host.MemoryWrite32(count_out, static_cast<uint32_t>(count));
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunk("eglGetConfigs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        const uint32_t out = jit.Regs()[1];
        const EGLint capacity = static_cast<EGLint>(jit.Regs()[2]);
        const uint32_t count_out = jit.Regs()[3];
        if (capacity < 0 || !GuestRangeValid(host.GetGuestMemorySize(), count_out, sizeof(EGLint)) ||
            (out && !GuestRangeValid(host.GetGuestMemorySize(), out, static_cast<size_t>(capacity) * sizeof(uint32_t)))) {
            jit.Regs()[0] = EGL_FALSE;
            return;
        }
        EGLint count = 0;
        EGLBoolean ok = eglGetConfigs(display, nullptr, 0, &count);
        if (ok && out && capacity > 0 && count > 0) {
            const EGLint limit = std::min(count, capacity);
            std::vector<EGLConfig> found(limit);
            ok = eglGetConfigs(display, found.data(), limit, &count);
            if (ok) for (EGLint i = 0; i < std::min(count, limit); ++i) host.MemoryWrite32(out + i * 4, configs.Add(found[i]));
        } else if (out && capacity == 0) {
            count = 0;
        }
        host.MemoryWrite32(count_out, static_cast<uint32_t>(count));
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunk("eglGetConfigAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!GuestRangeValid(host.GetGuestMemorySize(), jit.Regs()[3], sizeof(EGLint))) { jit.Regs()[0] = EGL_FALSE; return; }
        EGLint value = 0;
        const EGLBoolean ok = eglGetConfigAttrib(displays.Get(jit.Regs()[0]), configs.Get(jit.Regs()[1]), jit.Regs()[2], &value);
        if (ok) host.MemoryWrite32(jit.Regs()[3], static_cast<uint32_t>(value));
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunk("eglCreateWindowSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[3]);
        if (jit.Regs()[3] && !attributes) { jit.Regs()[0] = 0; return; }
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        EGLSurface surface = eglCreateWindowSurface(display, configs.Get(jit.Regs()[1]),
            windows.Get(jit.Regs()[2]), attributes);
        jit.Regs()[0] = surfaces.Add(surface);
        if (jit.Regs()[0]) { std::lock_guard<std::mutex> lock(storage_mutex); surface_displays[jit.Regs()[0]] = display; }
    });
    manager.RegisterThunk("eglCreateContext", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[3]);
        if (jit.Regs()[3] && !attributes) { jit.Regs()[0] = 0; return; }
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLContext shared = contexts.Get(jit.Regs()[2]);
        EGLContext context = eglCreateContext(display, configs.Get(jit.Regs()[1]),
            shared, attributes);
        jit.Regs()[0] = contexts.Add(context);
        if (jit.Regs()[0]) {
            TrackBridgeGlContext(host, context, shared);
            std::lock_guard<std::mutex> lock(storage_mutex);
            context_displays[jit.Regs()[0]] = display;
        }
    });
    manager.RegisterThunk("eglMakeCurrent", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLContext context = contexts.Get(jit.Regs()[3]);
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLSurface draw = surfaces.Get(jit.Regs()[1]), read = surfaces.Get(jit.Regs()[2]);
        jit.Regs()[0] = eglMakeCurrent(display, draw, read, context);
        if (jit.Regs()[0]) {
            frame_deadline = {};
            std::lock_guard<std::mutex> lock(storage_mutex);
            const auto thread = std::this_thread::get_id();
            if (context) current_contexts[thread] = context;
            else current_contexts.erase(thread);
            DropDestroyedContexts(host);
        }
    });
    manager.RegisterThunk("eglSwapBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLSurface surface = surfaces.Get(jit.Regs()[1]);
        PaceBridgeSwap(display, surface);
        jit.Regs()[0] = eglSwapBuffers(display, surface);
        ObserveBridgeGlSwap(host, display, surface, jit.Regs()[14], jit.Regs()[13], jit.Regs()[0] != EGL_FALSE);
    });
    manager.RegisterThunk("eglSwapInterval", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = eglSwapInterval(displays.Get(jit.Regs()[0]), static_cast<EGLint>(jit.Regs()[1]));
    });
    manager.RegisterThunk("eglDestroySurface", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLSurface surface = surfaces.Get(jit.Regs()[1]);
        const EGLBoolean ok = eglDestroySurface(display, surface);
        if (ok) { std::lock_guard<std::mutex> lock(storage_mutex); surface_displays.erase(jit.Regs()[1]); surfaces.Remove(jit.Regs()[1]); }
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunk("eglDestroyContext", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLContext context = contexts.Get(jit.Regs()[1]);
        const EGLBoolean ok = eglDestroyContext(displays.Get(jit.Regs()[0]), context);
        if (ok) {
            std::lock_guard<std::mutex> lock(storage_mutex);
            context_displays.erase(jit.Regs()[1]); contexts.Remove(jit.Regs()[1]);
            pending_contexts.insert(context);
            DropDestroyedContexts(host);
        }
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunk("eglTerminate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        jit.Regs()[0] = eglTerminate(display);
        if (jit.Regs()[0]) {
            std::lock_guard<std::mutex> lock(storage_mutex);
            for (auto it = context_displays.begin(); it != context_displays.end();) {
                if (it->second == display) {
                    pending_contexts.insert(contexts.Get(it->first));
                    contexts.Remove(it->first);
                    it = context_displays.erase(it);
                } else ++it;
            }
            for (auto it = surface_displays.begin(); it != surface_displays.end();) {
                if (it->second == display) {
                    surfaces.Remove(it->first); it = surface_displays.erase(it);
                }
                else ++it;
            }
            DropDestroyedContexts(host);
        }
    });
    manager.RegisterThunk("eglWaitNative", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglWaitNative(jit.Regs()[0]); });
    manager.RegisterThunk("eglWaitGL", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglWaitGL(); });
    manager.RegisterThunk("eglReleaseThread", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = eglReleaseThread();
        if (jit.Regs()[0]) {
            std::lock_guard<std::mutex> lock(storage_mutex);
            current_contexts.erase(std::this_thread::get_id());
            DropDestroyedContexts(host);
        }
    });
    manager.RegisterThunk("eglGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglGetError(); });
    manager.RegisterThunk("eglQueryString", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* value = eglQueryString(displays.Get(jit.Regs()[0]), jit.Regs()[1]);
        if (!value) { jit.Regs()[0] = 0; return; }
        std::lock_guard<std::mutex> lock(storage_mutex);
        auto it = query_strings.find(value);
        if (it != query_strings.end()) { jit.Regs()[0] = it->second; return; }
        const uint32_t address = CopyHostString(host, value);
        if (address) query_strings.emplace(value, address);
        jit.Regs()[0] = address;
    });
    manager.RegisterThunk("eglGetCurrentDisplay", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = displays.Add(eglGetCurrentDisplay()); });
    manager.RegisterThunk("eglGetCurrentContext", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = contexts.Add(eglGetCurrentContext()); });
    manager.RegisterThunk("eglGetCurrentSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = surfaces.Add(eglGetCurrentSurface(jit.Regs()[0])); });
    manager.RegisterThunk("eglGetProcAddress", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = GuestString(host, jit.Regs()[0]);
        if (!name) { jit.Regs()[0] = 0; return; }
        // Export a guest trampoline only. A known optional GL wrapper is not
        // evidence that the actual native driver supplies that entry point.
        if (strncmp(name, "gl", 2) == 0 && !eglGetProcAddress(name)) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = host.GetThunkManager().FindProvider(name, ThunkManager::ProviderKind::Function);
    });

    LOGI("Registered Android asset/window/EGL thunks");
}
