// SPDX-License-Identifier: GPL-3.0-or-later
// Exact SDL2/FMOD Ex JNI adapter for The Wolf Among Us 1.23.
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "jni_fake_env.h"
#include "thunk_manager.h"
#include "thunks_android.h"
#include "thunks_opensles.h"
#include "wolf_display_overrides.h"
#if WOLF_RENDERDOC_GLES_LAYER
#include "wolf_renderdoc_gles.h"
#endif
#ifdef WOLF_JNI_CONTRACT_CHECKS
#include "../tests/fakejni_art_checks.h"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "WolfBridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "WolfBridge", __VA_ARGS__)
std::unique_ptr<DynarmicHost> g_host;
JavaVM* g_jvm = nullptr;
JavaVM* GetHostJavaVM() { return g_jvm; }
extern "C" int IndexApks(const char* const* paths, size_t count);
extern "C" int TelltaleWidescreenOverrideEnabled();
static std::mutex g_load_mutex;
static std::atomic<bool> g_ready{false};
static bool g_load_failed = false;
static std::mutex g_fmod_buffer_mutex;
static uint32_t g_fmod_buffer = 0;
static std::array<uint32_t, 25> g_symbols{};
struct MethodSpec { const char* cls; const char* method; const char* signature; const char* library; const char* symbol; };
static constexpr MethodSpec kSpecs[] = {
    {"com/telltalegames/telltale/TelltaleActivity", "nativeOnPurchaseComplete", "(Ljava/lang/String;ZLjava/lang/String;)V", "libGameEngine.so", "Java_com_telltalegames_telltale_TelltaleActivity_nativeOnPurchaseComplete"},
    {"org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", "libfmodex.so", "Java_org_fmod_FMODAudioDevice_fmodGetInfo"},
    {"org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", "libfmodex.so", "Java_org_fmod_FMODAudioDevice_fmodProcess"},
    {"org/libsdl/app/SDLActivity", "nativeAddJoystick", "(ILjava/lang/String;Ljava/lang/String;IIIII)I", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeAddJoystick"},
    {"org/libsdl/app/SDLActivity", "nativeFlipBuffers", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeFlipBuffers"},
    {"org/libsdl/app/SDLActivity", "nativeInit", "()V", "libmain.so", "Java_org_libsdl_app_SDLActivity_nativeInit"},
    {"org/libsdl/app/SDLActivity", "nativeLowMemory", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeLowMemory"},
    {"org/libsdl/app/SDLActivity", "nativePause", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativePause"},
    {"org/libsdl/app/SDLActivity", "nativeQuit", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeQuit"},
    {"org/libsdl/app/SDLActivity", "nativeRemoveJoystick", "(I)I", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeRemoveJoystick"},
    {"org/libsdl/app/SDLActivity", "nativeResume", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_nativeResume"},
    {"org/libsdl/app/SDLActivity", "onNativeAccel", "(FFF)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeAccel"},
    {"org/libsdl/app/SDLActivity", "onNativeHat", "(IIII)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeHat"},
    {"org/libsdl/app/SDLActivity", "onNativeJoy", "(IIF)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeJoy"},
    {"org/libsdl/app/SDLActivity", "onNativeKeyDown", "(I)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeKeyDown"},
    {"org/libsdl/app/SDLActivity", "onNativeKeyUp", "(I)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeKeyUp"},
    {"org/libsdl/app/SDLActivity", "onNativeKeyboardFocusLost", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeKeyboardFocusLost"},
    {"org/libsdl/app/SDLActivity", "onNativePadDown", "(II)I", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativePadDown"},
    {"org/libsdl/app/SDLActivity", "onNativePadUp", "(II)I", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativePadUp"},
    {"org/libsdl/app/SDLActivity", "onNativeResize", "(III)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeResize"},
    {"org/libsdl/app/SDLActivity", "onNativeSurfaceChanged", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged"},
    {"org/libsdl/app/SDLActivity", "onNativeSurfaceDestroyed", "()V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeSurfaceDestroyed"},
    {"org/libsdl/app/SDLActivity", "onNativeTouch", "(IIIFFF)V", "libSDL2.so", "Java_org_libsdl_app_SDLActivity_onNativeTouch"},
    {"org/libsdl/app/SDLInputConnection", "nativeCommitText", "(Ljava/lang/String;I)V", "libSDL2.so", "Java_org_libsdl_app_SDLInputConnection_nativeCommitText"},
    {"org/libsdl/app/SDLInputConnection", "nativeSetComposingText", "(Ljava/lang/String;I)V", "libSDL2.so", "Java_org_libsdl_app_SDLInputConnection_nativeSetComposingText"},
};
static void Error(JNIEnv* env, const char* message) {
    LOGE("%s", message);
    if (!env->ExceptionCheck()) {
        jclass type = env->FindClass("java/lang/IllegalStateException");
        if (type) { env->ThrowNew(type, message); env->DeleteLocalRef(type); }
    }
}
struct LocalFrame {
    JNIEnv* env;
    bool valid;
    explicit LocalFrame(JNIEnv* e) : env(e), valid(e->PushLocalFrame(64) == JNI_OK) {}
    ~LocalFrame() { if (valid) env->PopLocalFrame(nullptr); }
};
struct NativeScope {
    NativeScope() { FakeJniEnv::Instance().BeginNativeCall(); }
    ~NativeScope() { FakeJniEnv::Instance().EndNativeCall(); }
};
static std::string JavaString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (!text) return {};
    std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}
// Source images are read through owned APK descriptors; no file is materialized.
struct GuestView {
    int fd = -1;
    void* mapping = MAP_FAILED;
    size_t mapping_size = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::string asset_name;
    bool Release() {
        bool ok = true;
        if (mapping != MAP_FAILED) {
            const int status = munmap(mapping, mapping_size);
            LOGI("Guest source munmap %s: bytes=%zu result=%d", asset_name.c_str(), mapping_size, status);
            ok = status == 0;
            mapping = MAP_FAILED;
            data = nullptr;
        }
        if (fd >= 0) { ok = close(fd) == 0 && ok; fd = -1; }
        return ok;
    }
    ~GuestView() { Release(); }
    bool Open(AAssetManager* manager, const char* name) {
        const std::string path = std::string("guest_libs/") + name;
        asset_name = path;
        AAsset* asset = AAssetManager_open(manager, path.c_str(), AASSET_MODE_RANDOM);
        if (!asset) return false;
        off64_t offset = 0, length = 0;
        int asset_fd = AAsset_openFileDescriptor64(asset, &offset, &length);
        if (asset_fd >= 0) { fd = dup(asset_fd); close(asset_fd); }
        AAsset_close(asset);
        const long page = sysconf(_SC_PAGESIZE);
        struct stat st{};
        if (fd < 0 || page <= 0 || offset < 0 || length <= 0 || fstat(fd, &st) ||
            offset > st.st_size || length > st.st_size - offset) return false;
        const size_t bias = static_cast<size_t>(offset % page);
        if (static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max() - bias - static_cast<size_t>(page)) return false;
        mapping_size = (static_cast<size_t>(length) + bias + page - 1) / page * page;
        mapping = mmap(nullptr, mapping_size, PROT_READ, MAP_PRIVATE, fd, offset - bias);
        if (mapping == MAP_FAILED) return false;
        data = static_cast<const uint8_t*>(mapping) + bias;
        size = static_cast<size_t>(length);
        LOGI("Guest source %s: fd=%d offset=%lld bytes=%zu hostPage=%ld", name, fd, static_cast<long long>(offset), size, page);
        return true;
    }
};
static bool EnsureGameLoaded(JNIEnv* env) {
    FakeJniEnv& fake = FakeJniEnv::Instance();
    fake.AttachHostEnv(env);
    if (g_ready.load(std::memory_order_acquire)) {
        if (!g_host->HasFailed()) return true;
        Error(env, "Guest execution has failed"); return false;
    }
    std::lock_guard lock(g_load_mutex);
    if (g_load_failed) { Error(env, "Guest bootstrap previously failed"); return false; }
    if (g_ready.load()) return !g_host->HasFailed();
    LocalFrame frame(env);
    if (!frame.valid) return false;
    const auto fail = [&](const char* message) { g_load_failed = true; Error(env, message); return false; };
    jclass sdl = env->FindClass("org/libsdl/app/SDLActivity");
    jfieldID singleton = sdl ? env->GetStaticFieldID(sdl, "mSingleton", "Lorg/libsdl/app/SDLActivity;") : nullptr;
    jobject activity = singleton ? env->GetStaticObjectField(sdl, singleton) : nullptr;
    if (!activity || env->ExceptionCheck()) return fail("SDL activity is unavailable for guest bootstrap");
    jclass cls = env->GetObjectClass(activity);
    jmethodID get_info = env->GetMethodID(cls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jmethodID get_assets = env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject info = get_info ? env->CallObjectMethod(activity, get_info) : nullptr;
    jobject assets = get_assets ? env->CallObjectMethod(activity, get_assets) : nullptr;
    if (!info || !assets || env->ExceptionCheck()) return fail("ApplicationInfo or AssetManager is unavailable");
    jclass info_class = env->GetObjectClass(info);
    jfieldID source_dir = env->GetFieldID(info_class, "sourceDir", "Ljava/lang/String;");
    jfieldID split_dirs = env->GetFieldID(info_class, "splitSourceDirs", "[Ljava/lang/String;");
    if (!source_dir || !split_dirs) return fail("APK source metadata is unavailable");
    std::vector<std::string> paths{JavaString(env, static_cast<jstring>(env->GetObjectField(info, source_dir)))};
    auto splits = static_cast<jobjectArray>(env->GetObjectField(info, split_dirs));
    if (splits) for (jsize i = 0; i < env->GetArrayLength(splits); ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(splits, i));
        paths.push_back(JavaString(env, value)); env->DeleteLocalRef(value);
    }
    if (env->ExceptionCheck() || paths[0].empty()) return fail("APK source paths are invalid");
    std::vector<const char*> names;
    for (const auto& path : paths) names.push_back(path.c_str());
    AAssetManager* manager = AAssetManager_fromJava(env, assets);
    if (!manager) return fail("Host AssetManager initialization failed");
    if (IndexApks(names.data(), names.size())) return fail("Direct APK index initialization failed");
    g_host = std::make_unique<DynarmicHost>();
    if (!g_host->Initialize()) return fail("Guest heap/runtime self-check failed");
    LOGI("Guest arena: host=%p bytes=%zu", static_cast<void*>(g_host->GetGuestMemoryBase()), g_host->GetGuestMemorySize());
    fake.SetJavaVM(g_jvm); fake.Init(*g_host); fake.AttachHostEnv(env);
    fake.CacheClassLoader(env, activity);
    constexpr const char* libraries[] = {"libfmodex.so", "libSDL2.so", "libGameEngine.so", "libmain.so"};
    std::array<GuestView, 4> views;
    std::vector<GuestModuleImage> modules;
    for (size_t i = 0; i < views.size(); ++i) {
        if (!views[i].Open(manager, libraries[i])) return fail("A STORED guest library cannot be mapped from assets");
        modules.push_back({libraries[i], views[i].data, views[i].size});
    }
    if (!g_host->LoadModulesFromMemory(modules)) return fail("Guest ELF group dependency/relocation validation failed");
    const bool wide = TelltaleWidescreenOverrideEnabled() != 0;
    if (!InstallWolfDisplayOverrides(*g_host, wide)) return fail("Display override guest fingerprint mismatch");
    LOGI("WOLF_DISPLAY aspect=%s output_downscale=disabled texture_scalar=1.0",
         wide ? "physical" : "16:9");
#ifdef WOLF_FMOD_DIAGNOSTIC
    if (!g_host->ConfigureFmodProbe()) return fail("FMOD diagnostic source/opcode binding failed");
#endif
    for (size_t i = 0; i < std::size(kSpecs); ++i) {
        ElfLoader* module = g_host->FindModule(kSpecs[i].library);
        g_symbols[i] = module ? module->GetSymbolAddress(kSpecs[i].symbol) : 0;
        if (!g_symbols[i]) return fail("An exact JNI guest export is missing");
    }
#ifdef WOLF_JNI_CONTRACT_CHECKS
    if (!RunWolfFakeJniArtChecks(env, *g_host)) return fail("Diagnostic ART contracts failed");
#endif
    if (!g_host->RunModuleInitializers() || g_host->HasFailed()) return fail("Guest constructors failed");
    for (const char* library : libraries) {
        auto* module = g_host->FindModule(library);
        const uint32_t on_load = module->GetSymbolAddress("JNI_OnLoad");
        if (!on_load) continue;
        const uint32_t version = g_host->RunFunction(on_load, {fake.VmTableGuestAddr(), 0});
        LOGI("Guest %s JNI_OnLoad=0x%08x", library, version);
        if (g_host->HasFailed() || (version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6)) return fail("Guest JNI_OnLoad failed");
    }
    for (auto& view : views) if (!view.Release()) return fail("Guest source view cleanup failed");
    g_host->GetThunkManager().LogPolicyCounters("bootstrap");
    g_ready.store(true, std::memory_order_release);
    LOGI("Guest group ready: exact25 JNI exports, four direct source views released after bootstrap");
    return true;
}
static uint32_t ObjectArg(JNIEnv* env, jobject object) {
    auto& fake = FakeJniEnv::Instance(); fake.AttachHostEnv(env); return fake.AddObjectRef(object);
}
static uint32_t FloatArg(jfloat value) { uint32_t bits; memcpy(&bits, &value, sizeof(bits)); return bits; }
static uint32_t Call(JNIEnv* env, jobject object, size_t index, std::vector<uint32_t> args = {}) {
    auto& fake = FakeJniEnv::Instance();
    args.insert(args.begin(), ObjectArg(env, object));
    args.insert(args.begin(), fake.EnvTableGuestAddr());
    const auto& spec = kSpecs[index];
    const uint32_t registered = fake.FindRegisteredNative(spec.cls, spec.method, spec.signature);
    const uint32_t result = g_host->RunFunction(registered ? registered : g_symbols[index], args);
    if (g_host->HasExited() && !g_host->HasFailed()) {
        g_host->GetThunkManager().LogPolicyCounters("normal_exit");
        LOGI("Guest requested normal process exit");
        fflush(nullptr);
        _exit(0);
    }
    if (g_host->HasFailed()) Error(env, "Guest native call halted abnormally");
    return result;
}
static void Native_nativeOnPurchaseComplete(JNIEnv* env, jclass object, jstring arg0, jboolean arg1, jstring arg2) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 0, {ObjectArg(env, arg0), static_cast<uint32_t>(arg1), ObjectArg(env, arg2)});
}
static jint Native_fmodGetInfo(JNIEnv* env, jobject object, jint arg0) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    const jint result = static_cast<jint>(Call(env, object, 1, {static_cast<uint32_t>(arg0)}));
    return result;
}
static jint Native_fmodProcess(JNIEnv* env, jobject object, jobject arg0) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    std::lock_guard buffer_lock(g_fmod_buffer_mutex);
    auto& fake = FakeJniEnv::Instance(); const uint32_t buffer = fake.RegisterDirectBuffer(arg0);
    if (!buffer) { Error(env, "FMOD direct buffer shadow is unavailable"); return 0; }
    if (g_fmod_buffer && g_fmod_buffer != buffer) fake.ReleaseDirectBuffer(g_fmod_buffer);
    g_fmod_buffer = buffer;
    const jint result = static_cast<jint>(Call(env, object, 2, {buffer}));
    fake.CommitDirectBuffer(buffer);
    return result;
}
static jint Native_nativeAddJoystick(JNIEnv* env, jclass object, jint arg0, jstring arg1, jstring arg2, jint arg3, jint arg4, jint arg5, jint arg6, jint arg7) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    const jint result = static_cast<jint>(Call(env, object, 3, {static_cast<uint32_t>(arg0), ObjectArg(env, arg1), ObjectArg(env, arg2), static_cast<uint32_t>(arg3), static_cast<uint32_t>(arg4), static_cast<uint32_t>(arg5), static_cast<uint32_t>(arg6), static_cast<uint32_t>(arg7)}));
    return result;
}
static void Native_nativeFlipBuffers(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 4, {});
}
static void Native_nativeInit(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 5, {});
    ShutdownOpenSLES(g_host.get());
}
static void Native_nativeLowMemory(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 6, {});
}
static void Native_nativePause(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 7, {});
    PauseAudioPlayback(g_host.get());
    g_host->GetThunkManager().LogPolicyCounters("pause");
}
static void Native_nativeQuit(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 8, {});
    g_host->GetThunkManager().LogPolicyCounters("quit");
}
static jint Native_nativeRemoveJoystick(JNIEnv* env, jclass object, jint arg0) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    const jint result = static_cast<jint>(Call(env, object, 9, {static_cast<uint32_t>(arg0)}));
    return result;
}
static void Native_nativeResume(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    ResumeAudioPlayback(g_host.get());
    Call(env, object, 10, {});
}
static void Native_onNativeAccel(JNIEnv* env, jclass object, jfloat arg0, jfloat arg1, jfloat arg2) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 11, {FloatArg(arg0), FloatArg(arg1), FloatArg(arg2)});
}
static void Native_onNativeHat(JNIEnv* env, jclass object, jint arg0, jint arg1, jint arg2, jint arg3) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 12, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1), static_cast<uint32_t>(arg2), static_cast<uint32_t>(arg3)});
}
static void Native_onNativeJoy(JNIEnv* env, jclass object, jint arg0, jint arg1, jfloat arg2) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 13, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1), FloatArg(arg2)});
}
static void Native_onNativeKeyDown(JNIEnv* env, jclass object, jint arg0) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 14, {static_cast<uint32_t>(arg0)});
}
static void Native_onNativeKeyUp(JNIEnv* env, jclass object, jint arg0) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 15, {static_cast<uint32_t>(arg0)});
}
static void Native_onNativeKeyboardFocusLost(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 16, {});
}
static jint Native_onNativePadDown(JNIEnv* env, jclass object, jint arg0, jint arg1) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    const jint result = static_cast<jint>(Call(env, object, 17, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1)}));
    return result;
}
static jint Native_onNativePadUp(JNIEnv* env, jclass object, jint arg0, jint arg1) {
    if (!EnsureGameLoaded(env)) return 0;
    LocalFrame frame(env); if (!frame.valid) return 0;
    NativeScope scope;
    const jint result = static_cast<jint>(Call(env, object, 18, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1)}));
    return result;
}
static void Native_onNativeResize(JNIEnv* env, jclass object, jint arg0, jint arg1, jint arg2) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    ThunkManager::s_screenWidth = static_cast<uint32_t>(arg0); ThunkManager::s_screenHeight = static_cast<uint32_t>(arg1);
    Call(env, object, 19, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1), static_cast<uint32_t>(arg2)});
}
static void Native_onNativeSurfaceChanged(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 20, {});
}
static void Native_onNativeSurfaceDestroyed(JNIEnv* env, jclass object) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 21, {});
}
static void Native_onNativeTouch(JNIEnv* env, jclass object, jint arg0, jint arg1, jint arg2, jfloat arg3, jfloat arg4, jfloat arg5) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 22, {static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1), static_cast<uint32_t>(arg2), FloatArg(arg3), FloatArg(arg4), FloatArg(arg5)});
}
static void Native_nativeCommitText(JNIEnv* env, jobject object, jstring arg0, jint arg1) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 23, {ObjectArg(env, arg0), static_cast<uint32_t>(arg1)});
}
static void Native_nativeSetComposingText(JNIEnv* env, jobject object, jstring arg0, jint arg1) {
    if (!EnsureGameLoaded(env)) return;
    LocalFrame frame(env); if (!frame.valid) return;
    NativeScope scope;
    Call(env, object, 24, {ObjectArg(env, arg0), static_cast<uint32_t>(arg1)});
}
static const JNINativeMethod kTable0[] = {
    {const_cast<char*>("nativeOnPurchaseComplete"), const_cast<char*>("(Ljava/lang/String;ZLjava/lang/String;)V"), reinterpret_cast<void*>(Native_nativeOnPurchaseComplete)},
};
static const JNINativeMethod kTable1[] = {
    {const_cast<char*>("fmodGetInfo"), const_cast<char*>("(I)I"), reinterpret_cast<void*>(Native_fmodGetInfo)},
    {const_cast<char*>("fmodProcess"), const_cast<char*>("(Ljava/nio/ByteBuffer;)I"), reinterpret_cast<void*>(Native_fmodProcess)},
};
static const JNINativeMethod kTable2[] = {
    {const_cast<char*>("nativeAddJoystick"), const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;IIIII)I"), reinterpret_cast<void*>(Native_nativeAddJoystick)},
    {const_cast<char*>("nativeFlipBuffers"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativeFlipBuffers)},
    {const_cast<char*>("nativeInit"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativeInit)},
    {const_cast<char*>("nativeLowMemory"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativeLowMemory)},
    {const_cast<char*>("nativePause"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativePause)},
    {const_cast<char*>("nativeQuit"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativeQuit)},
    {const_cast<char*>("nativeRemoveJoystick"), const_cast<char*>("(I)I"), reinterpret_cast<void*>(Native_nativeRemoveJoystick)},
    {const_cast<char*>("nativeResume"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_nativeResume)},
    {const_cast<char*>("onNativeAccel"), const_cast<char*>("(FFF)V"), reinterpret_cast<void*>(Native_onNativeAccel)},
    {const_cast<char*>("onNativeHat"), const_cast<char*>("(IIII)V"), reinterpret_cast<void*>(Native_onNativeHat)},
    {const_cast<char*>("onNativeJoy"), const_cast<char*>("(IIF)V"), reinterpret_cast<void*>(Native_onNativeJoy)},
    {const_cast<char*>("onNativeKeyDown"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(Native_onNativeKeyDown)},
    {const_cast<char*>("onNativeKeyUp"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(Native_onNativeKeyUp)},
    {const_cast<char*>("onNativeKeyboardFocusLost"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_onNativeKeyboardFocusLost)},
    {const_cast<char*>("onNativePadDown"), const_cast<char*>("(II)I"), reinterpret_cast<void*>(Native_onNativePadDown)},
    {const_cast<char*>("onNativePadUp"), const_cast<char*>("(II)I"), reinterpret_cast<void*>(Native_onNativePadUp)},
    {const_cast<char*>("onNativeResize"), const_cast<char*>("(III)V"), reinterpret_cast<void*>(Native_onNativeResize)},
    {const_cast<char*>("onNativeSurfaceChanged"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_onNativeSurfaceChanged)},
    {const_cast<char*>("onNativeSurfaceDestroyed"), const_cast<char*>("()V"), reinterpret_cast<void*>(Native_onNativeSurfaceDestroyed)},
    {const_cast<char*>("onNativeTouch"), const_cast<char*>("(IIIFFF)V"), reinterpret_cast<void*>(Native_onNativeTouch)},
};
static const JNINativeMethod kTable3[] = {
    {const_cast<char*>("nativeCommitText"), const_cast<char*>("(Ljava/lang/String;I)V"), reinterpret_cast<void*>(Native_nativeCommitText)},
    {const_cast<char*>("nativeSetComposingText"), const_cast<char*>("(Ljava/lang/String;I)V"), reinterpret_cast<void*>(Native_nativeSetComposingText)},
};

extern "C" JNIEXPORT void JNICALL
Java_com_telltalegames_fables100_DisplayCadence_nativeSetFrameLimit(
        JNIEnv*, jclass, jint cap, jfloat actual_hz) {
    SetBridgeFrameLimit(cap, actual_hz);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
#if WOLF_RENDERDOC_GLES_LAYER
    wolf_renderdoc_gles::Initialize();
#endif
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    FakeJniEnv::Instance().SetJavaVM(vm);
    FakeJniEnv::Instance().AttachHostEnv(env);
    { jclass cls = env->FindClass("com/telltalegames/telltale/TelltaleActivity");
      if (!cls || env->RegisterNatives(cls, kTable0, std::size(kTable0)) != JNI_OK) { Error(env, "Exact JNI registration failed"); return JNI_ERR; }
      env->DeleteLocalRef(cls); }
    { jclass cls = env->FindClass("org/fmod/FMODAudioDevice");
      if (!cls || env->RegisterNatives(cls, kTable1, std::size(kTable1)) != JNI_OK) { Error(env, "Exact JNI registration failed"); return JNI_ERR; }
      env->DeleteLocalRef(cls); }
    { jclass cls = env->FindClass("org/libsdl/app/SDLActivity");
      if (!cls || env->RegisterNatives(cls, kTable2, std::size(kTable2)) != JNI_OK) { Error(env, "Exact JNI registration failed"); return JNI_ERR; }
      env->DeleteLocalRef(cls); }
    { jclass cls = env->FindClass("org/libsdl/app/SDLInputConnection");
      if (!cls || env->RegisterNatives(cls, kTable3, std::size(kTable3)) != JNI_OK) { Error(env, "Exact JNI registration failed"); return JNI_ERR; }
      env->DeleteLocalRef(cls); }
    LOGI("JNI_OnLoad: ELF64 host registered exact25 Wolf JNI methods");
    return JNI_VERSION_1_6;
}
