// SPDX-License-Identifier: GPL-3.0-or-later
#include "jni_fake_env.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <limits>

#define LOG_TAG "Arm32Bridge_FakeJni"
#if ARM32_BRIDGE_ENABLE_LOGGING
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

// JNIEnv table slot count (through GetObjectRefType)
static thread_local JNIEnv* thread_local_env = nullptr;
static thread_local unsigned dispatch_depth = 0;
static thread_local unsigned guest_local_depth = 0;
struct NativeRefs { std::vector<uint32_t> handles; unsigned local_depth; };
static thread_local std::vector<NativeRefs> native_refs;

static const uint32_t kEnvSlots = 233;

static const uint32_t kEnvTableBytes = kEnvSlots * 4;
// JavaVM (JNIInvokeInterface) slots: 0..7
static const uint32_t kVmSlots = 8;

FakeJniEnv& FakeJniEnv::Instance() {
    static FakeJniEnv inst;
    return inst;
}

void FakeJniEnv::Init(DynarmicHost& host) {
    if (m_host == &host) return;
    if (m_host) { host.Fail("FakeJNI host replaced without joined-worker Shutdown"); return; }
    m_host = &host;
    ThunkManager& tm = host.GetThunkManager();

    // Guest memory for tables
    m_env_table = host.GuestAlloc(kEnvTableBytes, 4);
    m_vm_table = host.GuestAlloc(kVmSlots * 4, 4);

    if (!m_env_table || !m_vm_table) {
        host.Fail("FakeJNI function table allocation failed");
        return;
    }
    auto register_slot = [&](uint32_t table, uint32_t offset, bool is_vm) {
        uint32_t thunk = tm.RegisterThunk(
            std::string("jni_") + (is_vm ? "vm" : "env") + "_" + std::to_string(offset),
            [this, offset, is_vm](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                if (is_vm) DispatchVmSlot(offset, jit);
                else DispatchEnvSlot(offset, jit);
            });
        host.MemoryWrite32(table + offset, thunk);
    };

    // Register all env slots: known offsets get real handlers (via DispatchEnvSlot
    // switch); unsupported calls halt the guest instead of fabricating success.
    for (uint32_t off = 0; off < kEnvSlots; ++off) {
        register_slot(m_env_table, off * 4, false);
    }
    for (uint32_t off = 0; off < kVmSlots; ++off) {
        register_slot(m_vm_table, off * 4, true);
    }

    // JNI ABI: guest code does `ldr r, [env]; ldr r, [r, #off]; blx r` �
    // i.e. [env] must point to the function-pointer table itself. Slot 0 of
    // both tables is reserved by the spec, so make it a self-pointer.
    host.MemoryWrite32(m_env_table, m_env_table);
    host.MemoryWrite32(m_vm_table, m_vm_table);

    LOGI("Fake JNI tables: env=0x%08X (%u bytes), vm=0x%08X", m_env_table, kEnvTableBytes, m_vm_table);
}

void FakeJniEnv::AttachHostEnv(JNIEnv* env) {
    thread_local_env = env;
}

void FakeJniEnv::BeginNativeCall() {
    native_refs.push_back({{}, guest_local_depth});
}

void FakeJniEnv::EndNativeCall() {
    if (native_refs.empty()) { if (m_host) m_host->Fail("Unbalanced FakeJNI native scope"); return; }
    NativeRefs refs = std::move(native_refs.back());
    native_refs.pop_back();
    // A guest local frame cannot escape the Java native frame that owns it.
    JNIEnv* env = GetEnv();
    while (guest_local_depth > refs.local_depth) {
        if (env) env->PopLocalFrame(nullptr);
        --guest_local_depth;
    }
    for (uint32_t handle : refs.handles) PinObjectRef(handle, -1);
}

void FakeJniEnv::CacheClassLoader(JNIEnv* env, jobject context) {
    if (!env || !context || env->ExceptionCheck()) return;
    jclass cls = env->GetObjectClass(context);
    jmethodID get_loader = cls ? env->GetMethodID(cls, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = get_loader ? env->CallObjectMethod(context, get_loader) : nullptr;
    jclass type = loader ? env->GetObjectClass(loader) : nullptr;
    jmethodID load = type ? env->GetMethodID(type, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jobject global = load ? env->NewGlobalRef(loader) : nullptr;
    if (type) env->DeleteLocalRef(type);
    if (loader) env->DeleteLocalRef(loader);
    if (cls) env->DeleteLocalRef(cls);
    if (!global || env->ExceptionCheck()) { if (m_host) m_host->Fail("Application class loader capture failed"); return; }
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_class_loader) env->DeleteGlobalRef(m_class_loader);
    m_class_loader = global;
    m_load_class = load;
}

void FakeJniEnv::Shutdown() {
    // Coordinator guarantees workers and native scopes are finished first.
    JNIEnv* env = GetEnv();
    if (!env || !m_host) return;
    std::lock_guard<std::mutex> buffers(m_buffer_mutex);
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [handle, buf] : m_direct_bufs) m_host->GuestFree(buf.scratch);
    for (auto& [handle, utf] : m_jstr_utf) m_host->GuestFree(utf);
    for (auto& [handle, ref] : m_objects) env->DeleteGlobalRef(ref.global);
    if (m_identity_system) env->DeleteGlobalRef(m_identity_system);
    if (m_class_loader) env->DeleteGlobalRef(m_class_loader);
    m_host->GuestFree(m_env_table);
    m_host->GuestFree(m_vm_table);
    m_objects.clear(); m_objects_by_hash.clear(); m_lru.clear();
    m_methods.clear(); m_method_ids.clear(); m_fields.clear(); m_field_ids.clear();
    m_direct_bufs.clear(); m_jstr_utf.clear(); m_jstr_utf_leases.clear();
    m_registered_natives.clear();
    m_identity_system = nullptr; m_identity_hash = nullptr;
    m_class_loader = nullptr; m_load_class = nullptr;
    m_env_table = m_vm_table = 0;
    m_next_obj_handle = 0x20000; m_next_mid_handle = 0x30000; m_next_fid_handle = 0x40000;
    m_host = nullptr;
}

// A thread that attaches itself and then exits still attached leaves the
// runtime's Thread object behind, and a checked build aborts the process over
// it. Guest worker threads (audio, streaming, a JIT's own helpers) come and go
// and none of them knows it is talking to a bridge, so the detach hangs off a
// TLS destructor instead of being left to whoever created the thread. Only
// threads attached here get the key set - detaching one the VM owns is worse
// than leaking it.
static pthread_key_t s_detach_key;
static JavaVM* s_detach_vm = nullptr;
static std::once_flag s_detach_once;
static bool s_detach_key_valid = false;

static void DetachOnThreadExit(void*) {
    if (s_detach_vm) s_detach_vm->DetachCurrentThread();
    thread_local_env = nullptr;
}

static bool MarkBridgeAttachment(JavaVM* vm) {
    std::call_once(s_detach_once, [vm] {
        s_detach_vm = vm;
        s_detach_key_valid = pthread_key_create(&s_detach_key, DetachOnThreadExit) == 0;
    });
    return s_detach_key_valid && pthread_setspecific(s_detach_key, reinterpret_cast<void*>(1)) == 0;
}

JNIEnv* FakeJniEnv::GetEnv() const {
    if (thread_local_env) return thread_local_env;
    if (!m_vm) return nullptr;
    JNIEnv* env = nullptr;
    const jint status = m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        thread_local_env = env;
        return env;
    }
    if (status == JNI_EDETACHED && m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        thread_local_env = env;
        if (!MarkBridgeAttachment(m_vm)) {
            m_vm->DetachCurrentThread(); thread_local_env = nullptr;
            if (m_host) m_host->Fail("JNI attachment TLS allocation failed");
            return nullptr;
        }
    }
    return env;
}

// -------------------------------------------------------------
// Handles
// -------------------------------------------------------------

// Cap on live global refs. ART's own limit is 51200 and hitting it kills the
// process outright, so the table has to stay well under it and reclaim rather
// than stop caching - a "cache full, hand out a ref and forget it" branch is the
// original leak with a delay on it.
static const size_t kMaxObjectRefs = 8192;

int32_t FakeJniEnv::IdentityHash(JNIEnv* env, jobject o) {
    // Caller holds m_mutex. NewGlobalRef/IsSameObject are safe with a pending
    // exception; invoking System.identityHashCode is not.
    if (env->ExceptionCheck()) return 0;
    if (!m_identity_hash) {
        jclass local = env->FindClass("java/lang/System");
        if (!local) return 0;
        m_identity_system = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (!m_identity_system) return 0;
        m_identity_hash = env->GetStaticMethodID(m_identity_system, "identityHashCode", "(Ljava/lang/Object;)I");
        if (!m_identity_hash) return 0;
    }
    return env->CallStaticIntMethod(m_identity_system, m_identity_hash, o);
}

void FakeJniEnv::TouchRef(uint32_t handle) const {
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
    }
}

void FakeJniEnv::EvictOneRef(JNIEnv* env) {
    if (m_lru.empty()) return;  // AddObjectRef rejects a full pinned table.
    uint32_t handle = m_lru.back();
    m_lru.pop_back();

    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return;

    auto bucket = m_objects_by_hash.find(it->second.hash);
    if (bucket != m_objects_by_hash.end()) {
        auto& v = bucket->second;
        v.erase(std::remove(v.begin(), v.end(), handle), v.end());
        if (v.empty()) m_objects_by_hash.erase(bucket);
    }
    // The UTF-8 copy of a jstring lives as long as the jstring's handle does;
    // leaving it behind strands the arena block and, worse, keeps a dead handle
    // mapped to it.
    auto utf = m_jstr_utf.find(handle);
    if (utf != m_jstr_utf.end()) {
        if (m_host) m_host->GuestFree(utf->second);
        m_jstr_utf.erase(utf);
    }
    env->DeleteGlobalRef(it->second.global);
    m_objects.erase(it);
}

uint32_t FakeJniEnv::AddObjectRef(jobject o, bool pin) {
    if (!o) return 0;
    JNIEnv* env = GetEnv();
    if (!env) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    int32_t hash = IdentityHash(env, o);
    const auto borrow = [&](uint32_t handle) {
        if (pin) PinRefLocked(handle, +1);
        if (dispatch_depth == 0 && !native_refs.empty()) {
            PinRefLocked(handle, +1);
            native_refs.back().handles.push_back(handle);
        }
    };
    for (uint32_t h : m_objects_by_hash[hash]) {
        auto it = m_objects.find(h);
        if (it != m_objects.end() && env->IsSameObject(it->second.global, o)) {
            TouchRef(h);
            borrow(h);
            return h;
        }
    }
    // Eviction rehashes both maps, so nothing looked up above survives it.
    if (m_objects.size() >= kMaxObjectRefs) EvictOneRef(env);
    if (m_objects.size() >= kMaxObjectRefs || m_next_obj_handle > UINT32_MAX - 4) {
        if (m_host) m_host->Fail("FakeJNI object handle capacity exhausted");
        return 0;
    }

    jobject global = env->NewGlobalRef(o);
    if (!global) return 0;
    uint32_t h = m_next_obj_handle;
    m_next_obj_handle += 4;

    ObjRef& ref = m_objects[h];
    ref.global = global;
    ref.hash = hash;
    ref.pins = 0;
    m_lru.push_front(h);
    ref.lru = m_lru.begin();
    m_objects_by_hash[hash].push_back(h);
    borrow(h);
    return h;
}

jobject FakeJniEnv::ResolveObject(uint32_t handle) const {
    if (!handle) return nullptr;
    JNIEnv* env = GetEnv();
    if (!env) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return nullptr;
    // Resolving counts as using: an object a slot is part way through working
    // with must not be the one another thread picks to evict.
    TouchRef(handle);
    return env->NewLocalRef(it->second.global);
}

void FakeJniEnv::PinObjectRef(uint32_t handle, int delta) {
    std::lock_guard<std::mutex> lk(m_mutex);
    PinRefLocked(handle, delta);
}

void FakeJniEnv::PinRefLocked(uint32_t handle, int delta) {
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return;
    ObjRef& ref = it->second;
    const bool was = ref.pins > 0;
    if (delta > 0 && ref.pins > std::numeric_limits<int>::max() - delta) {
        if (m_host) m_host->Fail("FakeJNI object pin count overflow");
        return;
    }
    ref.pins += delta;
    if (ref.pins < 0) ref.pins = 0;
    const bool now = ref.pins > 0;
    if (now == was) return;
    if (now) {
        m_lru.erase(ref.lru);
    } else {
        m_lru.push_front(handle);
        ref.lru = m_lru.begin();
    }
}

void FakeJniEnv::DemoteObjectRef(uint32_t handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0) {
        m_lru.splice(m_lru.end(), m_lru, it->second.lru);
    }
}

// One id per jmethodID. Guests that reach Java through reflection ask for the
// same method on every call; appending unconditionally grew this table, and the
// signature string in each entry, for the life of the process.
//
// Dedup by mid alone is only correct because handles are interned: clsHandle for
// a given class is now stable, so the first registration's copy stays right.
uint32_t FakeJniEnv::AddMethod(jmethodID mid, bool isStatic, uint32_t clsHandle, const std::string& name, const std::string& sig) {
    if (!mid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_method_ids.find(mid);
    if (known != m_method_ids.end()) return known->second;

    uint32_t h = m_next_mid_handle;
    m_next_mid_handle += 4;
    MethodInfo mi;
    mi.mid = mid;
    mi.isStatic = isStatic;
    mi.clsHandle = clsHandle;
    mi.name = name;
    mi.sig = sig;
    m_methods[h] = mi;
    m_method_ids[mid] = h;
    LOGI("[JNI] method 0x%08X %s%s static=%d", h, name.c_str(), sig.c_str(), isStatic);
    return h;
}

uint32_t FakeJniEnv::AddField(jfieldID fid) {
    if (!fid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_field_ids.find(fid);
    if (known != m_field_ids.end()) return known->second;

    uint32_t h = m_next_fid_handle;
    m_next_fid_handle += 4;
    m_fields[h] = fid;
    m_field_ids[fid] = h;
    return h;
}

std::vector<char> FakeJniEnv::MethodParams(uint32_t handle) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_methods.find(handle);
    return it == m_methods.end() ? std::vector<char>{} : ParseParams(it->second.sig);
}

jfieldID FakeJniEnv::ResolveField(uint32_t handle) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_fields.find(handle);
    return it == m_fields.end() ? nullptr : it->second;
}

// -------------------------------------------------------------
// Signature parsing & guest arg reading
// -------------------------------------------------------------

std::vector<char> FakeJniEnv::ParseParams(const std::string& sig) const {
    std::vector<char> out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    for (++i; i < sig.size() && sig[i] != ')'; ++i) {
        char c = sig[i];
        if (c == 'L') {
            while (i < sig.size() && sig[i] != ';') ++i;
            if (i == sig.size()) return {};
            out.push_back('L');
        } else if (c == '[') {
            while (i < sig.size() && sig[i] == '[') ++i;
            if (i == sig.size()) return {};
            if (sig[i] == 'L') {
                while (i < sig.size() && sig[i] != ';') ++i;
                if (i == sig.size()) return {};
            }
            out.push_back('L');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// A-variant: r3 points to an array of jvalue (8 bytes each on ARM32)
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsA(Dynarmic::A32::Jit&,
                                                        const std::vector<char>& params,
                                                        uint32_t jvalue_guest) {
    std::vector<GuestArg> args;
    if (!GuestRangeValid(m_host->GetGuestMemorySize(), jvalue_guest, params.size() * 8)) {
        m_host->Fail("Invalid guest JNI jvalue array"); return args;
    }
    uint8_t* mem = m_host->GetGuestMemoryBase();
    uint32_t p = jvalue_guest;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        switch (t) {
            case 'L':
                a.obj = ResolveObject(*reinterpret_cast<uint32_t*>(mem + p));
                break;
            case 'J':
                memcpy(&a.v, mem + p, 8);
                break;
            case 'D':
                memcpy(&a.v, mem + p, 8);
                break;
            case 'F': {
                float f; memcpy(&f, mem + p, 4);
                a.v = *reinterpret_cast<uint32_t*>(mem + p);
                break;
            }
            default:
                a.v = static_cast<uint32_t>(*reinterpret_cast<uint32_t*>(mem + p));
                break;
        }
        args.push_back(a);
        p += 8;
    }
    return args;
}

// V-variant: r3 is a va_list (guest stack pointer); 64-bit slots aligned to 8
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsV(Dynarmic::A32::Jit&,
                                                        const std::vector<char>& params,
                                                        uint32_t va_list_guest) {
    std::vector<GuestArg> args;
    uint8_t* mem = m_host->GetGuestMemoryBase();
    uint32_t p = va_list_guest;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        if (t == 'J' || t == 'D' || t == 'F') {
            if (p > UINT32_MAX - 7) { m_host->Fail("JNI va_list alignment overflow"); return {}; }
            p = (p + 7) & ~7u;
            if (!GuestRangeValid(m_host->GetGuestMemorySize(), p, 8)) {
                m_host->Fail("Invalid wide JNI va_list argument"); return {};
            }
            memcpy(&a.v, mem + p, 8);
            if (t == 'F') {
                double promoted; memcpy(&promoted, &a.v, 8);
                const float value = static_cast<float>(promoted);
                uint32_t bits; memcpy(&bits, &value, 4); a.v = bits;
            }
            p += 8;
        } else {
            if (!GuestRangeValid(m_host->GetGuestMemorySize(), p, 4)) {
                m_host->Fail("Invalid JNI va_list argument"); return {};
            }
            uint32_t raw = *reinterpret_cast<uint32_t*>(mem + p);
            if (t == 'L') a.obj = ResolveObject(raw);
            else a.v = raw;
            p += 4;
        }
        args.push_back(a);
    }
    return args;
}

// Base AAPCS32 varargs begin at r3. J/D and promoted float start at an
// even word, so a first 64-bit vararg skips r3 and uses [sp+0..7].
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsVar(Dynarmic::A32::Jit& jit,
                                                       const std::vector<char>& params) {
    std::vector<GuestArg> args;
    size_t words = 3;
    for (char type : params) {
        if (type == 'J' || type == 'D' || type == 'F') words = ((words + 1) & ~size_t{1}) + 2;
        else ++words;
    }
    const size_t stack_bytes = words > 4 ? (words - 4) * 4 : 0;
    if (!GuestRangeValid(m_host->GetGuestMemorySize(), jit.Regs()[13], stack_bytes)) {
        m_host->Fail("Invalid guest JNI variadic stack"); return args;
    }
    Arm32CallArgs reader(jit, m_host->GetGuestMemoryBase(), 3);
    for (char type : params) {
        GuestArg arg;
        arg.type = type;
        if (type == 'J' || type == 'D') arg.v = reader.U64();
        else if (type == 'F') {
            const float value = static_cast<float>(reader.F64());
            uint32_t bits; memcpy(&bits, &value, sizeof(bits)); arg.v = bits;
        } else {
            const uint32_t word = reader.U32();
            if (type == 'L') arg.obj = ResolveObject(word);
            else arg.v = word;
        }
        args.push_back(arg);
    }
    return args;
}

// -------------------------------------------------------------
// Host-side call marshaling
// -------------------------------------------------------------

void FakeJniEnv::CallMethod(Dynarmic::A32::Jit& jit, uint32_t mid_handle, uint32_t obj_handle,
                            char retType, const std::vector<GuestArg>& args, bool isStatic) {
    if (m_host->HasFailed()) { jit.Regs()[0] = jit.Regs()[1] = 0; return; }
    MethodInfo mi;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto mi_it = m_methods.find(mid_handle);
        if (mi_it == m_methods.end()) {
            LOGE("CallMethod: unknown method handle 0x%08X", mid_handle);
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            return;
        }
        mi = mi_it->second; // copy; host JNI call below runs without the lock
    }
    jobject obj = ResolveObject(obj_handle);
    if (!obj) {
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        return;
    }
    {
    }

    std::vector<jvalue> jvs(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        jvalue& jv = jvs[i];
        const GuestArg& a = args[i];
        switch (a.type) {
            case 'Z': jv.z = static_cast<jboolean>(a.v); break;
            case 'B': jv.b = static_cast<jbyte>(a.v); break;
            case 'C': jv.c = static_cast<jchar>(a.v); break;
            case 'S': jv.s = static_cast<jshort>(a.v); break;
            case 'I': jv.i = static_cast<jint>(a.v); break;
            case 'J': jv.j = static_cast<jlong>(a.v); break;
            case 'F': { float f; memcpy(&f, &a.v, 4); jv.f = f; break; }
            case 'D': { double d; memcpy(&d, &a.v, 8); jv.d = d; break; }
            case 'L': jv.l = a.obj; break;
        }
    }

    JNIEnv* env = GetEnv();
    if (env->ExceptionCheck()) {
        jit.Regs()[0] = jit.Regs()[1] = 0;
        return;
    }

    switch (retType) {
        case 'V':
            if (isStatic) env->CallStaticVoidMethodA(static_cast<jclass>(obj), mi.mid, jvs.data());
            else env->CallVoidMethodA(obj, mi.mid, jvs.data());
            break;
        case 'Z': {
            jboolean r = isStatic ? env->CallStaticBooleanMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                  : env->CallBooleanMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = r ? 1 : 0;
            break;
        }
        case 'B': case 'C': case 'S': {
            jint r = isStatic ? env->CallStaticIntMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                              : env->CallIntMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'I': {
            jint r = isStatic ? env->CallStaticIntMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                              : env->CallIntMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'J': {
            jlong r = isStatic ? env->CallStaticLongMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                               : env->CallLongMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
            break;
        }
        case 'F': {
            jfloat r = isStatic ? env->CallStaticFloatMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                : env->CallFloatMethodA(obj, mi.mid, jvs.data());
            LOGI("[JNI] %s%s -> %f", mi.name.c_str(), mi.sig.c_str(), static_cast<double>(r));
            WriteSF(jit, 0, r);
            break;
        }
        case 'D': {
            jdouble r = isStatic ? env->CallStaticDoubleMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallDoubleMethodA(obj, mi.mid, jvs.data());
            WriteDF(jit, 0, r);
            break;
        }
        case 'L': {
            jobject r = isStatic ? env->CallStaticObjectMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallObjectMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = AddObjectRef(r);
            break;
        }
        default:
            jit.Regs()[0] = 0;
            break;
    }
    if (env->ExceptionCheck()) {
        LOGE("[JNI] host method threw; preserving guest-visible exception");
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
    }
}

// -------------------------------------------------------------
// Direct ByteBuffer bridging
// -------------------------------------------------------------

uint32_t FakeJniEnv::RegisterDirectBuffer(jobject buf) {
    JNIEnv* env = GetEnv();
    if (!env || !buf || !m_host || env->ExceptionCheck()) return 0;
    const uint32_t handle = AddObjectRef(buf, true);
    if (!handle) return 0;
    std::lock_guard<std::mutex> buffers(m_buffer_mutex);
    auto found = m_direct_bufs.find(handle);
    if (found != m_direct_bufs.end()) {
        PinObjectRef(handle, -1); // existing cache already owns its pin
        memcpy(m_host->GetGuestMemoryBase() + found->second.scratch, found->second.hostPtr,
               static_cast<size_t>(found->second.capacity));
        return handle;
    }
    void* pointer = env->GetDirectBufferAddress(buf);
    const jlong capacity = env->GetDirectBufferCapacity(buf);
    if (!pointer || capacity <= 0 || static_cast<uint64_t>(capacity) > UINT32_MAX ||
        m_direct_bufs.size() >= 16) { PinObjectRef(handle, -1); return 0; }
    const uint32_t scratch = m_host->GuestAlloc(static_cast<size_t>(capacity), 16);
    if (!scratch || !GuestRangeValid(m_host->GetGuestMemorySize(), scratch, static_cast<size_t>(capacity))) {
        if (scratch) m_host->GuestFree(scratch);
        PinObjectRef(handle, -1);
        return 0;
    } // canonical owner pin lives until ReleaseDirectBuffer
    memcpy(m_host->GetGuestMemoryBase() + scratch, pointer, static_cast<size_t>(capacity));
    m_direct_bufs.emplace(handle, DirectBuf{pointer, capacity, scratch});
    return handle;
}

void FakeJniEnv::CommitDirectBuffer(uint32_t handle) {
    std::lock_guard<std::mutex> buffers(m_buffer_mutex);
    auto it = m_direct_bufs.find(handle);
    if (it != m_direct_bufs.end()) {
        memcpy(it->second.hostPtr, m_host->GetGuestMemoryBase() + it->second.scratch,
               static_cast<size_t>(it->second.capacity));
    }
}

void FakeJniEnv::ReleaseDirectBuffer(uint32_t handle) {
    std::lock_guard<std::mutex> buffers(m_buffer_mutex);
    auto it = m_direct_bufs.find(handle);
    if (it == m_direct_bufs.end()) return;
    m_host->GuestFree(it->second.scratch);
    m_direct_bufs.erase(it);
    PinObjectRef(handle, -1);
    DemoteObjectRef(handle);
}

jlong FakeJniEnv::DirectBufferCapacity(uint32_t handle) const {
    std::lock_guard<std::mutex> buffers(m_buffer_mutex);
    auto it = m_direct_bufs.find(handle);
    return it == m_direct_bufs.end() ? -1 : it->second.capacity;
}

uint32_t FakeJniEnv::FindRegisteredNative(const char* cls, const char* name, const char* sig) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_registered_natives.find(std::string(cls) + '\n' + name + '\n' + sig);
    return it == m_registered_natives.end() ? 0 : it->second;
}

// -------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------

// Helpers
static const char* ReadGuestStr(DynarmicHost& host, uint32_t gaddr) {
    if (!gaddr || !GuestRangeValid(host.GetGuestMemorySize(), gaddr, 1)) {
        host.Fail("Invalid guest JNI string pointer"); return "";
    }
    const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + gaddr);
    const size_t available = std::min<size_t>(host.GetGuestMemorySize() - gaddr, 1024 * 1024);
    if (!memchr(text, 0, available)) { host.Fail("Unterminated guest JNI string"); return ""; }
    return text;
}

static std::string NativeClassName(JNIEnv* env, jclass cls) {
    jclass type = env->FindClass("java/lang/Class");
    if (!type || !env->IsInstanceOf(cls, type)) return {};
    jmethodID get_name = env->GetMethodID(type, "getName", "()Ljava/lang/String;");
    auto name = get_name ? static_cast<jstring>(env->CallObjectMethod(cls, get_name)) : nullptr;
    const char* chars = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    std::string text = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(name, chars);
    std::replace(text.begin(), text.end(), '.', '/');
    return text;
}

void FakeJniEnv::DispatchVmSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    const uint32_t out = jit.Regs()[1];
    if (offset == 16 || offset == 24 || offset == 28) {
        if (!m_vm || !GuestRangeValid(m_host->GetGuestMemorySize(), out, 4)) {
            m_host->Fail("Invalid FakeJNI VM output pointer");
            jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
            return;
        }
        m_host->MemoryWrite32(out, 0);
        JNIEnv* env = nullptr;
        if (offset == 24) {
            const jint version = static_cast<jint>(jit.Regs()[2]);
            if (version != JNI_VERSION_1_1 && version != JNI_VERSION_1_2 &&
                version != JNI_VERSION_1_4 && version != JNI_VERSION_1_6) {
                jit.Regs()[0] = static_cast<uint32_t>(JNI_EVERSION); return;
            }
            const jint status = m_vm->GetEnv(reinterpret_cast<void**>(&env), version);
            if (status == JNI_OK) { AttachHostEnv(env); m_host->MemoryWrite32(out, m_env_table); }
            jit.Regs()[0] = static_cast<uint32_t>(status);
            return;
        }
        if (jit.Regs()[2]) {
            m_host->Fail("Non-null guest JavaVMAttachArgs is unsupported");
            jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR); return;
        }
        const jint status = m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            const jint attached = offset == 28 ? m_vm->AttachCurrentThreadAsDaemon(&env, nullptr)
                                               : m_vm->AttachCurrentThread(&env, nullptr);
            if (attached != JNI_OK) { jit.Regs()[0] = static_cast<uint32_t>(attached); return; }
            // GetEnv initializes the bridge's detach TLS key lazily. The real
            // VM owns Java entry threads; only this new attachment is ours.
            AttachHostEnv(nullptr);
            if (!MarkBridgeAttachment(m_vm)) {
                m_vm->DetachCurrentThread();
                m_host->Fail("Guest VM attachment TLS allocation failed");
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR); return;
            }
        } else if (status != JNI_OK) { jit.Regs()[0] = static_cast<uint32_t>(status); return; }
        AttachHostEnv(env);
        m_host->MemoryWrite32(out, m_env_table);
        jit.Regs()[0] = JNI_OK;
        return;
    }
    if (offset == 20) {
        // Never detach a Java-owned thread while a Java native frame is live.
        if (s_detach_vm && s_detach_key_valid && pthread_getspecific(s_detach_key)) {
            jit.Regs()[0] = static_cast<uint32_t>(m_vm->DetachCurrentThread());
            pthread_setspecific(s_detach_key, nullptr); thread_local_env = nullptr;
        } else jit.Regs()[0] = JNI_OK; // virtual guest detach of a Java-owned thread
        return;
    }
    LOGE("[JNI-VM] unsupported slot %u", offset / 4);
    m_host->Fail("Unsupported guest JavaVM operation");
    jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
}

void FakeJniEnv::DispatchEnvSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    JNIEnv* e = GetEnv();
    uint8_t* mem = m_host->GetGuestMemoryBase();

    if (!e) {
        LOGE("[JNI] no host env attached (slot %u)", offset / 4);
        jit.Regs()[0] = 0;
        m_host->Fail("JNI callback has no attached host environment");
        return;
    }

    // Every emulated JNI callback owns its host temporaries. Guest explicit
    // Push/PopLocalFrame remain separate so their frames cannot be popped by
    // this guard. ResolveObject creates locals under the object-table lock.
    const bool temporary_frame = offset != 76 && offset != 80;
    if (temporary_frame && e->PushLocalFrame(64) != JNI_OK) {
        m_host->Fail("JNI callback local frame allocation failed"); jit.Regs()[0] = 0; return;
    }
    ++dispatch_depth;
    struct SlotFrame {
        JNIEnv* env; bool pop;
        ~SlotFrame() { if (pop) env->PopLocalFrame(nullptr); --dispatch_depth; }
    } frame{e, temporary_frame};
    if (e->ExceptionCheck() && offset != 60 && offset != 64 && offset != 68 && offset != 72 &&
        offset != 76 && offset != 80 && offset != 88 && offset != 92 && offset != 664 &&
        offset != 680 && offset != 876 && offset != 912) {
        jit.Regs()[0] = jit.Regs()[1] = 0; return;
    }

    switch (offset) {
        case 16: // GetVersion
            jit.Regs()[0] = JNI_VERSION_1_6; break;
        // ---- class / object basics ----
        case 24: { // FindClass
            const char* name = ReadGuestStr(*m_host, jit.Regs()[1]);
            jclass c = e->FindClass(name);
            if (!c) {
                jobject loader = nullptr; jmethodID load = nullptr;
                { std::lock_guard<std::mutex> lk(m_mutex);
                  if (m_class_loader) loader = e->NewLocalRef(m_class_loader);
                  load = m_load_class; }
                if (loader && load && name) {
                    e->ExceptionClear();
                    std::string dotted(name); std::replace(dotted.begin(), dotted.end(), '/', '.');
                    jstring text = e->NewStringUTF(dotted.c_str());
                    c = text ? static_cast<jclass>(e->CallObjectMethod(loader, load, text)) : nullptr;
                }
            }
            if (!c) { jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddObjectRef(c);
            break;
        }
        case 40: { // GetSuperclass
            jobject o = ResolveObject(jit.Regs()[1]);
            jclass c = o ? e->GetSuperclass(static_cast<jclass>(o)) : nullptr;
            jit.Regs()[0] = c ? AddObjectRef(c) : 0;
            break;
        }
        // Handles are shared, so dropping the host ref in DeleteGlobalRef/
        // DeleteLocalRef would invalidate any copy the guest still holds. What
        // the three calls do instead is set how willing the table is to reclaim
        // the entry - which is as close to the real lifetime as a shared handle
        // gets, and it is what keeps the table off ART's 51200 limit.
        case 76: { // PushLocalFrame
            const jint result = e->PushLocalFrame(static_cast<jint>(jit.Regs()[1]));
            if (result == JNI_OK) ++guest_local_depth;
            jit.Regs()[0] = static_cast<uint32_t>(result); break;
        }
        case 80: { // PopLocalFrame
            if (!guest_local_depth) { m_host->Fail("Unbalanced guest JNI local frame"); jit.Regs()[0] = 0; break; }
            jobject result = e->PopLocalFrame(ResolveObject(jit.Regs()[1]));
            --guest_local_depth;
            jit.Regs()[0] = result ? AddObjectRef(result) : 0;
            if (result) e->DeleteLocalRef(result);
            break;
        }
        case 84: { // NewGlobalRef -> same handle, plus a pin
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; }
            PinObjectRef(jit.Regs()[1], +1);
            jit.Regs()[0] = jit.Regs()[1];
            break;
        }
        case 88: // DeleteGlobalRef -> drop the pin
            PinObjectRef(jit.Regs()[1], -1);
            jit.Regs()[0] = 0;
            break;
        case 92: // DeleteLocalRef -> next in line to be evicted
            DemoteObjectRef(jit.Regs()[1]);
            jit.Regs()[0] = 0;
            break;
        case 100: { // NewLocalRef
            jobject o = ResolveObject(jit.Regs()[1]);
            jit.Regs()[0] = AddObjectRef(o);
            break;
        }
        case 124: { // GetObjectClass
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; } // guest expects null, ART would abort
            jclass c = e->GetObjectClass(o);
            jit.Regs()[0] = c ? AddObjectRef(c) : 0;
            break;
        }
        case 128: { // IsInstanceOf
            jobject o = ResolveObject(jit.Regs()[1]);
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[2]));
            jit.Regs()[0] = c && e->IsInstanceOf(o, c) ? 1 : 0;
            break;
        }

        // ---- method/field IDs ----
        case 132: { // GetMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, false, jit.Regs()[1], name, sig);
            }
            break;
        }
        case 376: { // GetFieldID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            jfieldID fid = c ? e->GetFieldID(c, name, sig) : nullptr;
            if (!fid) {
                e->ExceptionClear();
                LOGE("[JNI] GetFieldID failed: %s%s", name, sig);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddField(fid);
            }
            break;
        }
        case 576: { // GetStaticFieldID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            jfieldID fid = c ? e->GetStaticFieldID(c, name, sig) : nullptr;
            if (!fid) { e->ExceptionClear(); LOGE("[JNI] GetStaticFieldID failed: %s%s", name, sig); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddField(fid);
            break;
        }
        case 580: { // GetStaticObjectField
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const jfieldID field = ResolveField(jit.Regs()[2]);
            jobject value = (c && field != nullptr) ? e->GetStaticObjectField(c, field) : nullptr;
            jit.Regs()[0] = value ? AddObjectRef(value) : 0;
            break;
        }
        case 600: { // GetStaticIntField
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const jfieldID field = ResolveField(jit.Regs()[2]);
            jit.Regs()[0] = (c && field != nullptr) ? static_cast<uint32_t>(e->GetStaticIntField(c, field)) : 0;
            break;
        }
        case 452: { // GetStaticMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetStaticMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetStaticMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, true, jit.Regs()[1], name, sig);
            }
            break;
        }

        // ---- object creation ----
        case 112: // NewObject (varargs)
        case 116: // NewObjectV
        case 120: { // NewObjectA
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            uint32_t mid_handle = jit.Regs()[2];
            MethodInfo mi;
            { std::lock_guard<std::mutex> lk(m_mutex);
              auto it = m_methods.find(mid_handle);
              if (!c || it == m_methods.end()) { jit.Regs()[0] = 0; break; }
              mi = it->second; }
            std::vector<char> params = ParseParams(mi.sig);
            std::vector<GuestArg> args = offset == 112 ? ReadArgsVar(jit, params)
                                           : offset == 116 ? ReadArgsV(jit, params, jit.Regs()[3])
                                                           : ReadArgsA(jit, params, jit.Regs()[3]);
            if (m_host->HasFailed()) { jit.Regs()[0] = 0; break; }
            // Build jvalues and call NewObjectA
            std::vector<jvalue> jvs(args.size());
            for (size_t i = 0; i < args.size(); ++i) {
                const GuestArg& a = args[i];
                switch (a.type) {
                    case 'Z': jvs[i].z = static_cast<jboolean>(a.v); break;
                    case 'I': jvs[i].i = static_cast<jint>(a.v); break;
                    case 'J': jvs[i].j = static_cast<jlong>(a.v); break;
                    case 'F': { float f; memcpy(&f, &a.v, 4); jvs[i].f = f; break; }
                    case 'L': jvs[i].l = a.obj; break;
                    default: jvs[i].i = static_cast<jint>(a.v); break;
                }
            }
            jobject o = e->NewObjectA(c, mi.mid, jvs.data());
            if (!o) { e->ExceptionClear(); LOGE("[JNI] NewObject failed"); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddObjectRef(o);
            break;
        }

        // ---- instance method calls (V and A variants) ----
        case 136: { // CallObjectMethod
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsVar(jit, params), false);
            break;
        }
        case 140: { // CallObjectMethodV
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsV(jit, params, jit.Regs()[3]), false);
            break;
        }
        case 144: { // CallObjectMethodA
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsA(jit, params, jit.Regs()[3]), false);
            break;
        }
        case 148: case 152: case 156: { // CallBooleanMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 148) ? ReadArgsVar(jit, params)
                                        : (offset == 152) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, false);
            break;
        }
        case 196: case 200: case 204: { // CallIntMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 196) ? ReadArgsVar(jit, params)
                                        : (offset == 200) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, false);
            break;
        }
        case 208: case 212: case 216: { // CallLongMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 208) ? ReadArgsVar(jit, params)
                                        : (offset == 212) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, false);
            break;
        }
        case 244: case 248: case 252: { // CallVoidMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 244) ? ReadArgsVar(jit, params)
                                        : (offset == 248) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, false);
            break;
        }

        // ---- static method calls ----
        case 456: case 460: case 464: { // CallStaticObjectMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 456) ? ReadArgsVar(jit, params)
                                        : (offset == 460) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', args, true);
            break;
        }
        case 468: case 472: case 476: { // CallStaticBooleanMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 468) ? ReadArgsVar(jit, params)
                                        : (offset == 472) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, true);
            break;
        }
        case 516: case 520: case 524: { // CallStaticIntMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 516) ? ReadArgsVar(jit, params)
                                        : (offset == 520) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, true);
            break;
        }
        case 528: case 532: case 536: { // CallStaticLongMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 528) ? ReadArgsVar(jit, params)
                                        : (offset == 532) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, true);
            break;
        }
        case 540: case 544: case 548: { // CallStaticFloatMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 540) ? ReadArgsVar(jit, params)
                                        : (offset == 544) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'F', args, true);
            break;
        }
        case 552: case 556: case 560: { // CallStaticDoubleMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 552) ? ReadArgsVar(jit, params)
                                        : (offset == 556) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'D', args, true);
            break;
        }
        case 564: case 568: case 572: { // CallStaticVoidMethod / V / A
            std::vector<char> params = MethodParams(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 564) ? ReadArgsVar(jit, params)
                                        : (offset == 568) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, true);
            break;
        }

        // ---- field access ----
        case 400: { // GetIntField
            jobject o = ResolveObject(jit.Regs()[1]);
            const jfieldID field = ResolveField(jit.Regs()[2]);
            if (!o || field == nullptr) { jit.Regs()[0] = 0; break; }
            jit.Regs()[0] = static_cast<uint32_t>(e->GetIntField(o, field));
            break;
        }
        case 380: { // GetObjectField
            jobject o = ResolveObject(jit.Regs()[1]);
            const jfieldID field = ResolveField(jit.Regs()[2]);
            if (!o || field == nullptr) { jit.Regs()[0] = 0; break; }
            jobject r = e->GetObjectField(o, field);
            jit.Regs()[0] = r ? AddObjectRef(r) : 0;
            break;
        }

        // ---- arrays ----
        case 684: { // GetArrayLength
            jarray array = static_cast<jarray>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = array ? static_cast<uint32_t>(e->GetArrayLength(array)) : 0;
            break;
        }
        case 688: { // NewObjectArray
            const jsize length = static_cast<jsize>(jit.Regs()[1]);
            jclass type = static_cast<jclass>(ResolveObject(jit.Regs()[2]));
            jobject initial = ResolveObject(jit.Regs()[3]);
            jobjectArray array = type ? e->NewObjectArray(length, type, initial) : nullptr;
            jit.Regs()[0] = array ? AddObjectRef(array) : 0;
            break;
        }
        case 692: { // GetObjectArrayElement
            jobjectArray array = static_cast<jobjectArray>(ResolveObject(jit.Regs()[1]));
            jobject value = array ? e->GetObjectArrayElement(array, static_cast<jsize>(jit.Regs()[2])) : nullptr;
            jit.Regs()[0] = value ? AddObjectRef(value) : 0;
            break;
        }
        case 696: { // SetObjectArrayElement
            jobjectArray array = static_cast<jobjectArray>(ResolveObject(jit.Regs()[1]));
            if (array) e->SetObjectArrayElement(array, static_cast<jsize>(jit.Regs()[2]), ResolveObject(jit.Regs()[3]));
            jit.Regs()[0] = 0;
            break;
        }
        case 704: { // NewByteArray
            jbyteArray array = e->NewByteArray(static_cast<jsize>(jit.Regs()[1]));
            jit.Regs()[0] = array ? AddObjectRef(array) : 0;
            break;
        }
        case 736: { // GetByteArrayElements
            jbyteArray array = static_cast<jbyteArray>(ResolveObject(jit.Regs()[1]));
            if (!array) { jit.Regs()[0] = 0; break; }
            const jsize length = e->GetArrayLength(array);
            const uint32_t guest = m_host->GuestAlloc(static_cast<size_t>(length), 8);
            if (!guest) { jit.Regs()[0] = 0; break; }
            e->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(mem + guest));
            if (jit.Regs()[2]) m_host->MemoryWrite8(jit.Regs()[2], 1);
            jit.Regs()[0] = guest;
            break;
        }
        case 748: { // GetIntArrayElements
            jintArray array = static_cast<jintArray>(ResolveObject(jit.Regs()[1]));
            if (!array) { jit.Regs()[0] = 0; break; }
            const jsize length = e->GetArrayLength(array);
            const uint32_t guest = m_host->GuestAlloc(static_cast<size_t>(length) * sizeof(jint), 8);
            if (!guest) { jit.Regs()[0] = 0; break; }
            e->GetIntArrayRegion(array, 0, length, reinterpret_cast<jint*>(mem + guest));
            if (jit.Regs()[2]) m_host->MemoryWrite8(jit.Regs()[2], 1);
            jit.Regs()[0] = guest;
            break;
        }
        case 768: { // ReleaseByteArrayElements
            jbyteArray array = static_cast<jbyteArray>(ResolveObject(jit.Regs()[1]));
            const uint32_t guest = jit.Regs()[2];
            const jint mode = static_cast<jint>(jit.Regs()[3]);
            if (array && guest && mode != JNI_ABORT) {
                const jsize length = e->GetArrayLength(array);
                e->SetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(mem + guest));
            }
            if (guest && mode != JNI_COMMIT) m_host->GuestFree(guest);
            jit.Regs()[0] = 0;
            break;
        }
        case 780: { // ReleaseIntArrayElements
            jintArray array = static_cast<jintArray>(ResolveObject(jit.Regs()[1]));
            const uint32_t guest = jit.Regs()[2];
            const jint mode = static_cast<jint>(jit.Regs()[3]);
            if (array && guest && mode != JNI_ABORT) {
                const jsize length = e->GetArrayLength(array);
                e->SetIntArrayRegion(array, 0, length, reinterpret_cast<jint*>(mem + guest));
            }
            if (guest && mode != JNI_COMMIT) m_host->GuestFree(guest);
            jit.Regs()[0] = 0;
            break;
        }
        case 832: { // SetByteArrayRegion
            const uint32_t sp = jit.Regs()[13];
            if (!GuestRangeValid(m_host->GetGuestMemorySize(), sp, sizeof(uint32_t))) {
                LOGE("[JNI] SetByteArrayRegion rejected invalid guest stack 0x%08X", sp);
                jit.Regs()[0] = 0;
                break;
            }

            jbyteArray array = static_cast<jbyteArray>(ResolveObject(jit.Regs()[1]));
            const jint start = static_cast<jint>(jit.Regs()[2]);
            const jint length = static_cast<jint>(jit.Regs()[3]);
            const uint32_t source = m_host->MemoryRead32(sp);
            if (!array || start < 0 || length < 0) {
                LOGE("[JNI] SetByteArrayRegion rejected array=%p start=%d length=%d",
                     array, start, length);
                jit.Regs()[0] = 0;
                break;
            }

            const jsize array_length = e->GetArrayLength(array);
            if (start > array_length || length > array_length - start ||
                !GuestRangeValid(m_host->GetGuestMemorySize(), source,
                                 static_cast<size_t>(length))) {
                LOGE("[JNI] SetByteArrayRegion rejected start=%d length=%d arrayLength=%d source=0x%08X",
                     start, length, array_length, source);
                jit.Regs()[0] = 0;
                break;
            }

            e->SetByteArrayRegion(array, start, length,
                                  reinterpret_cast<const jbyte*>(mem + source));
            jit.Regs()[0] = 0;
            break;
        }

        // ---- strings ----
        case 668: { // NewStringUTF
            const char* s = ReadGuestStr(*m_host, jit.Regs()[1]);
            jstring js = e->NewStringUTF(s ? s : "");
            jit.Regs()[0] = AddObjectRef(js);
            break;
        }
        case 656: { // GetStringLength
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = s ? static_cast<uint32_t>(e->GetStringLength(s)) : 0;
            break;
        }
        case 660: { // GetStringChars (UTF-16)
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            if (!s) { jit.Regs()[0] = 0; break; }
            const jchar* chars = e->GetStringChars(s, nullptr);
            if (!chars) { jit.Regs()[0] = 0; break; }
            jsize len = e->GetStringLength(s);
            uint32_t guest = m_host->GuestAlloc((len + 1) * 2, 4);
            if (guest) {
                memcpy(mem + guest, chars, len * 2);
                memset(mem + guest + len * 2, 0, 2);
            }
            e->ReleaseStringChars(s, chars);
            if (guest && jit.Regs()[2]) {
                if (!GuestRangeValid(m_host->GetGuestMemorySize(), jit.Regs()[2], 1)) {
                    m_host->GuestFree(guest); guest = 0;
                    m_host->Fail("Invalid JNI UTF16 isCopy output");
                } else m_host->MemoryWrite8(jit.Regs()[2], JNI_TRUE);
            }
            jit.Regs()[0] = guest;
            break;
        }
        case 664: // ReleaseStringChars -> the copy is ours, and this is the only
                  // call that says when it stops being needed
            m_host->GuestFree(jit.Regs()[2]);
            jit.Regs()[0] = 0;
            break;
        case 676: { // GetStringUTFChars
            const uint32_t handle = jit.Regs()[1];
            jstring s = static_cast<jstring>(ResolveObject(handle));
            const uint32_t isCopyOut = jit.Regs()[2];
            if (!s) { jit.Regs()[0] = 0; break; }
            if (isCopyOut && !GuestRangeValid(m_host->GetGuestMemorySize(), isCopyOut, 1)) {
                m_host->Fail("Invalid JNI UTF isCopy output"); jit.Regs()[0] = 0; break;
            }
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_objects.find(handle) == m_objects.end()) {
                m_host->Fail("JNI UTF owner expired before acquisition"); jit.Regs()[0] = 0; break;
            }
            auto cached = m_jstr_utf.find(handle);
            if (cached != m_jstr_utf.end()) {
                PinRefLocked(handle, +1); ++m_jstr_utf_leases[handle];
                if (isCopyOut) m_host->MemoryWrite8(isCopyOut, JNI_TRUE);
                jit.Regs()[0] = cached->second;
                break;
            }
            const char* utf = e->GetStringUTFChars(s, nullptr);
            if (!utf) { jit.Regs()[0] = 0; break; }
            const size_t len = strlen(utf) + 1;
            uint32_t guest = m_host->GuestAlloc(len, 4);
            if (guest) memcpy(mem + guest, utf, len);
            e->ReleaseStringUTFChars(s, utf);
            if (!guest) { jit.Regs()[0] = 0; break; }
            m_jstr_utf[handle] = guest;
            m_jstr_utf_leases[handle] = 1; PinRefLocked(handle, +1);
            if (isCopyOut) m_host->MemoryWrite8(isCopyOut, JNI_TRUE);
            jit.Regs()[0] = guest;
            break;
        }
        case 680: { // ReleaseStringUTFChars; last lease frees the owned scratch
            const uint32_t handle = jit.Regs()[1];
            std::lock_guard<std::mutex> lk(m_mutex);
            auto copy = m_jstr_utf.find(handle);
            auto lease = m_jstr_utf_leases.find(handle);
            if (copy == m_jstr_utf.end() || lease == m_jstr_utf_leases.end() ||
                copy->second != jit.Regs()[2] || !lease->second) {
                m_host->Fail("Unmatched JNI UTF release"); jit.Regs()[0] = 0; break;
            }
            PinRefLocked(handle, -1);
            if (--lease->second == 0) {
                m_host->GuestFree(copy->second);
                m_jstr_utf.erase(copy); m_jstr_utf_leases.erase(lease);
            }
            jit.Regs()[0] = 0;
            break;
        }

        // ---- exceptions ----
        case 60: { // ExceptionOccurred
            jthrowable t = e->ExceptionOccurred();
            jit.Regs()[0] = t ? AddObjectRef(t) : 0;
            break;
        }
        case 64: // ExceptionDescribe
            e->ExceptionDescribe();
            jit.Regs()[0] = 0;
            break;
        case 68: // ExceptionClear
            e->ExceptionClear();
            jit.Regs()[0] = 0;
            break;
        case 912: { // ExceptionCheck
            jit.Regs()[0] = e->ExceptionCheck() ? 1 : 0;
            break;
        }
        case 56: { // ThrowNew
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[2]);
            if (c) e->ThrowNew(c, msg);
            jit.Regs()[0] = 0;
            break;
        }
        case 72: { // FatalError
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[1]);
            LOGE("[JNI] FatalError: %s", msg ? msg : "");
            m_host->Fail(msg ? msg : "Guest JNI FatalError");
            jit.Regs()[0] = 0;
            break;
        }

        // ---- direct buffers ----
        case 860: { // RegisterNatives: validate all ARM32 records before publish
            jclass cls = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const jint count = static_cast<jint>(jit.Regs()[3]);
            const uint32_t records = jit.Regs()[2];
            bool valid = cls && count >= 0 && count <= 4096 && !(records & 3) &&
                GuestRangeValid(m_host->GetGuestMemorySize(), records, static_cast<size_t>(count) * 12);
            const std::string class_name = valid ? NativeClassName(e, cls) : "";
            valid = valid && !class_name.empty() && !e->ExceptionCheck();
            std::vector<std::pair<std::string, uint32_t>> staged;
            for (jint i = 0; valid && i < count; ++i) {
                const uint32_t row = records + static_cast<uint32_t>(i) * 12;
                const char* name = ReadGuestStr(*m_host, m_host->MemoryRead32(row));
                const char* sig = ReadGuestStr(*m_host, m_host->MemoryRead32(row + 4));
                const uint32_t target = m_host->MemoryRead32(row + 8);
                valid = !m_host->HasFailed() && *name && strlen(name) <= 255 &&
                    *sig == '(' && strlen(sig) <= 1024 && m_host->IsExecutableAddress(target);
                if (!valid) break;
                bool is_static = true;
                jmethodID method = e->GetStaticMethodID(cls, name, sig);
                if (!method) {
                    e->ExceptionClear(); is_static = false;
                    method = e->GetMethodID(cls, name, sig);
                }
                if (!method) { e->ExceptionClear(); valid = false; break; }
                jobject reflected = e->ToReflectedMethod(cls, method, is_static ? JNI_TRUE : JNI_FALSE);
                jclass reflected_type = reflected ? e->GetObjectClass(reflected) : nullptr;
                jmethodID modifiers = reflected_type ? e->GetMethodID(reflected_type, "getModifiers", "()I") : nullptr;
                jmethodID declaring = reflected_type ? e->GetMethodID(reflected_type, "getDeclaringClass", "()Ljava/lang/Class;") : nullptr;
                const jint flags = modifiers ? e->CallIntMethod(reflected, modifiers) : 0;
                jobject owner = declaring && !e->ExceptionCheck() ? e->CallObjectMethod(reflected, declaring) : nullptr;
                valid = (flags & 0x100) && owner && !e->ExceptionCheck() && e->IsSameObject(owner, cls);
                const std::string key = class_name + '\n' + name + '\n' + sig;
                for (const auto& existing : staged) if (existing.first == key) valid = false;
                if (valid) staged.emplace_back(key, target);
                if (reflected_type) e->DeleteLocalRef(reflected_type);
                if (reflected) e->DeleteLocalRef(reflected);
                if (owner) e->DeleteLocalRef(owner);
            }
            if (valid) {
                std::lock_guard<std::mutex> lk(m_mutex);
                size_t additions = 0;
                for (const auto& [key, target] : staged) if (!m_registered_natives.count(key)) ++additions;
                valid = additions <= 4096 - m_registered_natives.size();
                if (valid) for (const auto& [key, target] : staged) m_registered_natives[key] = target;
            }
            if (e->ExceptionCheck()) e->ExceptionClear();
            jit.Regs()[0] = static_cast<uint32_t>(valid ? JNI_OK : JNI_ERR);
            break;
        }
        case 864: { // UnregisterNatives: clear only this class's guest bindings
            jclass cls = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const std::string class_name = cls ? NativeClassName(e, cls) : "";
            if (class_name.empty() || e->ExceptionCheck()) { jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR); break; }
            const std::string prefix = class_name + '\n';
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_registered_natives.begin(); it != m_registered_natives.end();) {
                if (it->first.compare(0, prefix.size(), prefix) == 0) it = m_registered_natives.erase(it);
                else ++it;
            }
            jit.Regs()[0] = JNI_OK; break;
        }
        case 920: { // GetDirectBufferAddress
            const uint32_t handle = jit.Regs()[1];
            { std::lock_guard<std::mutex> buffers(m_buffer_mutex);
              auto it = m_direct_bufs.find(handle);
              if (it != m_direct_bufs.end()) { jit.Regs()[0] = it->second.scratch; break; } }
            jobject buf = ResolveObject(handle);
            const uint32_t registered = buf ? RegisterDirectBuffer(buf) : 0;
            std::lock_guard<std::mutex> buffers(m_buffer_mutex);
            auto it = m_direct_bufs.find(registered);
            jit.Regs()[0] = it == m_direct_bufs.end() ? 0 : it->second.scratch;
            break;
        }
        case 924: { // GetDirectBufferCapacity returns signed jlong in r0:r1
            const uint32_t handle = jit.Regs()[1];
            jlong capacity = DirectBufferCapacity(handle);
            if (capacity < 0) {
                jobject buf = ResolveObject(handle);
                capacity = buf ? e->GetDirectBufferCapacity(buf) : -1;
            }
            jit.Regs()[0] = static_cast<uint32_t>(capacity);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(capacity) >> 32);
            break;
        }

        // ---- misc ----
        case 104: // EnsureLocalCapacity
            jit.Regs()[0] = static_cast<uint32_t>(e->EnsureLocalCapacity(static_cast<jint>(jit.Regs()[1]))); break;
        case 876: { // GetJavaVM
            uint32_t out = jit.Regs()[1];
            if (!GuestRangeValid(m_host->GetGuestMemorySize(), out, 4)) {
                m_host->Fail("Invalid JNI GetJavaVM output");
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
            } else { m_host->MemoryWrite32(out, m_vm_table); jit.Regs()[0] = JNI_OK; }
            break;
        }
        case 96: { // IsSameObject
            jobject a = ResolveObject(jit.Regs()[1]);
            jobject b = ResolveObject(jit.Regs()[2]);
            jit.Regs()[0] = e->IsSameObject(a, b) ? 1 : 0;
            break;
        }
        case 52: { // Throw
            jobject t = ResolveObject(jit.Regs()[1]);
            if (t) e->Throw(static_cast<jthrowable>(t));
            jit.Regs()[0] = 0;
            break;
        }
        case 416: case 436: { // SetObjectField / SetIntField
            jobject o = ResolveObject(jit.Regs()[1]);
            const jfieldID field = ResolveField(jit.Regs()[2]);
            if (o && field != nullptr) {
                if (offset == 416) e->SetObjectField(o, field, ResolveObject(jit.Regs()[3]));
                else e->SetIntField(o, field, static_cast<jint>(jit.Regs()[3]));
            }
            jit.Regs()[0] = 0;
            break;
        }
        case 404: { // GetLongField
            jobject o = ResolveObject(jit.Regs()[1]);
            const jfieldID field = ResolveField(jit.Regs()[2]);
            jlong v = (o && field != nullptr) ? e->GetLongField(o, field) : 0;
            jit.Regs()[0] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
            break;
        }

        default: {
            LOGE("[JNI] unsupported slot %u off=0x%X PC=0x%08X LR=0x%08X",
                 offset / 4, offset, jit.Regs()[15], jit.Regs()[14]);
            m_host->Fail("Unsupported guest JNIEnv operation");
            jit.Regs()[0] = jit.Regs()[1] = 0;
            break;
        }
    }
}
