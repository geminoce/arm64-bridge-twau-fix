// SPDX-License-Identifier: GPL-3.0-or-later
#include "wolf_renderdoc_gles.h"

#if WOLF_RENDERDOC_GLES_LAYER
#include "renderdoc_app.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>

#define LOG_TAG "Arm32Bridge_RenderDocGLES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" void* AndroidGLESLayer_Initialize(
    void* layer_id, void* (*get_next_layer_proc_address)(void*, const char*));
extern "C" void* AndroidGLESLayer_GetProcAddress(
    const char* function_name, __eglMustCastToProperFunctionPointerType next);
extern "C" int RENDERDOC_CC RENDERDOC_GetAPI(RENDERDOC_Version version, void** out_api);

namespace wolf_renderdoc_gles {
namespace {
std::once_flag g_init_once;
std::atomic<bool> g_ready{false};
void* g_egl = nullptr;
void* g_gles3 = nullptr;
void* g_gles2 = nullptr;
void* g_gles1 = nullptr;
int g_layer_identity = 0;

void* RawLookup(const char* name) noexcept {
    if (!name || !*name) return nullptr;
    void* result = nullptr;
    if (std::strncmp(name, "egl", 3) == 0) {
        if (g_egl) result = dlsym(g_egl, name);
    } else {
        if (g_gles3) result = dlsym(g_gles3, name);
        if (!result && g_gles2) result = dlsym(g_gles2, name);
        if (!result && g_gles1) result = dlsym(g_gles1, name);
        if (!result && g_egl) {
            using GetProc = __eglMustCastToProperFunctionPointerType (*)(const char*);
            auto get_proc = reinterpret_cast<GetProc>(dlsym(g_egl, "eglGetProcAddress"));
            if (get_proc) result = reinterpret_cast<void*>(get_proc(name));
        }
    }
    return result;
}

void* NextLayerProc(void* layer_id, const char* name) {
    if (layer_id != &g_layer_identity) return nullptr;
    return RawLookup(name);
}

bool IsIntercepted(const char* name) {
    void* next = RawLookup(name);
    if (!next) return false;
    void* hooked = AndroidGLESLayer_GetProcAddress(
        name, reinterpret_cast<__eglMustCastToProperFunctionPointerType>(next));
    return hooked && hooked != next;
}
} // namespace

bool IsReady() noexcept { return g_ready.load(std::memory_order_acquire); }

void* ResolveNext(const char* name) noexcept { return RawLookup(name); }

void* ResolveLayer(const char* name, void* next) noexcept {
    if (!IsReady() || !name || !next) return nullptr;
    return AndroidGLESLayer_GetProcAddress(
        name, reinterpret_cast<__eglMustCastToProperFunctionPointerType>(next));
}

void Initialize() {
    std::call_once(g_init_once, [] {
        g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        g_gles3 = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        g_gles2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        g_gles1 = dlopen("libGLESv1_CM.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_egl || (!g_gles3 && !g_gles2)) {
            LOGE("manual GLES layer init failed: driver handles EGL=%d GLESv3=%d GLESv2=%d",
                 g_egl != nullptr, g_gles3 != nullptr, g_gles2 != nullptr);
            return;
        }

        // The Android layer ABI does not assign meaning to the initializer's
        // return value. GetProcAddress below proves whether hooks were supplied.
        (void)AndroidGLESLayer_Initialize(&g_layer_identity, NextLayerProc);

        constexpr const char* probes[] = {
            "eglGetProcAddress", "eglMakeCurrent", "eglSwapBuffers",
            "glUseProgram", "glDrawElements", "glBufferData"};
        unsigned intercepted = 0;
        for (const char* name : probes) intercepted += IsIntercepted(name) ? 1u : 0u;
        if (!intercepted) {
            LOGE("RenderDoc layer initialized but intercepted none of the bridge entry points");
            return;
        }

        void* raw_api = nullptr;
        const int api_ok = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_0_0, &raw_api);
        bool overlay_masked = false;
        if (api_ok && raw_api) {
            auto* api = static_cast<RENDERDOC_API_1_0_0*>(raw_api);
            if (api->MaskOverlayBits) {
                api->MaskOverlayBits(0, 0);
                overlay_masked = true;
            }
            if (api->SetCaptureKeys) api->SetCaptureKeys(nullptr, 0);
            if (api->SetFocusToggleKeys) api->SetFocusToggleKeys(nullptr, 0);
        } else {
            LOGE("RenderDoc API unavailable (GetAPI=%d)", api_ok);
        }
        if (!overlay_masked) {
            LOGE("RenderDoc overlay API unavailable; leaving native GLES dispatch active");
            return;
        }

        g_ready.store(true, std::memory_order_release);
        LOGI("manual GLES layer active; intercepted=%u/%zu overlay_masked=%d capture_keys_disabled=%d",
             intercepted, std::size(probes), overlay_masked, api_ok && raw_api);
    });
}

} // namespace wolf_renderdoc_gles
#else
namespace wolf_renderdoc_gles {
void Initialize() {}
bool IsReady() noexcept { return false; }
void* ResolveNext(const char*) noexcept { return nullptr; }
void* ResolveLayer(const char*, void*) noexcept { return nullptr; }
} // namespace wolf_renderdoc_gles
#endif
