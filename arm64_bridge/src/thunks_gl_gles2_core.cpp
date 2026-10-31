// SPDX-License-Identifier: GPL-3.0-or-later
#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#define LOG_TAG "Arm32Bridge_GLES2Core"
#if ARM32_BRIDGE_ENABLE_LOGGING
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGE(...) ((void)0)
#endif

namespace {

const char* GuestGlName(DynarmicHost& host, uint32_t address, const char* function_name) {
    const char* name = static_cast<const char*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, 1));
    if (name && memchr(name, 0, std::min<size_t>(host.GetGuestMemorySize() - address, 1024 * 1024))) return name;
    LOGE("%s rejected unterminated guest name at 0x%08X", function_name, address);
    return nullptr;
}

bool CheckedArrayBytes(size_t count, size_t components, size_t element_size,
                       size_t* bytes) {
    const uint64_t total = static_cast<uint64_t>(count) *
                           static_cast<uint64_t>(components) *
                           static_cast<uint64_t>(element_size);
    if (total > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    *bytes = static_cast<size_t>(total);
    return true;
}

template <typename T>
const T* GuestInputArray(DynarmicHost& host, uint32_t address, size_t count,
                         size_t components, const char* function_name) {
    size_t bytes = 0;
    if (!CheckedArrayBytes(count, components, sizeof(T), &bytes) ||
        bytes == 0 ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, bytes)) {
        LOGE("%s rejected guest input: address=0x%08X count=%zu components=%zu",
             function_name, address, count, components);
        return nullptr;
    }
    return reinterpret_cast<const T*>(
        GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, bytes));
}

template <typename T>
T* GuestOutputArray(DynarmicHost& host, uint32_t address, size_t count,
                    size_t components, const char* function_name) {
    size_t bytes = 0;
    if (!CheckedArrayBytes(count, components, sizeof(T), &bytes) ||
        bytes == 0 ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, bytes)) {
        LOGE("%s rejected guest output: address=0x%08X count=%zu components=%zu",
             function_name, address, count, components);
        return nullptr;
    }
    return reinterpret_cast<T*>(
        GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, bytes));
}

template <typename T>
T* OptionalGuestOutput(DynarmicHost& host, uint32_t address, size_t count,
                       const char* function_name, bool* valid) {
    if (address == 0) {
        *valid = true;
        return nullptr;
    }
    T* output = GuestOutputArray<T>(host, address, count, 1, function_name);
    *valid = output != nullptr;
    return output;
}

const void* CountedGuestBytes(DynarmicHost& host, uint32_t address,
                              GLsizei byte_count, const char* function_name,
                              bool* valid) {
    if (byte_count <= 0) {
        *valid = true;
        return nullptr;
    }
    const void* input = GuestInputArray<uint8_t>(
        host, address, static_cast<size_t>(byte_count), 1, function_name);
    *valid = input != nullptr;
    return input;
}

template <typename T>
const T* CountedGuestValues(DynarmicHost& host, uint32_t address,
                            GLsizei count, size_t components,
                            const char* function_name, bool* valid) {
    if (count <= 0) {
        *valid = true;
        if (count < 0) return nullptr;
        const T* values = address
            ? static_cast<const T*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, 0))
            : nullptr;
        // shortcut: this avoids Mali's NULL fault only, extend if guest code repeats count-0 matrices.
        static const T empty_values[16]{};
        return values ? values : empty_values;
    }
    const T* input = GuestInputArray<T>(
        host, address, static_cast<size_t>(count), components, function_name);
    *valid = input != nullptr;
    return input;
}

}  // namespace

size_t GetGlStateValueCount(GLenum pname) {
    switch (pname) {
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
        case GL_SMOOTH_POINT_SIZE_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
            return 2;
        case GL_CURRENT_NORMAL:
        case GL_POINT_DISTANCE_ATTENUATION:
            return 3;
        case GL_MODELVIEW_MATRIX:
        case GL_PROJECTION_MATRIX:
        case GL_TEXTURE_MATRIX:
            return 16;
        case GL_CURRENT_COLOR:
        case GL_CURRENT_TEXTURE_COORDS:
        case GL_LIGHT_MODEL_AMBIENT:
        case GL_FOG_COLOR:
        case GL_BLEND_COLOR:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_SCISSOR_BOX:
        case GL_VIEWPORT:
            return 4;
        case GL_COMPRESSED_TEXTURE_FORMATS: {
            GLint count = 0;
            glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        case GL_SHADER_BINARY_FORMATS: {
            GLint count = 0;
            glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &count);
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        case GL_PROGRAM_BINARY_FORMATS: {
            GLint count = 0;
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        default:
            return 1;
    }
}

namespace {

size_t GetVertexAttribValueCount(GLenum pname) {
    return pname == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
}

constexpr size_t TextureByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type, GLint unpack_alignment) {
    if (width <= 0 || height <= 0 ||
        (unpack_alignment != 1 && unpack_alignment != 2 && unpack_alignment != 4 && unpack_alignment != 8)) return 0;
    size_t bytes_per_pixel = 0;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1) {
        bytes_per_pixel = 2;
    } else if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_ALPHA:
            case GL_LUMINANCE: bytes_per_pixel = 1; break;
            case GL_LUMINANCE_ALPHA: bytes_per_pixel = 2; break;
            case GL_RGB: bytes_per_pixel = 3; break;
            case GL_RGBA: bytes_per_pixel = 4; break;
            default: break;
        }
    }
    if (!bytes_per_pixel) return 0;
    const uint64_t row_bytes = static_cast<uint64_t>(width) * bytes_per_pixel;
    const uint64_t row_stride = (row_bytes + unpack_alignment - 1) & ~static_cast<uint64_t>(unpack_alignment - 1);
    const uint64_t total = row_stride * static_cast<uint64_t>(height - 1) + row_bytes;
    return total <= std::numeric_limits<size_t>::max() ? static_cast<size_t>(total) : 0;
}

static_assert(TextureByteCount(1, 2, GL_RGB, GL_UNSIGNED_BYTE, 4) == 7);
static_assert(TextureByteCount(1, 2, GL_RGB, GL_UNSIGNED_BYTE, 1) == 6);
static_assert(TextureByteCount(1, 2, GL_RGBA, GL_UNSIGNED_BYTE, 8) == 12);
static_assert(TextureByteCount(1, 1, GL_RGB, GL_UNSIGNED_BYTE, 8) == 3);
static_assert(TextureByteCount(3, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 4) == 14);
static_assert(TextureByteCount(0, 2, GL_RGB, GL_UNSIGNED_BYTE, 4) == 0);
static_assert(TextureByteCount(1, 2, GL_RGB, GL_UNSIGNED_BYTE, 3) == 0);

uint32_t HostVertexPointerToGuest(DynarmicHost& host, const void* pointer,
                                  GLint array_buffer_binding) {
    if (!pointer) return 0;

    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    if (array_buffer_binding != 0) {
        return value <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(value)
            : 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(host.GetGuestMemoryBase());
    const uintptr_t end = base + host.GetGuestMemorySize();
    if (value < base || value >= end) return 0;
    const uintptr_t offset = value - base;
    return offset <= std::numeric_limits<uint32_t>::max()
        ? static_cast<uint32_t>(offset)
        : 0;
}

}  // namespace

bool BridgeHasGlES3();
GLuint BridgeBoundBuffer(GLenum);

// Byte spans include alignment, row length and skip state. Only a real PBO
// binding changes a guest address into an offset, including offset zero.
bool ResolveBridgeGlPixels(DynarmicHost& host, GLsizei width, GLsizei height,
                           GLenum format, GLenum type, uint32_t address,
                           bool pack, bool nullable, const void** pixels, uint32_t* pixel_buffer = nullptr) {
    const bool es3 = BridgeHasGlES3();
    const GLuint pbo = es3 ? BridgeBoundBuffer(pack ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER) : 0;
    if (pixel_buffer) *pixel_buffer = pbo;
    if (pbo) {
        *pixels = reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
        return true;
    }
    *pixels = nullptr;
    if (width <= 0 || height <= 0 || (nullable && !address)) return true;
    size_t bpp = 0, component_size = 0;
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
        case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: bpp = 4; break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: bpp = 8; break;
        case GL_BYTE: case GL_UNSIGNED_BYTE: component_size = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: component_size = 2; break;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: component_size = 4; break;
        default: return false;
    }
    if (!bpp) {
        size_t components = 0;
        switch (format) {
            case GL_ALPHA: case GL_LUMINANCE: case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT: components = 1; break;
            case GL_LUMINANCE_ALPHA: case GL_RG: case GL_RG_INTEGER: components = 2; break;
            case GL_RGB: case GL_RGB_INTEGER: case GL_SRGB_EXT: components = 3; break;
            case GL_RGBA: case GL_RGBA_INTEGER: case GL_BGRA_EXT: case GL_SRGB_ALPHA_EXT: components = 4; break;
            default: return false;
        }
        bpp = components * component_size;
    }
    GLint alignment = 4, row_length = 0, skip_rows = 0, skip_pixels = 0;
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &alignment);
    if (es3) {
        glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    }
    if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) || row_length < 0 || skip_rows < 0 || skip_pixels < 0) return false;
    const uint64_t row = static_cast<uint64_t>(row_length ? row_length : width) * bpp;
    const uint64_t stride = (row + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
    const uint64_t rows = static_cast<uint64_t>(skip_rows) + static_cast<uint64_t>(height - 1);
    const uint64_t tail = (static_cast<uint64_t>(skip_pixels) + width) * bpp;
    const uint64_t limit = host.GetGuestMemorySize();
    if (tail > limit || (stride && rows > (limit - tail) / stride)) return false;
    const size_t bytes = static_cast<size_t>(rows * stride + tail);
    *pixels = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, bytes);
    return *pixels != nullptr;
}

bool ResolveBridgeGlCompressed(DynarmicHost& host, uint32_t address,
                               GLsizei size, bool nullable, const void** pixels,
                               uint32_t* unpack_buffer = nullptr) {
    const GLuint pbo = BridgeHasGlES3() ? BridgeBoundBuffer(GL_PIXEL_UNPACK_BUFFER) : 0;
    if (unpack_buffer) *unpack_buffer = pbo;
    if (pbo) {
        *pixels = reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
        return true;
    }
    *pixels = nullptr;
    if (size <= 0 || (nullable && !address)) return true;
    *pixels = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, static_cast<size_t>(size));
    return *pixels != nullptr;
}

void RegisterGles2CoreThunks(ThunkManager& manager) {
    manager.RegisterThunk("glBindAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32(), index = args.U32();
        const char* name = GuestGlName(host, args.Ptr(), "glBindAttribLocation");
        if (name) glBindAttribLocation(program, index, name);
    });
    manager.RegisterThunk("glGetAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32();
        const char* name = GuestGlName(host, args.Ptr(), "glGetAttribLocation");
        jit.Regs()[0] = name ? glGetAttribLocation(program, name) : static_cast<uint32_t>(-1);
    });
    manager.RegisterThunk("glGetUniformLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32();
        const char* name = GuestGlName(host, args.Ptr(), "glGetUniformLocation");
        jit.Regs()[0] = name ? glGetUniformLocation(program, name) : static_cast<uint32_t>(-1);
    });
    manager.RegisterThunk("glGenBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        GLuint* names = count > 0 ? GuestOutputArray<GLuint>(host, address, count, 1, "glGenBuffers") : nullptr;
        if (count > 0 && !names) return;
        glGenBuffers(count, names);
    });
    manager.RegisterThunk("glGenFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        GLuint* names = count > 0 ? GuestOutputArray<GLuint>(host, address, count, 1, "glGenFramebuffers") : nullptr;
        if (count > 0 && !names) return;
        glGenFramebuffers(count, names);
    });
    manager.RegisterThunk("glGenRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        GLuint* names = count > 0 ? GuestOutputArray<GLuint>(host, address, count, 1, "glGenRenderbuffers") : nullptr;
        if (count > 0 && !names) return;
        glGenRenderbuffers(count, names);
    });
    manager.RegisterThunk("glGenTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        GLuint* names = count > 0 ? GuestOutputArray<GLuint>(host, address, count, 1, "glGenTextures") : nullptr;
        if (count > 0 && !names) return;
        glGenTextures(count, names);
    });
    manager.RegisterThunk("glDeleteFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        const GLuint* names = count > 0 ? GuestInputArray<GLuint>(host, address, count, 1, "glDeleteFramebuffers") : nullptr;
        if (count > 0 && !names) return;
        glDeleteFramebuffers(count, names);
    });
    manager.RegisterThunk("glDeleteRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        const GLuint* names = count > 0 ? GuestInputArray<GLuint>(host, address, count, 1, "glDeleteRenderbuffers") : nullptr;
        if (count > 0 && !names) return;
        glDeleteRenderbuffers(count, names);
    });
    manager.RegisterThunk("glDeleteTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLsizei count = args.I32();
        const uint32_t address = args.Ptr();
        const GLuint* names = count > 0 ? GuestInputArray<GLuint>(host, address, count, 1, "glDeleteTextures") : nullptr;
        if (count > 0 && !names) return;
        glDeleteTextures(count, names);
    });
    manager.RegisterThunk("glGetActiveAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32(), index = args.U32();
        const GLsizei capacity = args.I32();
        const uint32_t length_address = args.Ptr(), size_address = args.Ptr();
        const uint32_t type_address = args.Ptr(), name_address = args.Ptr();
        if (capacity < 0) { glGetActiveAttrib(program, index, capacity, nullptr, nullptr, nullptr, nullptr); return; }
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(host, length_address, 1, "glGetActiveAttrib", &length_valid);
        GLint* size = GuestOutputArray<GLint>(host, size_address, 1, 1, "glGetActiveAttrib");
        GLenum* type = GuestOutputArray<GLenum>(host, type_address, 1, 1, "glGetActiveAttrib");
        GLchar* name = capacity > 0 ? GuestOutputArray<GLchar>(host, name_address, capacity, 1, "glGetActiveAttrib") : nullptr;
        if (!length_valid || !size || !type || (capacity > 0 && !name)) return;
        glGetActiveAttrib(program, index, capacity, length, size, type, name);
    });
    manager.RegisterThunk("glGetActiveUniform", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = args.U32(), index = args.U32();
        const GLsizei capacity = args.I32();
        const uint32_t length_address = args.Ptr(), size_address = args.Ptr();
        const uint32_t type_address = args.Ptr(), name_address = args.Ptr();
        if (capacity < 0) { glGetActiveUniform(program, index, capacity, nullptr, nullptr, nullptr, nullptr); return; }
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(host, length_address, 1, "glGetActiveUniform", &length_valid);
        GLint* size = GuestOutputArray<GLint>(host, size_address, 1, 1, "glGetActiveUniform");
        GLenum* type = GuestOutputArray<GLenum>(host, type_address, 1, 1, "glGetActiveUniform");
        GLchar* name = capacity > 0 ? GuestOutputArray<GLchar>(host, name_address, capacity, 1, "glGetActiveUniform") : nullptr;
        if (!length_valid || !size || !type || (capacity > 0 && !name)) return;
        glGetActiveUniform(program, index, capacity, length, size, type, name);
    });
    manager.RegisterThunk("glGetIntegerv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum pname = args.U32();
        GLint* data = GuestOutputArray<GLint>(host, args.Ptr(), GetGlStateValueCount(pname), 1, "glGetIntegerv");
        if (data) glGetIntegerv(pname, data);
    });
    manager.RegisterThunk("glGetProgramInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint object = args.U32();
        const GLsizei capacity = args.I32();
        const uint32_t length_address = args.Ptr(), log_address = args.Ptr();
        if (capacity < 0) { glGetProgramInfoLog(object, capacity, nullptr, nullptr); return; }
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(host, length_address, 1, "glGetProgramInfoLog", &length_valid);
        GLchar* log = capacity > 0 ? GuestOutputArray<GLchar>(host, log_address, capacity, 1, "glGetProgramInfoLog") : nullptr;
        if (!length_valid || (capacity > 0 && !log)) return;
        glGetProgramInfoLog(object, capacity, length, log);
    });
    manager.RegisterThunk("glGetShaderInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint object = args.U32();
        const GLsizei capacity = args.I32();
        const uint32_t length_address = args.Ptr(), log_address = args.Ptr();
        if (capacity < 0) { glGetShaderInfoLog(object, capacity, nullptr, nullptr); return; }
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(host, length_address, 1, "glGetShaderInfoLog", &length_valid);
        GLchar* log = capacity > 0 ? GuestOutputArray<GLchar>(host, log_address, capacity, 1, "glGetShaderInfoLog") : nullptr;
        if (!length_valid || (capacity > 0 && !log)) return;
        glGetShaderInfoLog(object, capacity, length, log);
    });
    manager.RegisterThunk("glGetProgramiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint object = args.U32();
        const GLenum pname = args.U32();
        GLint* data = GuestOutputArray<GLint>(host, args.Ptr(), pname == GL_COMPUTE_WORK_GROUP_SIZE ? 3 : 1, 1, "glGetProgramiv");
        if (data) glGetProgramiv(object, pname, data);
    });
    manager.RegisterThunk("glGetShaderiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint object = args.U32();
        const GLenum pname = args.U32();
        GLint* data = GuestOutputArray<GLint>(host, args.Ptr(), 1, 1, "glGetShaderiv");
        if (data) glGetShaderiv(object, pname, data);
    });
    manager.RegisterThunk("glGetRenderbufferParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint object = args.U32();
        const GLenum pname = args.U32();
        GLint* data = GuestOutputArray<GLint>(host, args.Ptr(), 1, 1, "glGetRenderbufferParameteriv");
        if (data) glGetRenderbufferParameteriv(object, pname, data);
    });
    manager.RegisterThunk("glUniform3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = args.I32();
        const GLsizei count = args.I32();
        bool valid = false;
        const GLfloat* values = CountedGuestValues<GLfloat>(host, args.Ptr(), count, 3, "glUniform3fv", &valid);
        if (valid) glUniform3fv(location, count, values);
    });
    manager.RegisterThunk("glUniform4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = args.I32();
        const GLsizei count = args.I32();
        bool valid = false;
        const GLfloat* values = CountedGuestValues<GLfloat>(host, args.Ptr(), count, 4, "glUniform4fv", &valid);
        if (valid) glUniform4fv(location, count, values);
    });
    manager.RegisterThunk("glUniformMatrix3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = args.I32();
        const GLsizei count = args.I32();
        const GLboolean transpose = static_cast<GLboolean>(args.U32());
        bool valid = false;
        const GLfloat* values = CountedGuestValues<GLfloat>(host, args.Ptr(), count, 9, "glUniformMatrix3fv", &valid);
        if (valid) glUniformMatrix3fv(location, count, transpose, values);
    });
    manager.RegisterThunk("glUniformMatrix4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = args.I32();
        const GLsizei count = args.I32();
        const GLboolean transpose = static_cast<GLboolean>(args.U32());
        bool valid = false;
        const GLfloat* values = CountedGuestValues<GLfloat>(host, args.Ptr(), count, 16, "glUniformMatrix4fv", &valid);
        if (valid) glUniformMatrix4fv(location, count, transpose, values);
    });
    manager.RegisterThunk("glBlendColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLfloat red = args.F32();
        const GLfloat green = args.F32();
        const GLfloat blue = args.F32();
        const GLfloat alpha = args.F32();
        glBlendColor(red, green, blue, alpha);
    });
    manager.RegisterThunk("glBlendEquation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        glBlendEquation(static_cast<GLenum>(args.U32()));
    });
    manager.RegisterThunk("glBlendEquationSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum rgb = static_cast<GLenum>(args.U32());
        const GLenum alpha = static_cast<GLenum>(args.U32());
        glBlendEquationSeparate(rgb, alpha);
    });
    manager.RegisterThunk("glClearDepthf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        glClearDepthf(args.F32());
    });
    manager.RegisterThunk("glDepthRangef", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLfloat near_value = args.F32();
        const GLfloat far_value = args.F32();
        glDepthRangef(near_value, far_value);
    });
    manager.RegisterThunk("glTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = args.U32(); const GLint level = args.I32(), x = args.I32(), y = args.I32();
        const GLsizei width = args.I32(), height = args.I32(); const GLenum format = args.U32(), type = args.U32();
        const uint32_t address = args.Ptr(); const void* pixels = nullptr;
        uint32_t pbo = 0;
        const bool resolved = ResolveBridgeGlPixels(host, width, height, format, type, address, false, false, &pixels, &pbo);
        if (resolved) glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
    });
    manager.RegisterThunk("glClearStencil", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        glClearStencil(static_cast<GLint>(args.I32()));
    });
    manager.RegisterThunk("glCompressedTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = args.U32(); const GLint level = args.I32(), x = args.I32(), y = args.I32();
        const GLsizei width = args.I32(), height = args.I32(); const GLenum format = args.U32();
        const GLsizei size = args.I32(); const uint32_t address = args.Ptr(); const void* data = nullptr;
        if (ResolveBridgeGlCompressed(host, address, size, false, &data))
            glCompressedTexSubImage2D(target, level, x, y, width, height, format, size, data);
    });
    manager.RegisterThunk("glCopyTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLenum internal_format = static_cast<GLenum>(args.U32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLint border = static_cast<GLint>(args.I32());
        glCopyTexImage2D(target, level, internal_format, x, y, width, height, border);
    });
    manager.RegisterThunk("glCopyTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint xoffset = static_cast<GLint>(args.I32());
        const GLint yoffset = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    });
    manager.RegisterThunk("glDetachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLuint shader = static_cast<GLuint>(args.U32());
        glDetachShader(program, shader);
    });

    manager.RegisterThunk("glGetAttachedShaders", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei max_count = static_cast<GLsizei>(args.I32());
        const uint32_t count_address = args.Ptr();
        const uint32_t shaders_address = args.Ptr();
        bool count_valid = false;
        GLsizei* count = OptionalGuestOutput<GLsizei>(
            host, count_address, 1, "glGetAttachedShaders", &count_valid);
        if (!count_valid) return;
        GLuint* shaders = nullptr;
        if (max_count > 0) {
            shaders = GuestOutputArray<GLuint>(
                host, shaders_address, static_cast<size_t>(max_count), 1,
                "glGetAttachedShaders");
            if (!shaders) return;
        }
        glGetAttachedShaders(program, max_count, count, shaders);
    });
    manager.RegisterThunk("glGetBufferParameteriv", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), 1, 1, "glGetBufferParameteriv");
        if (!params) return;
        glGetBufferParameteriv(target, pname, params);
    });
    manager.RegisterThunk("glGetFloatv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const size_t count = GetGlStateValueCount(pname);
        GLfloat* data = GuestOutputArray<GLfloat>(
            host, args.Ptr(), count, 1, "glGetFloatv");
        if (!data) return;
        glGetFloatv(pname, data);
    });
    manager.RegisterThunk("glGetShaderPrecisionFormat", [](Dynarmic::A32::Jit& jit,
                                                      DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum shader_type = static_cast<GLenum>(args.U32());
        const GLenum precision_type = static_cast<GLenum>(args.U32());
        GLint* range = GuestOutputArray<GLint>(
            host, args.Ptr(), 2, 1, "glGetShaderPrecisionFormat");
        GLint* precision = GuestOutputArray<GLint>(
            host, args.Ptr(), 1, 1, "glGetShaderPrecisionFormat");
        if (!range || !precision) return;
        glGetShaderPrecisionFormat(shader_type, precision_type, range, precision);
    });
    manager.RegisterThunk("glGetShaderSource", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint shader = static_cast<GLuint>(args.U32());
        const GLsizei buffer_size = static_cast<GLsizei>(args.I32());
        const uint32_t length_address = args.Ptr();
        const uint32_t source_address = args.Ptr();
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(
            host, length_address, 1, "glGetShaderSource", &length_valid);
        if (!length_valid) return;
        GLchar* source = nullptr;
        if (buffer_size > 0) {
            source = GuestOutputArray<GLchar>(
                host, source_address, static_cast<size_t>(buffer_size), 1,
                "glGetShaderSource");
            if (!source) return;
        }
        glGetShaderSource(shader, buffer_size, length, source);
    });
    manager.RegisterThunk("glGetTexParameterfv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), pname == GL_TEXTURE_CROP_RECT_OES ? 4 : 1, 1, "glGetTexParameterfv");
        if (!params) return;
        glGetTexParameterfv(target, pname, params);
    });
    manager.RegisterThunk("glGetTexParameteriv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), pname == GL_TEXTURE_CROP_RECT_OES ? 4 : 1, 1, "glGetTexParameteriv");
        if (!params) return;
        glGetTexParameteriv(target, pname, params);
    });
    manager.RegisterThunk("glGetUniformfv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLint location = static_cast<GLint>(args.I32());
        // A single GLES2 uniform location contains at most one mat4 (16 scalars).
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), 16, 1, "glGetUniformfv");
        if (!params) return;
        glGetUniformfv(program, location, params);
    });
    manager.RegisterThunk("glGetUniformiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLint location = static_cast<GLint>(args.I32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), 16, 1, "glGetUniformiv");
        if (!params) return;
        glGetUniformiv(program, location, params);
    });
    manager.RegisterThunk("glGetVertexAttribfv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), GetVertexAttribValueCount(pname), 1,
            "glGetVertexAttribfv");
        if (!params) return;
        glGetVertexAttribfv(index, pname, params);
    });
    manager.RegisterThunk("glGetVertexAttribiv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), GetVertexAttribValueCount(pname), 1,
            "glGetVertexAttribiv");
        if (!params) return;
        glGetVertexAttribiv(index, pname, params);
    });
    manager.RegisterThunk("glGetVertexAttribPointerv", [](Dynarmic::A32::Jit& jit,
                                                     DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        uint32_t* guest_pointer = GuestOutputArray<uint32_t>(
            host, args.Ptr(), 1, 1, "glGetVertexAttribPointerv");
        if (!guest_pointer) return;
        void* host_pointer = nullptr;
        GLint array_buffer_binding = 0;
        glGetVertexAttribPointerv(index, pname, &host_pointer);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &array_buffer_binding);
        *guest_pointer = HostVertexPointerToGuest(
            host, host_pointer, array_buffer_binding);
    });

    manager.RegisterThunk("glIsBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(glIsBuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glIsEnabled", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(glIsEnabled(static_cast<GLenum>(args.U32())));
    });
    manager.RegisterThunk("glIsFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(
            glIsFramebuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glIsProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(glIsProgram(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glIsRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(
            glIsRenderbuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glIsShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(glIsShader(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glIsTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        jit.Regs()[0] = static_cast<uint32_t>(glIsTexture(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunk("glReleaseShaderCompiler", [](Dynarmic::A32::Jit&, DynarmicHost&) {
        glReleaseShaderCompiler();
    });
    manager.RegisterThunk("glSampleCoverage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLfloat value = args.F32();
        const GLboolean invert = static_cast<GLboolean>(args.U32());
        glSampleCoverage(value, invert);
    });

    manager.RegisterThunk("glStencilFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum function = static_cast<GLenum>(args.U32());
        const GLint reference = static_cast<GLint>(args.I32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilFunc(function, reference, mask);
    });
    manager.RegisterThunk("glStencilFuncSeparate", [](Dynarmic::A32::Jit& jit,
                                                 DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLenum function = static_cast<GLenum>(args.U32());
        const GLint reference = static_cast<GLint>(args.I32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilFuncSeparate(face, function, reference, mask);
    });
    manager.RegisterThunk("glStencilMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        glStencilMask(static_cast<GLuint>(args.U32()));
    });
    manager.RegisterThunk("glStencilMaskSeparate", [](Dynarmic::A32::Jit& jit,
                                                 DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilMaskSeparate(face, mask);
    });
    manager.RegisterThunk("glStencilOp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum fail = static_cast<GLenum>(args.U32());
        const GLenum depth_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_pass = static_cast<GLenum>(args.U32());
        glStencilOp(fail, depth_fail, depth_pass);
    });
    manager.RegisterThunk("glStencilOpSeparate", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLenum stencil_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_pass = static_cast<GLenum>(args.U32());
        glStencilOpSeparate(face, stencil_fail, depth_fail, depth_pass);
    });
    manager.RegisterThunk("glTexParameterfv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const GLfloat* params = GuestInputArray<GLfloat>(
            host, args.Ptr(), pname == GL_TEXTURE_CROP_RECT_OES ? 4 : 1, 1, "glTexParameterfv");
        if (!params) return;
        glTexParameterfv(target, pname, params);
    });

    manager.RegisterThunk("glUniform1fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 1, "glUniform1fv", &valid);
        if (!valid) return;
        glUniform1fv(location, count, value);
    });
    manager.RegisterThunk("glUniform1iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 1, "glUniform1iv", &valid);
        if (!valid) return;
        glUniform1iv(location, count, value);
    });
    manager.RegisterThunk("glUniform2fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 2, "glUniform2fv", &valid);
        if (!valid) return;
        glUniform2fv(location, count, value);
    });
    manager.RegisterThunk("glUniform2i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        glUniform2i(location, x, y);
    });
    manager.RegisterThunk("glUniform2iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 2, "glUniform2iv", &valid);
        if (!valid) return;
        glUniform2iv(location, count, value);
    });
    manager.RegisterThunk("glUniform3i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLint z = static_cast<GLint>(args.I32());
        glUniform3i(location, x, y, z);
    });
    manager.RegisterThunk("glUniform3iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 3, "glUniform3iv", &valid);
        if (!valid) return;
        glUniform3iv(location, count, value);
    });
    manager.RegisterThunk("glUniform4i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLint z = static_cast<GLint>(args.I32());
        const GLint w = static_cast<GLint>(args.I32());
        glUniform4i(location, x, y, z, w);
    });
    manager.RegisterThunk("glUniform4iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 4, "glUniform4iv", &valid);
        if (!valid) return;
        glUniform4iv(location, count, value);
    });
    manager.RegisterThunk("glUniformMatrix2fv", [](Dynarmic::A32::Jit& jit,
                                              DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLboolean transpose = static_cast<GLboolean>(args.U32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 4, "glUniformMatrix2fv", &valid);
        if (!valid) return;
        glUniformMatrix2fv(location, count, transpose, value);
    });
    manager.RegisterThunk("glValidateProgram", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        glValidateProgram(static_cast<GLuint>(args.U32()));
    });

    manager.RegisterThunk("glVertexAttrib1f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        glVertexAttrib1f(index, x);
    });
    manager.RegisterThunk("glVertexAttrib1fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 1, "glVertexAttrib1fv");
        if (!values) return;
        glVertexAttrib1fv(index, values);
    });
    manager.RegisterThunk("glVertexAttrib2f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        glVertexAttrib2f(index, x, y);
    });
    manager.RegisterThunk("glVertexAttrib2fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 2, "glVertexAttrib2fv");
        if (!values) return;
        glVertexAttrib2fv(index, values);
    });
    manager.RegisterThunk("glVertexAttrib3f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        const GLfloat z = args.F32();
        glVertexAttrib3f(index, x, y, z);
    });
    manager.RegisterThunk("glVertexAttrib3fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 3, "glVertexAttrib3fv");
        if (!values) return;
        glVertexAttrib3fv(index, values);
    });
    manager.RegisterThunk("glVertexAttrib4f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        const GLfloat z = args.F32();
        const GLfloat w = args.F32();
        glVertexAttrib4f(index, x, y, z, w);
    });
    manager.RegisterThunk("glVertexAttrib4fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host.GetGuestMemoryBase());
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 4, "glVertexAttrib4fv");
        if (!values) return;
        glVertexAttrib4fv(index, values);
    });

}
