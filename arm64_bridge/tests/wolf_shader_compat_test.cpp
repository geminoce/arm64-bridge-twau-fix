// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone component/GPU test; argv[1] is the ignored WSC2 evidence corpus.
#include "wolf_shader_compat.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::uint32_t U32(std::istream& in) {
    unsigned char b[4];
    Check(static_cast<bool>(in.read(reinterpret_cast<char*>(b), 4)), "short corpus u32");
    return b[0] | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) |
           (std::uint32_t(b[3]) << 24);
}
std::string Blob(std::istream& in) {
    const auto n = U32(in);
    Check(n < 1024 * 1024, "oversized corpus source");
    std::string s(n, '\0');
    Check(static_cast<bool>(in.read(s.data(), n)), "short corpus source");
    return s;
}
struct Source {
    std::uint32_t category;
    std::string original, repaired;
};
struct Pair {
    std::string comment, vertex, fragment;
    std::uint32_t source_index = 0;
};
struct Corpus {
    std::vector<std::string> headers;
    std::vector<Source> sources;
    // SDF normal/tint; shadow 0/400/1000/1400; linear depth.
    std::array<Pair, 8> pairs;
    explicit Corpus(const char* path) {
        std::ifstream in(path, std::ios::binary);
        char magic[4];
        Check(static_cast<bool>(in.read(magic, 4)) && std::string_view(magic, 4) == "WSC3", "bad corpus magic");
        const auto header_count = U32(in);
        Check(header_count == 4, "unexpected header count");
        for (unsigned i = 0; i < header_count; ++i) headers.push_back(Blob(in));
        const auto source_count = U32(in);
        Check(source_count == 13788, "unexpected source count");
        for (unsigned i = 0; i < source_count; ++i) {
            const auto category = U32(in);
            Check(category <= 5, "unknown source category");
            auto original = Blob(in), repaired = Blob(in);
            Check((category == 0) == repaired.empty(), "invalid source expectation");
            sources.push_back({category, std::move(original), std::move(repaired)});
        }
        const auto references = U32(in);
        Check(references == 362948, "unexpected reference count");
        std::uint32_t changed = 0, unchanged = 0, idempotent = 0, counts[6]{};
        bool negative[6]{};
        for (unsigned i = 0; i < references; ++i) {
            const auto source_index = U32(in), header_index = U32(in);
            const auto comment = Blob(in);
            Check(source_index < sources.size() && header_index < headers.size(), "bad corpus reference");
            const auto& s = sources[source_index];
            const auto& header = headers[header_index];
            const auto result = PrepareWolfShaderSource(comment, header, s.original);
            if ((header_index & 1u) == 0) {
                for (auto& p : pairs) if (!p.comment.empty() && p.comment == comment)
                    p.vertex = comment + header + s.original;
            }
            if (s.category == 0) {
                Check(result.empty(), "unexpected source modification");
                ++unchanged;
                continue;
            }
            std::string expected = comment;
            const auto n = s.category == 2 ? wolf_shader_compat_detail::kDerivativeExtension.size() : 0;
            expected.append(s.repaired, 0, n);
            expected.append(header);
            expected.append(s.repaired, n, s.repaired.size() - n);
            if (result != expected) {
                std::fprintf(stderr, "Mismatch reference %u source %u category %u %s", i, source_index, s.category, comment.c_str());
                throw std::runtime_error("source repair mismatch");
            }
            ++changed; ++counts[s.category];
            Check(PrepareWolfShaderSource(comment, header, result).empty(), "repair is not idempotent");
            ++idempotent;
            if (!negative[s.category]) {
                negative[s.category] = true;
                Check(PrepareWolfShaderSource(comment, header + " ", s.original).empty(), "unknown header accepted");
                Check(PrepareWolfShaderSource("//Other, features: 00000000, variance: 00000000\n", header, s.original).empty(), "unknown effect accepted");
                Check(PrepareWolfShaderSource(comment.substr(0, comment.size() - 1), header, s.original).empty(), "malformed comment accepted");
                Check(PrepareWolfShaderSource(comment, header, {}).empty(), "empty body accepted");
                auto duplicate = s.original + (s.category == 4 || s.category == 5 ? "uniform sampler2D F43;" :
                    s.category == 1 ? "uniform sampler2D F47;" : "uniform sampler2D F55;");
                Check(PrepareWolfShaderSource(comment, header, duplicate).empty(), "duplicate declaration accepted");
                if (s.category == 1 || s.category == 5) {
                    auto color = s.original;
                    const auto marker = color.find(").w;", color.find("texture2D (F47,"));
                    Check(marker != color.npos, "missing SDF fixture marker");
                    color.replace(marker, 4, ").x;");
                    Check(PrepareWolfShaderSource(comment, header, color).empty(), "non-alpha lookup accepted");
                }
                if (s.category == 4 || s.category == 5) {
                    auto wrong_swizzle = s.original;
                    const auto marker = wrong_swizzle.find(".wzy * 2.0", wrong_swizzle.find("texture2D (F43,"));
                    Check(marker != wrong_swizzle.npos, "missing normal decode fixture marker");
                    wrong_swizzle.replace(marker, 4, ".xyz");
                    Check(PrepareWolfShaderSource(comment, header, wrong_swizzle).empty(), "unknown normal swizzle accepted");
                    auto wrong_uv = s.original;
                    const auto uv = wrong_uv.find("texture2D (F43, T1)");
                    Check(uv != wrong_uv.npos, "missing normal UV fixture marker");
                    wrong_uv.replace(uv, std::string_view("texture2D (F43, T1)").size(), "texture2D (F43, T0)");
                    Check(PrepareWolfShaderSource(comment, header, wrong_uv).empty(), "unknown normal lookup accepted");
                    Check(PrepareWolfShaderSource(comment, header, s.repaired).empty(), "repaired normal body is not idempotent");
                }
            }
            std::string_view effect;
            std::uint32_t features = 0, variance = 0;
            Check(wolf_shader_compat_detail::Comment(comment, effect, features, variance), "corpus comment parse");
            int pair = -1;
            if ((s.category == 1 || s.category == 5) && (features & 3u) == 0) pair = header_index == 3 ? 1 : 0;
            if (s.category == 2) pair = 2 + ((variance & 0x1000u) ? 2 : 0) + ((variance & 0x400u) ? 1 : 0);
            if (s.category == 3) pair = 6;
            if ((s.category == 4 || s.category == 5) && features == 0x82087 && variance == 6) pair = 7;
            if (pair >= 0 && pairs[pair].comment.empty()) {
                pairs[pair].comment = comment;
                pairs[pair].fragment = result;
                pairs[pair].source_index = source_index;
            }
        }
        Check(in.peek() == std::char_traits<char>::eof(), "trailing corpus bytes");
        Check(changed == 115213 && unchanged == 247735 && idempotent == changed, "wrong patch coverage");
        Check(counts[1] == 34560 && counts[2] == 12 && counts[3] == 1 &&
              counts[4] == 57600 && counts[5] == 23040, "wrong category coverage");
        for (const auto& p : pairs) Check(!p.vertex.empty() && !p.fragment.empty(), "missing real paired GPU fixture");
        std::printf("PASS corpus sources=%u references=%u changed=%u unchanged=%u idempotent=%u sdf=%u shadow=%u linear=%u normal=%u\n",
                    source_count, references, changed, unchanged, idempotent, counts[1]+counts[5], counts[2], counts[3], counts[4]+counts[5]);
    }
};

struct Context {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    Context() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        Check(eglInitialize(display, nullptr, nullptr), "EGL initialize");
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr; EGLint count = 0;
        Check(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "ES3 RGBA8 pbuffer config");
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint surface_attributes[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        surface = eglCreatePbufferSurface(display, config, surface_attributes);
        Check(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
              eglMakeCurrent(display, surface, surface, context), "EGL current");
        std::printf("GPU %s; %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        glViewport(0, 0, 4, 4);
        for (GLenum state : {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_DITHER}) glDisable(state);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    ~Context() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
};
GLuint Shader(GLenum type, const std::string& source) {
    const auto shader = glCreateShader(type);
    const char* text = source.data(); const GLint size = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &size);
    glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GPU shader compile: %s\n%.100s\n", log, source.c_str());
        glDeleteShader(shader); throw std::runtime_error("GPU shader compile");
    }
    if (source.find("#extension") != source.npos) {
        GLint n = 0; glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &n);
        Check(n > 0 && n < 1024 * 1024, "driver shader source size");
        std::string actual(n, '\0'); glGetShaderSource(shader, n, nullptr, actual.data());
        Check(actual.find("#extension") < actual.find("precision "), "driver receives extension before precision");
    }
    return shader;
}
GLuint Program(const std::string& vertex, const std::string& fragment) {
    const auto vs = Shader(GL_VERTEX_SHADER, vertex), fs = Shader(GL_FRAGMENT_SHADER, fragment);
    const auto program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "A0"); glLinkProgram(program);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{}; glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GPU program link: %s\n", log);
        glDeleteProgram(program); throw std::runtime_error("GPU program link");
    }
    return program;
}
struct Quad {
    GLuint buffer = 0;
    Quad() {
        const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
        glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    }
    ~Quad() { glDeleteBuffers(1, &buffer); }
    std::array<GLubyte, 4> Pixel(GLuint program) const {
        glUseProgram(program); glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLubyte, 4> pixel{}; glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        Check(glGetError() == GL_NO_ERROR, "GPU value draw has no GL error");
        return pixel;
    }
};
GLuint Texture(bool depth, const void* data, unsigned unit) {
    GLuint texture = 0; glGenTextures(1, &texture); glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (depth) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexImage2D(GL_TEXTURE_2D, 0, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, 1, 1, 0,
                 depth ? GL_DEPTH_COMPONENT : GL_RGBA, depth ? GL_FLOAT : GL_UNSIGNED_BYTE, data);
    Check(glGetError() == GL_NO_ERROR, "GPU fixture texture upload");
    return texture;
}
void Uniform4(GLuint program, const char* name, float x, float y, float z = 0, float w = 0) {
    const auto location = glGetUniformLocation(program, name);
    Check(location >= 0, "GPU value uniform active"); glUniform4f(location, x, y, z, w);
}
void Sampler(GLuint program, const char* name, int unit) {
    const auto location = glGetUniformLocation(program, name);
    Check(location >= 0, "GPU value sampler active"); glUniform1i(location, unit);
}
int Byte(double value) { return static_cast<int>(std::lround(std::fmax(0.0, std::fmin(1.0, value)) * 255)); }
void Near(GLubyte actual, int expected, int tolerance, const char* label) {
    if (std::abs(static_cast<int>(actual) - expected) > tolerance) {
        std::fprintf(stderr, "%s actual=%u expected=%d tolerance=%d\n", label, actual, expected, tolerance);
        throw std::runtime_error(label);
    }
}

void GpuContracts(const Corpus& corpus) {
    Context context;
    unsigned programs = 0, value_draws = 0;
    for (const auto& pair : corpus.pairs) {
        const auto program = Program(pair.vertex, pair.fragment);
        glDeleteProgram(program); ++programs;
        std::printf("PASS real paired GPU compile/link %s", pair.comment.c_str());
    }
    Quad quad;
    const std::string sdf_vertex =
        "vhi vec4 T4;\nattribute vec2 A0;\nuhi vec4 Vprobe;\n"
        "void main(){gl_Position=vec4(A0,0.0,1.0);T4=vec4(0.5,0.5,Vprobe.xy);}\n";
    const std::string sdf_body =
        "vhi vec4 T4;\nuniform sampler2D F47;\nvoid main ()\n{\n"
        "  highp float detailAlpha_2;\n  float t4;\n"
        "  t4 = texture2D (F47, T4.xy).w;\n  detailAlpha_2 = t4;\n"
        "  highp float t5;\n  t5 = clamp (((detailAlpha_2 * T4.z) + T4.w), 0.0, 1.0);\n"
        "  gl_FragData[0] = vec4(t5,detailAlpha_2,0.25,1.0);\n}\n";
    for (unsigned tint = 0; tint < 2; ++tint) {
        const auto& pair = corpus.pairs[tint];
        const auto& vh = corpus.headers[tint ? 2 : 0];
        const auto& fh = corpus.headers[tint ? 3 : 1];
        const auto fs = PrepareWolfShaderSource(pair.comment, fh, sdf_body);
        Check(!fs.empty(), "GPU SDF fixture matches repair contract");
        const auto program = Program(vh + sdf_vertex, fs); ++programs;
        glUseProgram(program); Sampler(program, "F47", 0); Uniform4(program, "Vprobe", -50, 25.5f);
        std::array<GLubyte, 4> texel{17, 35, 63, 0};
        const auto texture = Texture(false, texel.data(), 0);
        for (int alpha : {0, 127, 128, 255}) {
            texel[3] = static_cast<GLubyte>(alpha);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
            const auto pixel = quad.Pixel(program); ++value_draws;
            Near(pixel[0], Byte(-50.0 * alpha / 255.0 + 25.5), 2, "GPU highp SDF mask");
            Near(pixel[1], alpha, 1, "GPU SDF preserves sampled alpha");
            Near(pixel[2], 64, 1, "GPU SDF preserves color constant"); Near(pixel[3], 255, 0, "GPU SDF preserves output alpha");
        }
        glDeleteTextures(1, &texture); glDeleteProgram(program);
        std::printf("PASS GPU SDF alpha oracle header=%s\n", tint ? "mediump tint" : "lowp normal");
    }
    const auto& linear = corpus.pairs[6];
    const auto& linear_body = corpus.sources[linear.source_index].repaired;
    const std::string depth_vertex =
        "vhi vec2 T0;\nattribute vec2 A0;\nvoid main(){gl_Position=vec4(A0,0.0,1.0);T0=vec2(0.5);}\n";
    const auto& manual = corpus.pairs[2];
    const auto& manual_body = corpus.sources[manual.source_index].repaired;
    const std::string manual_vertex =
        "vhi vec3 T1;\nvhi vec2 T0;\nattribute vec2 A0;\nuhi vec4 Vprobe;\n"
        "void main(){gl_Position=vec4(A0,0.0,1.0);T0=vec2(0.5);T1=vec3(0.25,0.25,Vprobe.x*0.5);}\n";
    for (unsigned tint = 0; tint < 2; ++tint) {
        // Original depth effects use the normal header. The second header is a
        // default-precision control, not an invented Engine effect variant.
        const auto& vh = corpus.headers[tint ? 2 : 0];
        const auto& fh = corpus.headers[tint ? 3 : 1];
        const auto program = Program(vh + depth_vertex, linear.comment + fh + linear_body); ++programs;
        glUseProgram(program); Sampler(program, "F55", 0); Uniform4(program, "F2F", .75f, .0001f);
        float depth = .7503f;
        const auto texture = Texture(true, &depth, 0);
        for (float value : {.7502f, .7503f, .7504f}) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &value);
            const auto pixel = quad.Pixel(program); ++value_draws;
            const auto expected = Byte(double(.0001f) / (double(value) - double(.75f)));
            for (auto channel : pixel) Near(channel, expected, 2, "GPU highp linear depth");
        }
        glDeleteTextures(1, &texture); glDeleteProgram(program);
        const auto n = wolf_shader_compat_detail::kDerivativeExtension.size();
        const auto shadow = Program(vh + manual_vertex, manual.comment + manual_body.substr(0, n) + fh + manual_body.substr(n)); ++programs;
        glUseProgram(shadow); Sampler(shadow, "F55", 0); Sampler(shadow, "F5D", 1);
        Uniform4(shadow, "Vprobe", .7502f, 0); Uniform4(shadow, "F2F", 0, 1);
        Uniform4(shadow, "F23", 0, 0); Uniform4(shadow, "F1D", 0, 0, 0, 1);
        Uniform4(shadow, "F1F", .2f, .4f, .6f, 1);
        depth = .5f; const auto scene_texture = Texture(true, &depth, 0);
        depth = .7503f; const auto shadow_texture = Texture(true, &depth, 1);
        for (float value : {.7501f, .7503f}) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &value);
            const auto pixel = quad.Pixel(shadow); ++value_draws;
            Near(pixel[0], 51, 1, "GPU shadow preserves red"); Near(pixel[1], 102, 1, "GPU shadow preserves green");
            Near(pixel[2], 153, 1, "GPU shadow preserves blue");
            Near(pixel[3], value < .7502f ? 255 : 0, 0, "GPU highp shadow compare");
        }
        glDeleteTextures(1, &scene_texture); glDeleteTextures(1, &shadow_texture); glDeleteProgram(shadow);
        std::printf("PASS GPU depth reconstruction/compare oracle header=%s\n", tint ? "mediump control" : "lowp original");
    }
    glUseProgram(0);
    Check(glGetError() == GL_NO_ERROR, "GPU contracts finish without GL errors");
    std::printf("PASS GPU programs=%u value_draws=%u\n", programs, value_draws);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "usage: wolf_shader_compat_test shader_corpus.bin");
        Corpus corpus(argv[1]); GpuContracts(corpus);
        std::puts("PASS wolf_shader_compat corpus and GPU contracts");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL wolf_shader_compat %s\n", e.what());
        return 1;
    }
}
