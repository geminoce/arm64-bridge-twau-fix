// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include "wolf_shader_compat.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

JavaVM* GetHostJavaVM() { return nullptr; }

namespace {
unsigned checks = 0;
void Check(bool ok, const char* label) {
    ++checks;
    std::printf("{\"case\":\"%s\",\"status\":\"%s\"}\n", label, ok ? "PASSED" : "FAILED");
    if (!ok) throw std::runtime_error(label);
}
struct Context {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    Context() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        Check(eglInitialize(display, nullptr, nullptr), "EGL initialized");
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        Check(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "ES2 pbuffer config");
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        const EGLint surface_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        surface = eglCreatePbufferSurface(display, config, surface_attributes);
        Check(context && surface && eglMakeCurrent(display, surface, surface, context), "EGL current");
        std::printf("GPU %s; %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    }
    ~Context() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface) eglDestroySurface(display, surface);
        if (context) eglDestroyContext(display, context);
        eglTerminate(display);
    }
};

uint32_t Allocate(DynarmicHost& host, size_t size) {
    const uint32_t address = host.GuestAlloc(size, 16);
    Check(address != 0, "guest fixture allocation");
    std::memset(host.GetGuestMemoryBase() + address, 0, size);
    return address;
}
void Opcode(DynarmicHost& host, uint32_t address, uint32_t value) {
    host.MemoryWrite32(address, value);
    host.InvalidateGuestCode(address, 4);
}

void ShaderSourceContracts(DynarmicHost& host) {
    const std::string comment = "//Mesh, features: 00080080, variance: 00000000\n";
    const std::string header(wolf_shader_compat_detail::kFragmentHeader);
    const std::string body =
        "vhi vec4 T4;\nuniform sampler2D F47;\nvoid main ()\n{\n"
        "  highp float detailAlpha_2;\n  float t4;\n"
        "  t4 = texture2D (F47, T4.xy).w;\n  detailAlpha_2 = t4;\n"
        "  highp float t5;\n  t5 = clamp (((detailAlpha_2 * T4.z) + T4.w), 0.0, 1.0);\n"
        "  gl_FragColor = vec4(t5);\n}\n";
    const auto expected = PrepareWolfShaderSource(comment, header, body);
    Check(!expected.empty(), "shader fixture exercises scoped precision repair");
    const uint32_t fn = host.GetThunkManager().FindProvider("glShaderSource", ThunkManager::ProviderKind::Function);
    Check(fn != 0, "shader source provider found");
    const GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
    const std::array<std::string, 3> sources{comment, header, body};
    std::array<uint32_t, 3> pointers{};
    std::array<GLint, 3> lengths{};
    for (size_t i = 0; i < sources.size(); ++i) {
        pointers[i] = Allocate(host, sources[i].size() + 16);
        std::memcpy(host.GetGuestMemoryBase() + pointers[i], sources[i].data(), sources[i].size());
        std::memcpy(host.GetGuestMemoryBase() + pointers[i] + sources[i].size(), "\0invalid suffix", 16);
        lengths[i] = static_cast<GLint>(sources[i].size());
    }
    const uint32_t pointer_array = Allocate(host, sizeof(pointers)), length_array = Allocate(host, sizeof(lengths));
    std::memcpy(host.GetGuestMemoryBase() + pointer_array, pointers.data(), sizeof(pointers));
    std::memcpy(host.GetGuestMemoryBase() + length_array, lengths.data(), sizeof(lengths));
    auto source = [&]() {
        GLint length = 0;
        glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
        std::string result(static_cast<size_t>(length), '\0');
        GLsizei written = 0;
        glGetShaderSource(shader, length, &written, result.data());
        result.resize(static_cast<size_t>(written));
        return result;
    };
    host.RunFunction(fn, {shader, 3, pointer_array, length_array});
    Check(source() == expected, "guest shader repair respects explicit lengths");
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    Check(compiled, "guest repaired fragment compiles on GPU");
    lengths.fill(-1);
    std::memcpy(host.GetGuestMemoryBase() + length_array, lengths.data(), sizeof(lengths));
    host.RunFunction(fn, {shader, 3, pointer_array, length_array});
    Check(source() == expected, "guest shader repair respects negative string lengths");
    host.RunFunction(fn, {shader, 3, pointer_array, 0});
    Check(source() == expected, "guest shader repair respects implicit string lengths");
    host.RunFunction(fn, {shader, 1, pointer_array, 0});
    Check(source() == comment, "unrelated shader source passes through exactly");
    Check(glGetError() == GL_NO_ERROR && !host.HasFailed(), "shader source contracts preserve valid GL calls");
    glDeleteShader(shader);
    for (uint32_t address : pointers) host.GuestFree(address);
    host.GuestFree(pointer_array);
    host.GuestFree(length_array);
}

void TextureContracts(DynarmicHost& host, uint32_t base) {
    const uint32_t bind = base + 0x42E650u;
    const uint32_t branch = base + 0x42E678u;
    const uint32_t cache = base + 0xC82624u;
    const uint32_t accepted = host.MemoryRead32(branch);
    Check(accepted == 0x0A000006u || accepted == 0xE320F000u, "texture binding opcode guard");
    const uint32_t sampler = Allocate(host, 4);
    const uint32_t object = Allocate(host, 0x60);
    const uint32_t set_sampler = host.FindSymbol("_ZN19T3SamplerStateBlock23InternalSetSamplerStateEjj");
    Check(set_sampler != 0, "native sampler setter found");
    GLuint textures[3]{};
    glGenTextures(3, textures);
    const uint32_t pixel = 0xFFFFFFFFu;
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    host.MemoryWrite32(object + 0x18, 1);
    host.MemoryWrite32(object + 0x1C, 1);
    host.MemoryWrite32(object + 0x20, 1);
    host.MemoryWrite32(object + 0x50, textures[0]);
    for (bool fixed : {false, true}) {
        Opcode(host, branch, fixed ? 0xE320F000u : 0x0A000006u);
        for (unsigned i = 0; i < 32; ++i) host.MemoryWrite32(cache + i * 4, 0);
        host.MemoryWrite32(object + 0x44, UINT32_MAX);
        host.MemoryWrite32(sampler, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        host.RunFunction(bind, {0x4A, 0, object, sampler});
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        host.RunFunction(set_sampler, {sampler, 2, 1});
        host.RunFunction(bind, {0x4A, 0, object, sampler});
        GLint active = 0, intended_filter = 0, other_filter = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &intended_filter);
        glActiveTexture(GL_TEXTURE1);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &other_filter);
        const bool coherent = active == GL_TEXTURE0 && intended_filter == GL_LINEAR && other_filter == GL_NEAREST;
        Check(fixed ? coherent : !coherent, fixed ? "texture sampler cache hit preserves unit and other texture" : "original wrong-unit sampler update reproduced");
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        host.RunFunction(bind, {0x4A, 0, object, sampler});
        GLint actual = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &actual);
        Check(fixed ? actual == static_cast<GLint>(textures[0]) : actual != static_cast<GLint>(textures[0]),
            fixed ? "texture cache repairs direct resource-upload binding" : "original stale texture binding reproduced");
        Check(glGetError() == GL_NO_ERROR && !host.HasFailed(), "texture contracts preserve valid GL calls");
    }
    Opcode(host, branch, accepted);
    glDeleteTextures(3, textures);
    host.GuestFree(object);
    host.GuestFree(sampler);
}

void VertexContracts(DynarmicHost& host, uint32_t base) {
    const uint32_t bind = base + 0x42E394u, unlock = base + 0x42EA7Cu;
    const uint32_t branch = base + 0x42E3ACu;
    const uint32_t accepted = host.MemoryRead32(branch);
    Check(accepted == 0x0A000075u || accepted == 0xE320F000u, "vertex binding opcode guard");
    const uint32_t va = Allocate(host, 0x48), ib_a = Allocate(host, 0x40), ib_b = Allocate(host, 0x40);
    const uint32_t declaration = Allocate(host, 4), indices = Allocate(host, 8);
    const uint16_t values[3] = {0, 1, 2};
    std::memcpy(host.GetGuestMemoryBase() + indices, values, sizeof(values));
    GLuint buffers[2]{};
    glGenBuffers(2, buffers);
    for (GLuint buffer : buffers) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    }
    host.MemoryWrite32(ib_a + 0x14, buffers[0]);
    host.MemoryWrite32(ib_b + 0x14, buffers[1]);
    host.MemoryWrite32(ib_b + 0x20, 3);
    host.MemoryWrite32(ib_b + 0x24, 2);
    host.MemoryWrite32(ib_b + 0x28, indices);
    host.MemoryWrite32(va + 0x44, ib_a);
    for (bool fixed : {false, true}) {
        Opcode(host, branch, fixed ? 0xE320F000u : 0x0A000075u);
        host.MemoryWrite32(base + 0xC826A4u, 0);
        host.MemoryWrite32(base + 0xC82618u, 0);
        host.RunFunction(bind, {va, 0, declaration});
        GLint actual = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &actual);
        Check(actual == static_cast<GLint>(buffers[0]), "native VA initially binds intended index buffer");
        host.MemoryWrite8(ib_b + 0x18, 1);
        host.RunFunction(unlock, {ib_b});
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &actual);
        Check(actual == static_cast<GLint>(buffers[1]), "native index upload changes actual binding");
        host.RunFunction(bind, {va, 0, declaration});
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &actual);
        Check(fixed ? actual == static_cast<GLint>(buffers[0]) : actual != static_cast<GLint>(buffers[0]),
            fixed ? "vertex cache repairs binding after native index upload" : "original stale index binding reproduced");
        Check(glGetError() == GL_NO_ERROR && !host.HasFailed(), "vertex contracts preserve valid GL calls");
    }
    Opcode(host, branch, accepted);
    glDeleteBuffers(2, buffers);
    for (uint32_t address : {va, ib_a, ib_b, declaration, indices}) host.GuestFree(address);
}

void PrimitiveContracts(DynarmicHost& host, uint32_t base) {
    constexpr uint32_t patches[][3] = {
        {0x42E6F8, 0xEA000015, 0xE3A01000}, {0x42E6FC, 0xEA000005, 0xEA000014},
        {0x42E750, 0xEAFFFFF0, 0xEAFFFFFF}, {0x42E754, 0xE3A01000, 0x92400001},
        {0x42E778, 0xEA000012, 0xE3A02000}, {0x42E77C, 0xEA000005, 0xEA000011},
        {0x42E7C4, 0xEAFFFFF3, 0xEAFFFFFF}, {0x42E7C8, 0xE3A02000, 0x92400001},
    };
    std::array<uint32_t, std::size(patches)> accepted{};
    for (size_t i = 0; i < accepted.size(); ++i) {
        accepted[i] = host.MemoryRead32(base + patches[i][0]);
        Check(accepted[i] == patches[i][1] || accepted[i] == patches[i][2], "primitive opcode guard");
    }
    auto shader = [](GLenum type, const char* source) {
        const GLuint value = glCreateShader(type);
        glShaderSource(value, 1, &source, nullptr);
        glCompileShader(value);
        GLint ok = 0;
        glGetShaderiv(value, GL_COMPILE_STATUS, &ok);
        Check(ok, "primitive fixture shader compiles");
        return value;
    };
    const GLuint vs = shader(GL_VERTEX_SHADER,
        "attribute vec2 A0; void main(){gl_Position=vec4(A0,0.0,1.0);gl_PointSize=1.0;}");
    const GLuint fs = shader(GL_FRAGMENT_SHADER,
        "precision mediump float; void main(){gl_FragColor=vec4(1.0);}");
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "A0");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    Check(linked, "primitive fixture program links");
    glUseProgram(program);
    const GLfloat vertices[] = {-.75f, -.75f, .75f, -.75f, -.75f, .75f, .75f, .75f};
    const uint16_t indices[] = {0, 1, 2, 3};
    GLuint buffers[2]{};
    glGenBuffers(2, buffers);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glViewport(0, 0, 16, 16);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    const uint32_t inst = Allocate(host, 0x54), node = Allocate(host, 12), ib = Allocate(host, 0x28);
    host.MemoryWrite32(inst + 0x50, node);
    host.MemoryWrite32(ib + 0x24, 2);
    for (bool fixed : {false, true}) {
        for (const auto& patch : patches) Opcode(host, base + patch[0], patch[fixed ? 2 : 1]);
        for (bool indexed : {false, true}) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexed ? buffers[1] : 0);
            for (uint16_t mode : {1, 2}) {
                const uint16_t primitive[] = {0, static_cast<uint16_t>(mode == 1 ? 1 : 2), mode, 0};
                std::memcpy(host.GetGuestMemoryBase() + node, primitive, sizeof(primitive));
                glClear(GL_COLOR_BUFFER_BIT);
                host.RunFunction(base + 0x42E6CCu, {inst, indexed ? ib : 0});
                std::array<uint8_t, 16 * 16 * 4> pixels{};
                glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                unsigned lit = 0, gap = 0;
                for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
                    if (pixels[(y * 16 + x) * 4] > 128) {
                        ++lit;
                        if (x >= 5 && x <= 10 && y >= 5 && y <= 10) ++gap;
                    }
                }
                Check(mode == 1 ? (fixed ? lit > 0 : lit == 0) : (fixed ? lit > 0 && gap == 0 : gap > 0),
                    mode == 1 ? (fixed ? "native point list renders a point" : "original point list omission reproduced")
                              : (fixed ? "native independent lines preserve the gap" : "original spurious line connections reproduced"));
                Check(glGetError() == GL_NO_ERROR && !host.HasFailed(), "primitive contracts preserve valid GL calls");
            }
        }
    }
    for (size_t i = 0; i < accepted.size(); ++i) Opcode(host, base + patches[i][0], accepted[i]);
    for (uint32_t address : {inst, node, ib}) host.GuestFree(address);
    glUseProgram(0);
    glDeleteProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteBuffers(2, buffers);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        Context context;
        DynarmicHost host;
        Check(host.Initialize(), "host initialized");
        constexpr const char* names[] = {"libfmodex.so", "libSDL2.so", "libGameEngine.so", "libmain.so"};
        std::array<std::vector<uint8_t>, 4> images;
        std::vector<GuestModuleImage> modules;
        for (size_t i = 0; i < images.size(); ++i) {
            std::ifstream input(std::string(argv[1]) + "/" + names[i], std::ios::binary);
            images[i] = std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
            Check(input && !images[i].empty(), names[i]);
            modules.push_back({names[i], images[i].data(), images[i].size()});
        }
        Check(host.LoadModulesFromMemory(modules), "guest modules relocated without game initializers");
        const auto* engine = host.FindModule("libGameEngine.so");
        Check(engine != nullptr, "engine found");
        const uint32_t sampler_init = host.FindSymbol("_ZN19T3SamplerStateBlock10InitializeEv");
        Check(sampler_init != 0, "native sampler table initializer found");
        host.RunFunction(sampler_init);
        Check(!host.HasFailed(), "native sampler masks initialized without game startup");
        ShaderSourceContracts(host);
        TextureContracts(host, engine->GetBaseAddress());
        VertexContracts(host, engine->GetBaseAddress());
        PrimitiveContracts(host, engine->GetBaseAddress());
        std::printf("RENDERER_STATE_CONTRACTS checks=%u failures=0\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RENDERER_STATE_CONTRACTS FAIL %s\n", error.what());
        return 1;
    }
}
