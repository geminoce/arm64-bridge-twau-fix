// SPDX-License-Identifier: GPL-3.0-or-later
#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "game_port_config.h"
#include "thunks_android.h"
#include "elf_loader.h"
#include "wolf_shader_compat.h"
#include <array>
#include <chrono>
#include <string_view>
#include <unistd.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <map>
#include <tuple>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Buffer maps must keep native pointers out of the ARM32 address space.
// ES3 uses a read/write host map; ES2 WRITE_ONLY mappings preserve bytes through
// CPU upload shadows. Keys include the guest arena and the native context.
using BufferKey = std::tuple<uint8_t*, uint64_t, GLuint>;
struct GuestMappedBuffer {
    uint32_t address = 0;
    size_t size = 0;
    void* native = nullptr;
    bool es3 = false;
};
static std::map<BufferKey, GuestMappedBuffer> s_mapped_buffers;
static std::map<BufferKey, std::vector<uint8_t>> s_buffer_shadows;
static std::mutex s_mapped_buffers_mutex;
static std::map<std::pair<uint8_t*, EGLContext>, uint64_t> s_context_groups;
static uint64_t s_next_group = 1;

// Caller holds s_mapped_buffers_mutex. EGL shared contexts name the same buffer
// store; a context switch must not orphan or confuse its staging allocation.
static BufferKey BufferKeyLocked(DynarmicHost& host, GLuint buffer) {
    auto [it, inserted] = s_context_groups.emplace(
        std::make_pair(host.GetGuestMemoryBase(), eglGetCurrentContext()), 0);
    if (inserted) it->second = s_next_group++;
    return {host.GetGuestMemoryBase(), it->second, buffer};
}

void TrackBridgeGlContext(DynarmicHost& host, void* context, void* shared) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
    uint64_t group = 0;
    if (shared) {
        auto [it, inserted] = s_context_groups.emplace(std::make_pair(host.GetGuestMemoryBase(), shared), 0);
        if (inserted) it->second = s_next_group++;
        group = it->second;
    } else group = s_next_group++;
    s_context_groups[{host.GetGuestMemoryBase(), context}] = group;
}

bool BridgeHasGlES3() {
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return false;
    EGLint version = 0;
    if (eglQueryContext(eglGetCurrentDisplay(), context, EGL_CONTEXT_CLIENT_VERSION, &version) && version >= 3)
        return true;
    // The GL version describes the actual context, including a driver's ES2
    // compatible context exposed as ES3. No GL error is read or cleared here.
    const char* value = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return value && strncmp(value, "OpenGL ES 3", 11) == 0;
}

GLuint BridgeBoundBuffer(GLenum target) {
    GLenum binding = 0;
    if (target == GL_ARRAY_BUFFER) binding = GL_ARRAY_BUFFER_BINDING;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) binding = GL_ELEMENT_ARRAY_BUFFER_BINDING;
    else if (BridgeHasGlES3()) {
        if (target == GL_PIXEL_PACK_BUFFER) binding = GL_PIXEL_PACK_BUFFER_BINDING;
        else if (target == GL_PIXEL_UNPACK_BUFFER) binding = GL_PIXEL_UNPACK_BUFFER_BINDING;
    }
    GLint value = 0;
    if (binding) glGetIntegerv(binding, &value);
    return static_cast<GLuint>(value);
}

void ResetBridgeGlStorage(DynarmicHost& host) {
    std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
    for (auto it = s_mapped_buffers.begin(); it != s_mapped_buffers.end();) {
        if (std::get<0>(it->first) == host.GetGuestMemoryBase()) {
            host.GuestFree(it->second.address);
            it = s_mapped_buffers.erase(it);
        } else ++it;
    }
    for (auto it = s_buffer_shadows.begin(); it != s_buffer_shadows.end();) {
        if (std::get<0>(it->first) == host.GetGuestMemoryBase()) it = s_buffer_shadows.erase(it);
        else ++it;
    }
    for (auto it = s_context_groups.begin(); it != s_context_groups.end();) {
        if (it->first.first == host.GetGuestMemoryBase()) it = s_context_groups.erase(it);
        else ++it;
    }
}

void ForgetBridgeGlContext(DynarmicHost& host, void* context) {
    std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
    const auto context_it = s_context_groups.find({host.GetGuestMemoryBase(), context});
    if (context_it == s_context_groups.end()) return;
    const uint64_t group = context_it->second;
    s_context_groups.erase(context_it);
    for (const auto& [key, other_group] : s_context_groups)
        if (key.first == host.GetGuestMemoryBase() && other_group == group) return;
    for (auto it = s_mapped_buffers.begin(); it != s_mapped_buffers.end();) {
        if (std::get<0>(it->first) == host.GetGuestMemoryBase() && std::get<1>(it->first) == group) {
            host.GuestFree(it->second.address);
            it = s_mapped_buffers.erase(it);
        } else ++it;
    }
    for (auto it = s_buffer_shadows.begin(); it != s_buffer_shadows.end();) {
        if (std::get<0>(it->first) == host.GetGuestMemoryBase() && std::get<1>(it->first) == group)
            it = s_buffer_shadows.erase(it);
        else ++it;
    }
}

void BridgeBufferSubData(DynarmicHost& host, GLenum target, GLintptr offset,
                         GLsizeiptr size, const void* data) {
    glBufferSubData(target, offset, size, data);
    if (offset < 0 || size < 0 || !data) return;
    const GLuint buffer = BridgeBoundBuffer(target);
    std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
    const BufferKey key = BufferKeyLocked(host, buffer);
    auto it = s_buffer_shadows.find(key);
    if (it != s_buffer_shadows.end() && static_cast<size_t>(offset) <= it->second.size() &&
        static_cast<size_t>(size) <= it->second.size() - static_cast<size_t>(offset))
        memcpy(it->second.data() + offset, data, static_cast<size_t>(size));
}

// UI scaling, the virtual-resolution way: tell the guest the screen is
// s_virtual{Width,Height} and stretch the result here. The guest sizes its viewport
// in those units while the real default framebuffer is still the surface size, so
// the window-system framebuffer -- and only that one -- gets mapped back. Offscreen
// targets are allocated in the guest's own units and must not be touched, which is
// what the actual framebuffer binding check is for.
// Leave s_virtualWidth/Height at 0 to disable the whole path.
static void ScaleViewportToSurface(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    const uint32_t vw = ThunkManager::s_virtualWidth, vh = ThunkManager::s_virtualHeight;
    const uint32_t rw = ThunkManager::s_screenWidth, rh = ThunkManager::s_screenHeight;
    if (!vw || !vh || !rw || !rh || (vw == rw && vh == rh)) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0) return;
    x = static_cast<GLint>((float)x * rw / vw);
    w = static_cast<GLsizei>((float)w * rw / vw);
    y = static_cast<GLint>((float)y * rh / vh);
    h = static_cast<GLsizei>((float)h * rh / vh);
}
#define LOG_TAG "Arm32Bridge_GL"
#if ARM32_BRIDGE_ENABLE_LOGGING
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

void ObserveBridgeGlSwap(DynarmicHost& host, void* display, void* surface, uint32_t lr, uint32_t sp, bool ok) {
    static thread_local unsigned display_rows = 0;
    static thread_local std::array<uint32_t, 4> last_size{};
    const auto* engine = host.FindModule("libGameEngine.so");
    if (ok && engine && display_rows < 8) {
        const uint32_t base = engine->GetBaseAddress();
        const std::array<uint32_t, 4> size{
            host.MemoryRead32(base + 0xCC9D30u), host.MemoryRead32(base + 0xCC81B8u),
            host.MemoryRead32(base + 0xCC64DCu), host.MemoryRead32(base + 0xCC5CB4u)};
        if (size != last_size && size[0] && size[1] && size[2] && size[3]) {
            GLint viewport[4]{};
            glGetIntegerv(GL_VIEWPORT, viewport);
            LOGI("WOLF_DISPLAY_FRAME game=%u,%u device=%u,%u viewport=%d,%d,%d,%d force=%u texture_scalar_bits=%08X",
                 size[0], size[1], size[2], size[3], viewport[0], viewport[1], viewport[2], viewport[3],
                 host.MemoryRead8(base + 0xC75981u), host.MemoryRead32(base + 0xC754B8u));
            last_size = size;
            ++display_rows;
        }
    }
}

namespace {
struct GlStringCache {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> values;
    uint32_t Copy(DynarmicHost& host, const GLubyte* raw) {
        if (!raw) return 0; // Never cache an absent context's null result.
        const std::string value(reinterpret_cast<const char*>(raw));
        std::lock_guard<std::mutex> lock(mutex);
        auto it = values.find(value);
        if (it != values.end()) return it->second;
        const uint32_t address = host.GuestAlloc(value.size() + 1, 4);
        if (address) {
            memcpy(host.GetGuestMemoryBase() + address, value.c_str(), value.size() + 1);
            values.emplace(value, address);
        }
        return address;
    }
};
template<typename T> T GlProc(const char* name) {
    return reinterpret_cast<T>(eglGetProcAddress(name));
}
} // namespace

bool ResolveBridgeGlPixels(DynarmicHost&, GLsizei, GLsizei, GLenum, GLenum,
                           uint32_t, bool, bool, const void**, uint32_t* = nullptr);
bool ResolveBridgeGlCompressed(DynarmicHost&, uint32_t, GLsizei, bool, const void**, uint32_t* = nullptr);

void RegisterGles2CoreThunks(ThunkManager& manager);
size_t GetGlStateValueCount(GLenum pname);

void ThunkManager::RegisterGLThunks() {
    const auto string_cache = std::make_shared<GlStringCache>();

    RegisterThunk("glActiveTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glActiveTexture(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glAttachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glAttachShader(jit.Regs()[0], jit.Regs()[1]);
    });


    RegisterThunk("glBindBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBindBuffer(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glBindFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindFramebuffer(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glBindRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindRenderbuffer(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glBindTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindTexture(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glBlendFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFunc(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glBlendFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFuncSeparate(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]),
                            static_cast<GLenum>(jit.Regs()[2]), static_cast<GLenum>(jit.Regs()[3]));
    });

    RegisterThunk("glBufferData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0];
        const GLsizeiptr size = static_cast<int32_t>(jit.Regs()[1]);
        const uint32_t address = jit.Regs()[2];
        const void* data = address && size >= 0 ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, static_cast<size_t>(size)) : nullptr;
        if (address && size >= 0 && !data) return;
        glBufferData(target, size, data, jit.Regs()[3]);
        const GLuint buffer = BridgeBoundBuffer(target);
        if (size < 0 || !buffer) return;
        std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
        const BufferKey key = BufferKeyLocked(host, buffer);
        auto mapping = s_mapped_buffers.find(key);
        if (mapping != s_mapped_buffers.end()) {
            GLint mapped = GL_TRUE;
            glGetBufferParameteriv(target, GL_BUFFER_MAPPED_OES, &mapped);
            if (mapped) return; // Rejected BufferData leaves the existing mapping alive.
        }
        if (BridgeHasGlES3()) {
            // A replacement store invalidates any ES2 preimage in the group.
            s_buffer_shadows.erase(key);
        } else {
            GLint actual_size = 0, actual_usage = 0;
            glGetBufferParameteriv(target, GL_BUFFER_SIZE, &actual_size);
            glGetBufferParameteriv(target, GL_BUFFER_USAGE, &actual_usage);
            if (actual_size == size && static_cast<GLenum>(actual_usage) == jit.Regs()[3]) {
                auto& shadow = s_buffer_shadows[key];
                shadow.resize(static_cast<size_t>(size));
                if (data && size) memcpy(shadow.data(), data, static_cast<size_t>(size));
                // Null BufferData leaves an undefined store. Zero is one valid preimage.
                else std::fill(shadow.begin(), shadow.end(), 0);
            }
        }
        if (mapping != s_mapped_buffers.end()) {
            // BufferData implicitly unmaps; data may still refer to this guest staging block.
            host.GuestFree(mapping->second.address);
            s_mapped_buffers.erase(mapping);
        }
    });
    RegisterThunk("glBufferSubData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLintptr offset = static_cast<int32_t>(jit.Regs()[1]);
        const GLsizeiptr size = static_cast<int32_t>(jit.Regs()[2]);
        const void* data = size >= 0 ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[3], static_cast<size_t>(size)) : nullptr;
        if (size > 0 && !data) return;
        BridgeBufferSubData(host, jit.Regs()[0], offset, size, data);
    });

    RegisterThunk("glMapBufferOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0], access = jit.Regs()[1];
        jit.Regs()[0] = 0;
        const auto native_map = GlProc<PFNGLMAPBUFFEROESPROC>("glMapBufferOES");
        const auto native_unmap = GlProc<PFNGLUNMAPBUFFEROESPROC>("glUnmapBufferOES");
        const GLuint buffer = BridgeBoundBuffer(target);
        if (!buffer || access != GL_WRITE_ONLY_OES) {
            if (native_map) native_map(target, access); // Preserve the driver's error.
            return;
        }
        GLint size = 0, mapped = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        glGetBufferParameteriv(target, GL_BUFFER_MAPPED_OES, &mapped);
        if (size <= 0 || mapped) {
            if (native_map) native_map(target, access);
            return;
        }
        std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
        const BufferKey key = BufferKeyLocked(host, buffer);
        if (s_mapped_buffers.count(key)) return;
        const bool es3 = BridgeHasGlES3();
        auto shadow = s_buffer_shadows.find(key);
        if (!es3 && (shadow == s_buffer_shadows.end() || shadow->second.size() != static_cast<size_t>(size))) {
            LOGE("glMapBufferOES cannot preserve ES2 buffer %u without upload preimage", buffer);
            return;
        }
        if (!es3 && (!native_map || !native_unmap)) return;
        void* native = es3 ? glMapBufferRange(target, 0, size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)
                            : native_map(target, access);
        if (!native) return;
        const uint32_t address = host.GuestAlloc(static_cast<size_t>(size), 16);
        if (!address) { if (es3) glUnmapBuffer(target); else native_unmap(target); return; }
        // Reading a WRITE_ONLY OES native mapping is forbidden. ES2 uses the
        // exact CPU upload preimage; ES3 explicitly requested a readable map.
        memcpy(host.GetGuestMemoryBase() + address, es3 ? native : shadow->second.data(), static_cast<size_t>(size));
        s_mapped_buffers.emplace(key, GuestMappedBuffer{address, static_cast<size_t>(size), native, es3});
        jit.Regs()[0] = address;
    });

    RegisterThunk("glUnmapBufferOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = jit.Regs()[0];
        const GLuint buffer = BridgeBoundBuffer(target);
        std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
        const BufferKey key = BufferKeyLocked(host, buffer);
        auto it = s_mapped_buffers.find(key);
        if (it == s_mapped_buffers.end()) {
            const auto native_unmap = GlProc<PFNGLUNMAPBUFFEROESPROC>("glUnmapBufferOES");
            jit.Regs()[0] = BridgeHasGlES3() ? glUnmapBuffer(target)
                : native_unmap ? native_unmap(target) : GL_FALSE;
            return;
        }
        const GuestMappedBuffer mapping = it->second;
        const bool current_es3 = BridgeHasGlES3();
        const auto native_unmap = GlProc<PFNGLUNMAPBUFFEROESPROC>("glUnmapBufferOES");
        if (!current_es3 && !native_unmap) { jit.Regs()[0] = GL_FALSE; return; }
        const void* staged = host.GetGuestMemoryBase() + mapping.address;
        memcpy(mapping.native, staged, mapping.size);
        const GLboolean result = current_es3 ? glUnmapBuffer(target) : native_unmap(target);
        if (!mapping.es3 || !current_es3 || s_buffer_shadows.count(key)) {
            if (result == GL_TRUE) {
                auto& shadow = s_buffer_shadows[key];
                shadow.resize(mapping.size);
                memcpy(shadow.data(), staged, mapping.size);
            } else s_buffer_shadows.erase(key);
        }
        host.GuestFree(mapping.address);
        s_mapped_buffers.erase(it);
        jit.Regs()[0] = result;
    });

    RegisterThunk("glCheckFramebufferStatus", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCheckFramebufferStatus(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glClear", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glClear(static_cast<GLbitfield>(jit.Regs()[0]));
    });

    RegisterThunk("glClearColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float r = ReadSF(jit, 0);
        float g = ReadSF(jit, 1);
        float b = ReadSF(jit, 2);
        float a = ReadSF(jit, 3);
        glClearColor(r, g, b, a);
    });

    RegisterThunk("glColorMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glColorMask(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glCompileShader", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glCompileShader(jit.Regs()[0]);
    });

    RegisterThunk("glCompressedTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = args.U32(); const GLint level = args.I32();
        const GLenum internal_format = args.U32(); const GLsizei width = args.I32();
        const GLsizei height = args.I32(); const GLint border = args.I32();
        const GLsizei image_size = args.I32(); const uint32_t address = args.Ptr();
        const void* data = nullptr;
        uint32_t pbo = 0;
        const bool resolved = ResolveBridgeGlCompressed(host, address, image_size, true, &data, &pbo);
        if (resolved) glCompressedTexImage2D(target, level, internal_format, width, height, border, image_size, data);
    });

    RegisterThunk("glCreateProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCreateProgram();
    });

    RegisterThunk("glCreateShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCreateShader(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glCullFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glCullFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDeleteBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const GLuint* names = count > 0 ? static_cast<const GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint))) : nullptr;
        if (count > 0 && !names) return;
        glDeleteBuffers(count, names);
        std::lock_guard<std::mutex> lock(s_mapped_buffers_mutex);
        for (GLsizei i = 0; i < count; ++i) {
            const BufferKey key = BufferKeyLocked(host, names[i]);
            auto it = s_mapped_buffers.find(key);
            if (it != s_mapped_buffers.end()) { host.GuestFree(it->second.address); s_mapped_buffers.erase(it); }
            s_buffer_shadows.erase(key);
        }
    });


    RegisterThunk("glDeleteProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteProgram(jit.Regs()[0]);
    });


    RegisterThunk("glDeleteShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteShader(jit.Regs()[0]);
    });


    RegisterThunk("glDepthFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthFunc(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDepthMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthMask(static_cast<GLboolean>(jit.Regs()[0]));
    });

    RegisterThunk("glDisable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDisableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glDrawArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glDrawArrays(jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), static_cast<GLsizei>(jit.Regs()[2]));
    });

    RegisterThunk("glDrawElements", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t indices_gaddr = jit.Regs()[3];
        const void* indices = (BridgeBoundBuffer(GL_ELEMENT_ARRAY_BUFFER) != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), indices_gaddr);
        glDrawElements(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLsizei>(jit.Regs()[1]),
                       static_cast<GLenum>(jit.Regs()[2]), indices);
    });

    RegisterThunk("glEnable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnable(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glEnableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glFlush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFlush();
    });

    RegisterThunk("glFramebufferRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFramebufferRenderbuffer(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glFramebufferTexture2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFramebufferTexture2D(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3], host.MemoryRead32(jit.Regs()[13]));
    });

    RegisterThunk("glFrontFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFrontFace(static_cast<GLenum>(jit.Regs()[0]));
    });





    RegisterThunk("glGenerateMipmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenerateMipmap(static_cast<GLenum>(jit.Regs()[0]));
    });




    RegisterThunk("glGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum error = glGetError();
        jit.Regs()[0] = error;
    });







    // GL promises a pointer valid for the life of the context, and callers keep
    // it. One block per enum, so a capability check inside a frame loop costs
    // nothing after the first frame - GL_EXTENSIONS alone is a couple of KiB.
    RegisterThunk("glGetString", [string_cache](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum name = jit.Regs()[0];
        const GLubyte* value = glGetString(name);
        jit.Regs()[0] = string_cache->Copy(host, value);
    });
    RegisterThunk("glGetStringi", [string_cache](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const auto native = GlProc<PFNGLGETSTRINGIPROC>("glGetStringi");
        const GLenum name = jit.Regs()[0];
        const GLuint index = jit.Regs()[1];
        const GLubyte* value = native ? native(name, index) : nullptr;
        jit.Regs()[0] = value ? string_cache->Copy(host, value) : 0;
    });


    RegisterThunk("glHint", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glHint(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glLineWidth", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glLineWidth(ReadSF(jit, 0));
    });

    RegisterThunk("glLinkProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint prog = jit.Regs()[0];
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            LOGE("Program %u link failed: %s", prog, log);
        }
    });

    RegisterThunk("glPixelStorei", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPixelStorei(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glPolygonOffset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPolygonOffset(ReadSF(jit, 0), ReadSF(jit, 1));
    });

    RegisterThunk("glReadPixels", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint x = args.I32(), y = args.I32(); const GLsizei width = args.I32(), height = args.I32();
        const GLenum format = args.U32(), type = args.U32(); const uint32_t address = args.Ptr();
        const void* pixels = nullptr;
        if (ResolveBridgeGlPixels(host, width, height, format, type, address, true, false, &pixels))
            glReadPixels(x, y, width, height, format, type, const_cast<void*>(pixels));
    });

    RegisterThunk("glRenderbufferStorage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glRenderbufferStorage(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glScissor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glScissor(x, y, w, h);
    });

    RegisterThunk("glShaderSource", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        if (count < 0) { glShaderSource(jit.Regs()[0], count, nullptr, nullptr); return; }
        if (count > 4096) return;
        const size_t bytes = static_cast<size_t>(count) * sizeof(uint32_t);
        const auto pointers = static_cast<const uint32_t*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], bytes));
        const auto lengths = jit.Regs()[3] ? static_cast<const GLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[3], bytes)) : nullptr;
        if ((count && !pointers) || (jit.Regs()[3] && !lengths)) return;
        std::vector<const GLchar*> strings(static_cast<size_t>(count));
        for (GLsizei i = 0; i < count; ++i) {
            const uint32_t address = pointers[i];
            const GLint length = lengths ? lengths[i] : -1;
            if (!GuestRangeValid(host.GetGuestMemorySize(), address, length >= 0 ? static_cast<size_t>(length) : 1)) return;
            const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
            if (length < 0 && !memchr(text, 0, std::min<size_t>(host.GetGuestMemorySize() - address, 1024 * 1024))) return;
            strings[i] = text;
        }
        if (count == 3) {
            std::array<std::string_view, 3> parts;
            for (size_t i = 0; i < parts.size(); ++i) {
                const size_t length = lengths && lengths[i] >= 0 ?
                    static_cast<size_t>(lengths[i]) : std::strlen(strings[i]);
                parts[i] = {strings[i], length};
            }
            const std::string prepared = PrepareWolfShaderSource(parts[0], parts[1], parts[2]);
            if (!prepared.empty() && prepared.size() <= static_cast<size_t>(std::numeric_limits<GLint>::max())) {
                const GLchar* source = prepared.data();
                const GLint length = static_cast<GLint>(prepared.size());
                glShaderSource(jit.Regs()[0], 1, &source, &length);
                return;
            }
        }
        glShaderSource(jit.Regs()[0], count, strings.data(), lengths);
    });

    RegisterThunk("glTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = args.U32(); const GLint level = args.I32();
        const GLint internal_format = args.I32(); const GLsizei width = args.I32();
        const GLsizei height = args.I32(); const GLint border = args.I32();
        const GLenum format = args.U32(), type = args.U32(); const uint32_t address = args.Ptr();
        const void* pixels = nullptr;
        if (ResolveBridgeGlPixels(host, width, height, format, type, address, false, true, &pixels))
            glTexImage2D(target, level, internal_format, width, height, border, format, type, pixels);
    });

    RegisterThunk("glTexParameterf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexParameterf(jit.Regs()[0], jit.Regs()[1], ReadSF(jit, 2));
    });

    RegisterThunk("glTexParameteri", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexParameteri(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2]);
    });

    RegisterThunk("glUniform1f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform1f(jit.Regs()[0], ReadSF(jit, 1));
    });

    RegisterThunk("glUniform1i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform1i(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glUniform2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform2f(jit.Regs()[0], ReadSF(jit, 1), ReadSF(jit, 2));
    });

    RegisterThunk("glUniform3f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform3f(jit.Regs()[0], ReadSF(jit, 1),
                    ReadSF(jit, 2), ReadSF(jit, 3));
    });


    RegisterThunk("glUniform4f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        uint32_t w_bits = host.MemoryRead32(sp);
        float w; memcpy(&w, &w_bits, sizeof(w));
        glUniform4f(jit.Regs()[0], ReadSF(jit, 1),
                    ReadSF(jit, 2), ReadSF(jit, 3), w);
    });




    RegisterThunk("glUseProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUseProgram(jit.Regs()[0]);
    });

    RegisterThunk("glVertexAttribPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint index = jit.Regs()[0];
        GLint size = jit.Regs()[1];
        GLenum type = jit.Regs()[2];
        GLboolean normalized = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei stride = host.MemoryRead32(sp);
        uint32_t ptr_gaddr = host.MemoryRead32(sp + 4);

        const void* ptr = (BridgeBoundBuffer(GL_ARRAY_BUFFER) != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), ptr_gaddr);

        glVertexAttribPointer(index, size, type, normalized, stride, ptr);
    });

    RegisterThunk("glViewport", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei width = jit.Regs()[2], height = jit.Regs()[3];
        ScaleViewportToSurface(x, y, width, height);
        glViewport(x, y, width, height);
    });

    // Legacy GLES1/OES imports are function-pointer relocations in SDL2. Keep them typed
    // and guest-pointer-safe even when the active context is GLES2.
    RegisterThunk("glBindFramebufferOES", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const auto native = GlProc<PFNGLBINDFRAMEBUFFEROESPROC>("glBindFramebufferOES");
        if (native) native(jit.Regs()[0], jit.Regs()[1]);
        else LOGE("glBindFramebufferOES unavailable");
    });
    RegisterThunk("glBlendFuncSeparateOES", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const auto native = GlProc<PFNGLBLENDFUNCSEPARATEOESPROC>("glBlendFuncSeparateOES");
        if (native) native(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
        else LOGE("glBlendFuncSeparateOES unavailable");
    });
    RegisterThunk("glCheckFramebufferStatusOES", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const auto native = GlProc<PFNGLCHECKFRAMEBUFFERSTATUSOESPROC>("glCheckFramebufferStatusOES");
        jit.Regs()[0] = native ? native(jit.Regs()[0]) : 0;
        if (!native) LOGE("glCheckFramebufferStatusOES unavailable");
    });
    RegisterThunk("glColor4f", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glColor4f(ReadSF(jit, 0), ReadSF(jit, 1), ReadSF(jit, 2), ReadSF(jit, 3));
    });
    RegisterThunk("glDeleteFramebuffersOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const GLuint* framebuffers = count > 0
            ? static_cast<const GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint)))
            : nullptr;
        const auto native = GlProc<PFNGLDELETEFRAMEBUFFERSOESPROC>("glDeleteFramebuffersOES");
        if (native && (count <= 0 || framebuffers)) native(count, framebuffers);
        if (!native) LOGE("glDeleteFramebuffersOES unavailable");
    });
    RegisterThunk("glDisableClientState", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glDisableClientState(jit.Regs()[0]);
    });
    RegisterThunk("glDrawTexfOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLfloat x = args.F32(), y = args.F32(), z = args.F32(), width = args.F32(), height = args.F32();
        const auto native = GlProc<PFNGLDRAWTEXFOESPROC>("glDrawTexfOES");
        if (native) native(x, y, z, width, height);
        else LOGE("glDrawTexfOES unavailable in the actual native context");
    });
    RegisterThunk("glEnableClientState", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glEnableClientState(jit.Regs()[0]);
    });
    RegisterThunk("glFinish", [](Dynarmic::A32::Jit&, DynarmicHost&) { glFinish(); });
    RegisterThunk("glFramebufferTexture2DOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const auto native = GlProc<PFNGLFRAMEBUFFERTEXTURE2DOESPROC>("glFramebufferTexture2DOES");
        if (native) native(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3], host.MemoryRead32(jit.Regs()[13]));
        else LOGE("glFramebufferTexture2DOES unavailable");
    });
    RegisterThunk("glGenFramebuffersOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        GLuint* framebuffers = count > 0
            ? static_cast<GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint)))
            : nullptr;
        const auto native = GlProc<PFNGLGENFRAMEBUFFERSOESPROC>("glGenFramebuffersOES");
        if (native && (count <= 0 || framebuffers)) native(count, framebuffers);
        if (!native) LOGE("glGenFramebuffersOES unavailable");
    });
    RegisterThunk("glGetBooleanv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const size_t count = GetGlStateValueCount(jit.Regs()[0]);
        GLboolean* values = static_cast<GLboolean*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], count * sizeof(GLboolean)));
        if (values) glGetBooleanv(jit.Regs()[0], values);
    });
    RegisterThunk("glLoadIdentity", [](Dynarmic::A32::Jit&, DynarmicHost&) { glLoadIdentity(); });
    RegisterThunk("glMatrixMode", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { glMatrixMode(jit.Regs()[0]); });
    RegisterThunk("glOrthof", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLfloat left = args.F32();
        const GLfloat right = args.F32();
        const GLfloat bottom = args.F32();
        const GLfloat top = args.F32();
        const GLfloat near_value = args.F32();
        const GLfloat far_value = args.F32();
        glOrthof(left, right, bottom, top, near_value, far_value);
    });
    RegisterThunk("glPopMatrix", [](Dynarmic::A32::Jit&, DynarmicHost&) { glPopMatrix(); });
    RegisterThunk("glPushMatrix", [](Dynarmic::A32::Jit&, DynarmicHost&) { glPushMatrix(); });
    RegisterThunk("glRotatef", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glRotatef(ReadSF(jit, 0), ReadSF(jit, 1), ReadSF(jit, 2), ReadSF(jit, 3));
    });
    RegisterThunk("glShaderBinary", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const GLuint* shaders = count > 0
            ? static_cast<const GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint)))
            : nullptr;
        const GLsizei length = static_cast<GLsizei>(host.MemoryRead32(jit.Regs()[13]));
        const void* binary = length > 0
            ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[3], static_cast<size_t>(length))
            : nullptr;
        if (count > 0 && shaders && length > 0 && binary) glShaderBinary(count, shaders, jit.Regs()[2], binary, length);
    });
    RegisterThunk("glTexCoordPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei stride = static_cast<GLsizei>(jit.Regs()[2]);
        const uint32_t pointer = jit.Regs()[3];
        const void* data = BridgeBoundBuffer(GL_ARRAY_BUFFER) ? reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer))
                                                : GuestToHost(host.GetGuestMemoryBase(), pointer);
        glTexCoordPointer(jit.Regs()[0], jit.Regs()[1], stride, data);
    });
    RegisterThunk("glTexEnvf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glTexEnvf(jit.Regs()[0], jit.Regs()[1], ReadSF(jit, 2));
    });
    RegisterThunk("glTexParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const size_t count = jit.Regs()[1] == GL_TEXTURE_CROP_RECT_OES ? 4 : 1;
        const GLint* values = static_cast<const GLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], count * sizeof(GLint)));
        if (values) glTexParameteriv(jit.Regs()[0], jit.Regs()[1], values);
    });
    RegisterThunk("glTranslatef", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glTranslatef(ReadSF(jit, 0), ReadSF(jit, 1), ReadSF(jit, 2));
    });
    RegisterThunk("glVertexPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei stride = static_cast<GLsizei>(jit.Regs()[2]);
        const uint32_t pointer = jit.Regs()[3];
        const void* data = BridgeBoundBuffer(GL_ARRAY_BUFFER) ? reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer))
                                                : GuestToHost(host.GetGuestMemoryBase(), pointer);
        glVertexPointer(jit.Regs()[0], jit.Regs()[1], stride, data);
    });

    // Only actual guest extension names are added, using the native signatures.
    RegisterThunk("glBindVertexArrayOES", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const auto native = GlProc<PFNGLBINDVERTEXARRAYOESPROC>("glBindVertexArrayOES");
        if (native) native(jit.Regs()[0]);
    });
    RegisterThunk("glGenVertexArraysOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        auto* values = count > 0 ? static_cast<GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint))) : nullptr;
        const auto native = GlProc<PFNGLGENVERTEXARRAYSOESPROC>("glGenVertexArraysOES");
        if (native && (count <= 0 || values)) native(count, values);
    });
    RegisterThunk("glDeleteVertexArraysOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const auto* values = count > 0 ? static_cast<const GLuint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], static_cast<size_t>(count) * sizeof(GLuint))) : nullptr;
        const auto native = GlProc<PFNGLDELETEVERTEXARRAYSOESPROC>("glDeleteVertexArraysOES");
        if (native && (count <= 0 || values)) native(count, values);
    });
    RegisterThunk("glDiscardFramebufferEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        const auto* values = count > 0 ? static_cast<const GLenum*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], static_cast<size_t>(count) * sizeof(GLenum))) : nullptr;
        const auto native = GlProc<PFNGLDISCARDFRAMEBUFFEREXTPROC>("glDiscardFramebufferEXT");
        if (native && (count <= 0 || values)) native(jit.Regs()[0], count, values);
    });
    RegisterThunk("glAlphaFuncQCOM", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const auto native = GlProc<PFNGLALPHAFUNCQCOMPROC>("glAlphaFuncQCOM");
        if (native) native(jit.Regs()[0], ReadSF(jit, 1));
    });
    RegisterThunk("glProgramBinaryOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint length = static_cast<GLint>(jit.Regs()[3]);
        const void* data = length > 0 ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], static_cast<size_t>(length)) : nullptr;
        const auto native = GlProc<PFNGLPROGRAMBINARYOESPROC>("glProgramBinaryOES");
        if (native && (length <= 0 || data)) native(jit.Regs()[0], jit.Regs()[1], data, length);
    });
    RegisterThunk("glGetProgramBinaryOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32(); const GLsizei size = args.I32();
        const uint32_t length_address = args.Ptr(), format_address = args.Ptr(), binary_address = args.Ptr();
        auto* length = length_address ? static_cast<GLsizei*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), length_address, sizeof(GLsizei))) : nullptr;
        auto* format = static_cast<GLenum*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), format_address, sizeof(GLenum)));
        void* binary = size > 0 ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), binary_address, static_cast<size_t>(size)) : nullptr;
        const auto native = GlProc<PFNGLGETPROGRAMBINARYOESPROC>("glGetProgramBinaryOES");
        if (native && (!length_address || length) && format && (size <= 0 || binary)) native(program, size, length, format, binary);
    });
    RegisterGles2CoreThunks(*this);
}
