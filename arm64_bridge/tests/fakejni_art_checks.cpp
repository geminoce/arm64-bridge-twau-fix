// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakejni_art_checks.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "jni_fake_env.h"
#include <android/log.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr const char* tag = "WolfJNIContract";
struct GuestBlocks {
    DynarmicHost& host;
    std::vector<uint32_t> blocks;
    ~GuestBlocks() { for (uint32_t address : blocks) host.GuestFree(address); }
    uint32_t Allocate(size_t size, size_t alignment = 8) {
        const uint32_t address = host.GuestAlloc(size, alignment);
        if (address) blocks.push_back(address);
        return address;
    }
    uint32_t String(const char* text) {
        const size_t size = strlen(text) + 1;
        const uint32_t address = Allocate(size);
        if (address) memcpy(host.GetGuestMemoryBase() + address, text, size);
        return address;
    }
};
struct NativeScope {
    FakeJniEnv& fake;
    explicit NativeScope(FakeJniEnv& value) : fake(value) { fake.BeginNativeCall(); }
    ~NativeScope() { fake.EndNativeCall(); }
};
struct HostFrame {
    JNIEnv* env;
    bool valid;
    explicit HostFrame(JNIEnv* value) : env(value), valid(value->PushLocalFrame(128) == JNI_OK) {}
    ~HostFrame() { if (valid) env->PopLocalFrame(nullptr); }
};
bool Fail(const char* case_name) {
    __android_log_print(ANDROID_LOG_ERROR, tag, "FAIL %s", case_name);
    return false;
}
}

bool RunWolfFakeJniArtChecks(JNIEnv* env, DynarmicHost& host) {
    HostFrame frame(env);
    if (!frame.valid) return Fail("host local frame");
    auto& fake = FakeJniEnv::Instance();
    fake.AttachHostEnv(env);
    NativeScope scope(fake);
    GuestBlocks memory{host, {}};
    const auto call = [&](bool vm, uint32_t offset, std::vector<uint32_t> args) {
        const uint32_t table = vm ? fake.VmTableGuestAddr() : fake.EnvTableGuestAddr();
        args.insert(args.begin(), table);
        return host.RunFunction(host.MemoryRead32(table + offset), args);
    };
    const auto env_call = [&](uint32_t offset, std::vector<uint32_t> args) {
        return call(false, offset, std::move(args));
    };

    const uint32_t vm_out = memory.Allocate(8);
    if (!vm_out || static_cast<jint>(call(true, 24, {vm_out, JNI_VERSION_1_4})) != JNI_OK ||
        host.MemoryRead32(vm_out) != fake.EnvTableGuestAddr()) return Fail("VM GetEnv 1.4");
    if (static_cast<jint>(call(true, 24, {vm_out, 0x7fffffff})) != JNI_EVERSION ||
        host.MemoryRead32(vm_out) != 0) return Fail("VM invalid-version output");
    if (static_cast<jint>(env_call(876, {vm_out})) != JNI_OK ||
        host.MemoryRead32(vm_out) != fake.VmTableGuestAddr()) return Fail("GetJavaVM output");

    jclass byte_buffer = env->FindClass("java/nio/ByteBuffer");
    jmethodID allocate_direct = byte_buffer ? env->GetStaticMethodID(byte_buffer, "allocateDirect", "(I)Ljava/nio/ByteBuffer;") : nullptr;
    jmethodID allocate_heap = byte_buffer ? env->GetStaticMethodID(byte_buffer, "allocate", "(I)Ljava/nio/ByteBuffer;") : nullptr;
    jobject direct = allocate_direct ? env->CallStaticObjectMethod(byte_buffer, allocate_direct, 16) : nullptr;
    jobject heap = allocate_heap ? env->CallStaticObjectMethod(byte_buffer, allocate_heap, 16) : nullptr;
    if (!direct || !heap || env->ExceptionCheck()) return Fail("Java buffer allocation");
    auto* host_bytes = static_cast<uint8_t*>(env->GetDirectBufferAddress(direct));
    if (!host_bytes) return Fail("host direct address");
    for (size_t i = 0; i < 16; ++i) host_bytes[i] = static_cast<uint8_t>(0x30 + i);
    const uint32_t canonical = fake.AddObjectRef(direct);
    const uint32_t buffer = fake.RegisterDirectBuffer(direct);
    if (!canonical || buffer != canonical || fake.RegisterDirectBuffer(direct) != buffer)
        return Fail("buffer/object canonical interning");
    jobject local = fake.ResolveObject(buffer);
    if (!local || env->GetObjectRefType(local) != JNILocalRefType || !env->IsSameObject(local, direct))
        return Fail("ResolveObject local-ref ownership");
    env->DeleteLocalRef(local);
    const uint32_t scratch = env_call(920, {buffer});
    if (!scratch || memcmp(host.GetGuestMemoryBase() + scratch, host_bytes, 16)) return Fail("shadow initial copy");
    if (env_call(924, {buffer}) != 16 || host.GetResultHigh() != 0) return Fail("capacity positive r0:r1");
    const uint32_t heap_handle = fake.AddObjectRef(heap);
    if (env_call(924, {heap_handle}) != UINT32_MAX || host.GetResultHigh() != UINT32_MAX)
        return Fail("non-direct capacity minus-one r0:r1");
    for (size_t i = 0; i < 16; ++i) host.GetGuestMemoryBase()[scratch + i] = static_cast<uint8_t>(0x60 + i);
    fake.CommitDirectBuffer(buffer);
    for (size_t i = 0; i < 16; ++i) if (host_bytes[i] != static_cast<uint8_t>(0x60 + i)) return Fail("PCM commit");
    jbyteArray bytes = env->NewByteArray(16);
    jmethodID get = env->GetMethodID(byte_buffer, "get", "([B)Ljava/nio/ByteBuffer;");
    if (!bytes || !get) return Fail("Java PCM reader creation");
    jobject read_result = env->CallObjectMethod(direct, get, bytes);
    std::array<jbyte, 16> actual{};
    env->GetByteArrayRegion(bytes, 0, 16, actual.data());
    if (env->ExceptionCheck() || !read_result) return Fail("Java ByteBuffer.get after commit");
    for (size_t i = 0; i < 16; ++i) if (static_cast<uint8_t>(actual[i]) != static_cast<uint8_t>(0x60 + i))
        return Fail("Java PCM bytes after commit");
    fake.ReleaseDirectBuffer(buffer);
    if (host.GuestAllocSize(scratch) != 0 || fake.DirectBufferCapacity(buffer) != -1)
        return Fail("buffer shadow release");
    if (fake.RegisterDirectBuffer(direct) != canonical) return Fail("buffer re-registration identity");
    fake.ReleaseDirectBuffer(canonical);

    const uint32_t text = memory.String("Wolf JNI lease");
    if (!text) return Fail("UTF source allocation");
    const uint32_t string = env_call(668, {text});
    const uint32_t utf1 = env_call(676, {string, 0});
    const uint32_t utf2 = env_call(676, {string, 0});
    if (!string || !utf1 || utf1 != utf2 || strcmp(reinterpret_cast<const char*>(host.GetGuestMemoryBase() + utf1), "Wolf JNI lease"))
        return Fail("UTF paired lease acquisition");
    env_call(680, {string, utf1});
    if (!host.GuestAllocSize(utf1)) return Fail("UTF first lease release kept scratch");
    env_call(680, {string, utf2});
    if (host.GuestAllocSize(utf1) != 0) return Fail("UTF last lease release freed scratch");

    jclass math = env->FindClass("java/lang/Math");
    jmethodID scalb = math ? env->GetStaticMethodID(math, "scalb", "(FI)F") : nullptr;
    const uint32_t math_handle = fake.AddObjectRef(math);
    const uint32_t method = scalb ? fake.AddMethod(scalb, true, math_handle, "scalb", "(FI)F") : 0;
    if (!method) return Fail("Math.scalb lookup");
    // float 1.25 promotes to double 0x3ff4000000000000. First wide vararg
    // skips r3, so a poison r3 must not be consumed as its low word.
    constexpr uint32_t ten_float_bits = 0x41200000;
    if (env_call(540, {math_handle, method, 0xdeadbeef, 0, 0x3ff40000, 3}) != ten_float_bits)
        return Fail("promoted float vararg alignment");
    const uint32_t va = memory.Allocate(16);
    const uint32_t values = memory.Allocate(16);
    if (!va || !values) return Fail("floating argument storage");
    host.MemoryWrite32(va, 0); host.MemoryWrite32(va + 4, 0x3ff40000); host.MemoryWrite32(va + 8, 3);
    host.MemoryWrite32(values, 0x3fa00000); host.MemoryWrite32(values + 4, 0);
    host.MemoryWrite32(values + 8, 3); host.MemoryWrite32(values + 12, 0);
    if (env_call(544, {math_handle, method, va}) != ten_float_bits) return Fail("promoted float V layout");
    if (env_call(548, {math_handle, method, values}) != ten_float_bits) return Fail("typed float jvalue A layout");

    // Before guest OnLoad only: use original targets, never a game hook/stub.
    constexpr const char* cls_name = "org/libsdl/app/SDLInputConnection";
    constexpr const char* first = "nativeCommitText";
    constexpr const char* second = "nativeSetComposingText";
    constexpr const char* signature = "(Ljava/lang/String;I)V";
    if (fake.FindRegisteredNative(cls_name, first, signature) || fake.FindRegisteredNative(cls_name, second, signature))
        return Fail("registry test requires pre-OnLoad empty class");
    ElfLoader* sdl = host.FindModule("libSDL2.so");
    const uint32_t target1 = sdl ? sdl->GetSymbolAddress("Java_org_libsdl_app_SDLInputConnection_nativeCommitText") : 0;
    const uint32_t target2 = sdl ? sdl->GetSymbolAddress("Java_org_libsdl_app_SDLInputConnection_nativeSetComposingText") : 0;
    jclass cls = env->FindClass(cls_name);
    const uint32_t class_handle = fake.AddObjectRef(cls);
    const uint32_t records = memory.Allocate(24);
    const uint32_t name1 = memory.String(first), name2 = memory.String(second);
    const uint32_t good_sig = memory.String(signature), bad_sig = memory.String("(Ljava/lang/String;)V");
    if (!target1 || !target2 || !class_handle || !records || !name1 || !name2 || !good_sig || !bad_sig)
        return Fail("registry original-target metadata");
    host.MemoryWrite32(records, name1); host.MemoryWrite32(records + 4, good_sig); host.MemoryWrite32(records + 8, target1);
    host.MemoryWrite32(records + 12, name2); host.MemoryWrite32(records + 16, bad_sig); host.MemoryWrite32(records + 20, target2);
    if (static_cast<jint>(env_call(860, {class_handle, records, 2})) != JNI_ERR ||
        fake.FindRegisteredNative(cls_name, first, signature) || fake.FindRegisteredNative(cls_name, second, signature))
        return Fail("RegisterNatives all-or-nothing rejection");
    host.MemoryWrite32(records + 16, good_sig);
    if (static_cast<jint>(env_call(860, {class_handle, records, 2})) != JNI_OK ||
        fake.FindRegisteredNative(cls_name, first, signature) != target1 ||
        fake.FindRegisteredNative(cls_name, second, signature) != target2) return Fail("RegisterNatives exact tuple publish");
    if (static_cast<jint>(env_call(864, {class_handle})) != JNI_OK ||
        fake.FindRegisteredNative(cls_name, first, signature) || fake.FindRegisteredNative(cls_name, second, signature))
        return Fail("UnregisterNatives class cleanup");
    if (host.HasFailed() || env->ExceptionCheck()) return Fail("clean final JNI/guest state");
    __android_log_print(ANDROID_LOG_INFO, tag,
        "PASS VM1.4/EVersion GetJavaVM canonical/local refs shadow/JavaPCM/release UTFleases floatVar/V/A registryAtomic; diagnostic-only");
    return true;
}
