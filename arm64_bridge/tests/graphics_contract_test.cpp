// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Android test fixture. Keep this file outside the production bridge.
// The caller supplies an initialized DynarmicHost on an ARM64 Android device.
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include "thunks_android.h"
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

bool ResolveBridgeGlCompressed(DynarmicHost&, uint32_t, GLsizei, bool, const void**, uint32_t* = nullptr);
bool BridgeHasGlES3();

namespace {
struct Fixture {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    explicit Fixture(int version) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(display, nullptr, nullptr)) throw std::runtime_error("eglInitialize");
        const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,
            version == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display, attrs, &config, 1, &count) || count != 1) throw std::runtime_error("native config");
        const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION,version,EGL_NONE};
        const EGLint surface_attrs[] = {EGL_WIDTH,2,EGL_HEIGHT,2,EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
        surface = eglCreatePbufferSurface(display, config, surface_attrs);
        if (!context || !surface || !eglMakeCurrent(display, surface, surface, context)) throw std::runtime_error("native context");
    }
    ~Fixture() {
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (surface) eglDestroySurface(display,surface);
        if (context) eglDestroyContext(display,context);
        eglTerminate(display);
    }
};
struct Guest {
    DynarmicHost& host;
    std::vector<uint32_t> allocations;
    ~Guest() { for (uint32_t address : allocations) host.GuestFree(address); }
    uint32_t Bytes(const void* data, size_t bytes) {
        const uint32_t address = host.GuestAlloc(bytes,16);
        if (!address) throw std::runtime_error("guest allocation");
        allocations.push_back(address);
        if (data) memcpy(host.GetGuestMemoryBase()+address,data,bytes);
        else memset(host.GetGuestMemoryBase()+address,0,bytes);
        return address;
    }
    template<typename T,size_t N> uint32_t Array(const T (&values)[N]) { return Bytes(values,sizeof(values)); }
    uint32_t String(const char* value) { return Bytes(value,strlen(value)+1); }
    uint32_t Call(const char* name, std::vector<uint32_t> args={}) {
        const uint32_t fn = host.GetThunkManager().FindProvider(name,ThunkManager::ProviderKind::Function);
        if (!fn) throw std::runtime_error(std::string("missing provider ")+name);
        return host.RunFunction(fn,args);
    }
    uint32_t Word(uint32_t address) { return host.MemoryRead32(address); }
    void Check(bool ok,const char* name) {
        printf("{\"case\":\"%s\",\"status\":\"%s\"}\n",name,ok ? "PASSED" : "FAILED");
        fflush(stdout);
        if (!ok) throw std::runtime_error(name);
    }
    void CleanError(const char* name) {
        const uint32_t error = Call("glGetError");
        if (error != GL_NO_ERROR) printf("{\"case\":\"%s\",\"gl_error\":%u}\n",name,error);
        Check(error==GL_NO_ERROR,name);
    }
};

void GlPointerContracts(Guest& g, GLuint program, GLuint shader) {
    constexpr uint32_t guard = 0x41414141;
    const uint32_t edge = static_cast<uint32_t>(g.host.GetGuestMemorySize()-4);
    const struct { const char* generate; const char* remove; } objects[] = {
        {"glGenBuffers","glDeleteBuffers"}, {"glGenFramebuffers","glDeleteFramebuffers"},
        {"glGenRenderbuffers","glDeleteRenderbuffers"}, {"glGenTextures","glDeleteTextures"}};
    for (const auto& object : objects) {
        const uint32_t initial[] = {0,0,guard};
        const uint32_t names = g.Array(initial);
        g.Call(object.generate,{2,names});
        g.Check(g.Word(names) && g.Word(names+4) && g.Word(names+8)==guard,"generated_names_keep_canary");
        g.Call(object.remove,{2,names});
        g.host.MemoryWrite32(edge,guard);
        glEnable(0xFFFFFFFFu);
        g.Call(object.generate,{2,edge});
        g.Call(object.remove,{2,edge});
        g.Check(g.Word(edge)==guard && g.Call("glGetError")==GL_INVALID_ENUM,"object_name_spans_preserve_prior_error");
        g.Call(object.generate,{0,0}); g.Call(object.remove,{0,0});
        g.CleanError("zero_object_count_accepts_null");
        g.Call(object.generate,{UINT32_MAX,0});
        g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"negative_generate_count_reaches_driver");
        g.Call(object.remove,{UINT32_MAX,0});
        g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"negative_delete_count_reaches_driver");
    }

    GLint viewport[4]{}; glGetIntegerv(GL_VIEWPORT,viewport);
    const uint32_t state_initial[] = {0,0,0,0,guard};
    const uint32_t state = g.Array(state_initial);
    g.Call("glGetIntegerv",{GL_VIEWPORT,state});
    g.Check(memcmp(g.host.GetGuestMemoryBase()+state,viewport,sizeof(viewport))==0 &&
        g.Word(state+sizeof(viewport))==guard,"integer_state_uses_complete_vector_span");
    g.host.MemoryWrite32(edge,guard);
    g.Call("glGetIntegerv",{GL_VIEWPORT,edge});
    g.Check(g.Word(edge)==guard,"integer_state_rejects_short_vector_span");
    GLint format_count = 0; glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS,&format_count);
    g.Check(format_count>=0 && format_count<256,"program_binary_format_count_bound");
    const uint32_t formats = g.Bytes(nullptr,(static_cast<size_t>(format_count)+1)*4);
    g.host.MemoryWrite32(formats+format_count*4,guard);
    g.Call("glGetIntegerv",{GL_PROGRAM_BINARY_FORMATS,formats});
    g.Check(g.Word(formats+format_count*4)==guard,"program_binary_formats_keep_canary");

    const struct {
        const char* name;
        void (*native)(GLuint,GLuint,GLsizei,GLsizei*,GLint*,GLenum*,GLchar*);
    } active_queries[] = {{"glGetActiveAttrib",glGetActiveAttrib},{"glGetActiveUniform",glGetActiveUniform}};
    for (const auto& query : active_queries) {
        char expected[64]{}; GLsizei expected_length = 0; GLint expected_size = 0; GLenum expected_type = 0;
        query.native(program,0,sizeof(expected),&expected_length,&expected_size,&expected_type,expected);
        const uint32_t name = g.Bytes(nullptr,68), length = g.Bytes(nullptr,4);
        const uint32_t size = g.Bytes(nullptr,4), type = g.Bytes(nullptr,4);
        g.host.MemoryWrite32(name+64,guard);
        g.Call(query.name,{program,0,64,0,size,type,name});
        g.Check(strcmp(reinterpret_cast<const char*>(g.host.GetGuestMemoryBase()+name),expected)==0 &&
            g.Word(size)==static_cast<uint32_t>(expected_size) && g.Word(type)==expected_type &&
            g.Word(name+64)==guard,"active_query_optional_length_and_canary");
        g.Call(query.name,{program,0,0,length,size,type,0});
        g.Check(g.Word(length)==0,"zero_active_name_capacity_accepts_null");
        g.host.MemoryWrite32(edge,guard);
        g.Call(query.name,{program,0,64,0,size,type,edge});
        g.Check(g.Word(edge)==guard,"active_query_rejects_short_name_span");
        g.Call(query.name,{program,0,0,0,edge+2,type,0});
        g.Check(g.Word(edge)==guard,"active_query_rejects_short_scalar_span");
        g.Call(query.name,{program,0,UINT32_MAX,0,0,0,0});
        g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"negative_active_name_capacity_reaches_driver");
    }
    for (const auto& query : {std::pair{"glGetProgramInfoLog",program},std::pair{"glGetShaderInfoLog",shader}}) {
        const uint32_t log = g.Bytes(nullptr,68), length = g.Bytes(nullptr,4);
        g.host.MemoryWrite32(log+64,guard);
        g.Call(query.first,{query.second,64,0,log});
        g.Check(g.Word(log+64)==guard,"info_log_optional_length_and_canary");
        g.Call(query.first,{query.second,0,length,0});
        g.Check(g.Word(length)==0,"zero_info_log_capacity_accepts_null");
        g.host.MemoryWrite32(edge,guard);
        g.Call(query.first,{query.second,64,0,edge});
        g.Call(query.first,{query.second,0,edge+2,0});
        g.Check(g.Word(edge)==guard,"info_log_rejects_short_output_spans");
        g.Call(query.first,{query.second,UINT32_MAX,0,0});
        g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"negative_info_log_capacity_reaches_driver");
    }

    GLuint renderbuffer = 0; glGenRenderbuffers(1,&renderbuffer); glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA4,2,2);
    const struct { const char* name; GLuint object; GLenum pname; uint32_t expected; } scalar_queries[] = {
        {"glGetProgramiv",program,GL_LINK_STATUS,GL_TRUE}, {"glGetShaderiv",shader,GL_COMPILE_STATUS,GL_TRUE},
        {"glGetRenderbufferParameteriv",GL_RENDERBUFFER,GL_RENDERBUFFER_WIDTH,2}};
    for (const auto& query : scalar_queries) {
        const uint32_t initial[] = {0,guard}; const uint32_t output = g.Array(initial);
        g.Call(query.name,{query.object,query.pname,output});
        g.Check(g.Word(output)==query.expected && g.Word(output+4)==guard,"scalar_query_keeps_canary");
        g.host.MemoryWrite32(edge,guard);
        g.Call(query.name,{query.object,query.pname,edge+2});
        g.Check(g.Word(edge)==guard,"scalar_query_rejects_short_span");
    }
    glDeleteRenderbuffers(1,&renderbuffer);

    const GLint attribute = glGetAttribLocation(program,"p"), uniform = glGetUniformLocation(program,"u");
    g.host.MemoryWrite32(edge,0x78780070);
    g.Check(g.Call("glGetAttribLocation",{program,edge})==static_cast<uint32_t>(attribute),"attribute_name_can_end_at_arena_edge");
    g.host.MemoryWrite32(edge,0x78780075);
    g.Check(g.Call("glGetUniformLocation",{program,edge})==static_cast<uint32_t>(uniform),"uniform_name_can_end_at_arena_edge");
    g.host.MemoryWrite32(edge,guard);
    g.Check(g.Call("glGetAttribLocation",{program,edge})==UINT32_MAX &&
        g.Call("glGetUniformLocation",{program,edge})==UINT32_MAX,"unterminated_names_are_rejected");
    g.Call("glBindAttribLocation",{program,0,edge});

    const float values[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const uint32_t input = g.Array(values);
    const float unused_values[] = {-1,-2,-3,-4,-5,-6,-7,-8,-9,-10,-11,-12,-13,-14,-15,-16};
    const uint32_t zero_input = g.Array(unused_values);
    const struct {
        const char* function; const char* uniform; size_t components;
        void (*vector)(GLint,GLsizei,const GLfloat*);
        void (*matrix)(GLint,GLsizei,GLboolean,const GLfloat*);
    } uploads[] = {
        {"glUniform3fv","vector_three",3,glUniform3fv,nullptr}, {"glUniform4fv","u",4,glUniform4fv,nullptr},
        {"glUniformMatrix3fv","matrix_three",9,nullptr,glUniformMatrix3fv},
        {"glUniformMatrix4fv","matrix_four",16,nullptr,glUniformMatrix4fv}};
    for (const auto& upload : uploads) {
        const GLint location = glGetUniformLocation(program,upload.uniform);
        g.Check(location>=0,"pointer_contract_uniform_is_active");
        std::vector<uint32_t> args{static_cast<uint32_t>(location),1};
        if (upload.matrix) args.push_back(GL_FALSE);
        args.push_back(input);
        g.Call(upload.function,args);
        float actual[16]{}; glGetUniformfv(program,location,actual);
        g.Check(memcmp(actual,values,upload.components*sizeof(float))==0,"uniform_counted_upload_is_exact");
        g.CleanError("valid_uniform_upload_error_free");
        args.back() = edge; g.Call(upload.function,args);
        args[1] = INT32_MAX; args.back() = input; g.Call(upload.function,args);
        glGetUniformfv(program,location,actual);
        g.Check(memcmp(actual,values,upload.components*sizeof(float))==0,"short_and_overflowing_uniform_spans_are_rejected");
        g.CleanError("rejected_uniform_spans_preserve_error_state");
        auto seed_uniform = [&] {
            if (upload.matrix) upload.matrix(location,1,GL_FALSE,values);
            else upload.vector(location,1,values);
            g.CleanError("zero_uniform_seed_upload_error_free");
        };
        for (const bool have_values : {true,false}) {
            // Keep native and bridge probes independent of Mali's broken repeated-count-0 cache.
            seed_uniform();
            printf("{\"case\":\"native_zero_count_reference\",\"function\":\"%s\",\"nonnull\":true}\n",upload.function);
            fflush(stdout);
            if (upload.matrix) upload.matrix(location,0,GL_FALSE,unused_values);
            else upload.vector(location,0,unused_values);
            const GLenum native_error = glGetError();
            g.Check(native_error==GL_NO_ERROR,"native_zero_count_nonnull_reference_succeeds");
            glGetUniformfv(program,location,actual);
            g.Check(memcmp(actual,values,upload.components*sizeof(float))==0,"native_zero_count_keeps_values");
            g.CleanError("native_zero_count_readback_error_free");
            seed_uniform();
            args[1] = 0; args.back() = have_values ? zero_input : 0;
            g.Call(upload.function,args);
            const GLenum bridge_error = g.Call("glGetError");
            printf("{\"case\":\"uniform_zero_count_safe_native_parity\",\"function\":\"%s\","
                   "\"guest_nonnull\":%s,\"native_nonnull\":true,\"native_error\":%u,\"bridge_error\":%u}\n",
                   upload.function,have_values ? "true" : "false",native_error,bridge_error);
            g.Check(bridge_error==native_error,have_values ? "zero_uniform_count_value_matches_native" :
                "zero_uniform_count_null_matches_native");
            glGetUniformfv(program,location,actual);
            g.Check(memcmp(actual,values,upload.components*sizeof(float))==0,"zero_uniform_count_keeps_values");
            g.CleanError("zero_uniform_count_readback_error_free");
        }
        seed_uniform();
        args.back() = 0;
        glEnable(0xFFFFFFFFu);
        g.Call(upload.function,args);
        g.Check(g.Call("glGetError")==GL_INVALID_ENUM,"zero_uniform_count_preserves_prior_error");
        auto check_zero_validation = [&](GLint query_location, GLboolean transpose, const char* test) {
            if (upload.matrix) upload.matrix(query_location,0,transpose,values);
            else upload.vector(query_location,0,values);
            const GLenum native_error = glGetError();
            args[0] = static_cast<uint32_t>(query_location);
            if (upload.matrix) args[2] = transpose;
            g.Call(upload.function,args);
            const GLenum bridge_error = g.Call("glGetError");
            printf("{\"case\":\"%s\",\"function\":\"%s\",\"native_error\":%u,\"bridge_error\":%u}\n",
                   test,upload.function,native_error,bridge_error);
            g.Check(bridge_error==native_error,test);
        };
        check_zero_validation(INT32_MAX,GL_FALSE,"zero_uniform_invalid_location_matches_native");
        glUseProgram(0);
        check_zero_validation(location,GL_FALSE,"zero_uniform_without_program_matches_native");
        glUseProgram(program);
        if (upload.matrix) check_zero_validation(location,GL_TRUE,"zero_uniform_transpose_matches_native");
        args[0] = static_cast<uint32_t>(location);
        if (upload.matrix) args[2] = GL_FALSE;
        args.back() = 0;
        args[1] = UINT32_MAX; g.Call(upload.function,args);
        g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"negative_uniform_count_reaches_driver");
    }
    g.CleanError("gl_pointer_contracts_error_free");
}

void FrameDeadlineContracts(Guest& g) {
    constexpr int64_t ms = 1000000;
    const int64_t p30 = SetBridgeFrameLimit(30,60.0f);
    const int64_t unlimited = SetBridgeFrameLimit(0,60.0f);
    auto timeline = [](int64_t period, const std::vector<int64_t>& costs, int frames) {
        BridgeFrameDeadline state;
        int64_t now = 0;
        for (int i=0; i<frames; ++i) {
            now += costs[i%costs.size()];
            now = std::max(now,state.Deadline(period,now));
            state.Commit(now);
        }
        return now;
    };
    // The same production scheduler is called with a deterministic monotonic
    // clock. Alternating cheap/expensive frames expose max(now,next)+period.
    const int64_t natural = 200*50*ms;
    const int64_t unbounded = timeline(unlimited,{5*ms,95*ms},200);
    const int64_t capped = timeline(p30,{5*ms,95*ms},200);
    printf("{\"case\":\"frame_limit_weak_target_measurement\",\"synthetic\":true,"
           "\"natural_ns\":%lld,\"unlimited_ns\":%lld,\"cap30_ns\":%lld}\n",
           static_cast<long long>(natural),static_cast<long long>(unbounded),static_cast<long long>(capped));
    g.Check(std::abs(static_cast<double>(unbounded)/capped-1.0)<=0.03 &&
            std::abs(static_cast<double>(natural)/capped-1.0)<=0.03,
            "frame_limit_weak_target_matches_unlimited_3pct");
    for (const std::vector<int64_t> costs : {std::vector<int64_t>{7*ms,83*ms},
            std::vector<int64_t>{12*ms,68*ms},std::vector<int64_t>{21*ms,59*ms}}) {
        g.Check(std::abs(static_cast<double>(timeline(unlimited,costs,200))/
                timeline(p30,costs,200)-1.0)<=0.03,"frame_limit_weak_jitter_profiles");
    }
    for (const int cap : {30,45,60,0}) {
        const int64_t period = SetBridgeFrameLimit(cap,120.0f);
        BridgeFrameDeadline state;
        int64_t now = 0, previous = 0;
        bool cadence = true;
        for (int i=0; i<200; ++i) {
            now += ms;
            now = std::max(now,state.Deadline(period,now));
            state.Commit(now);
            if (i) cadence &= now-previous==period;
            previous = now;
        }
        g.Check(cadence,"frame_limit_strong_target_exact_phase");
    }
    BridgeFrameDeadline state;
    int64_t now = 0;
    bool debt_bounded = true;
    for (int i=0; i<2000; ++i) {
        now += 95*ms;
        const int64_t due = state.Deadline(p30,now);
        debt_bounded &= due<=now;
        state.Commit(now);
        debt_bounded &= state.next_ns>=now-4*p30 && state.next_ns<=now+p30;
    }
    g.Check(debt_bounded,"frame_limit_ordinary_overload_does_not_reset_phase");
    const int64_t light_start = now;
    int recovery_frames = 0;
    while (state.next_ns<now && recovery_frames<7) {
        now += 5*ms;
        now = std::max(now,state.Deadline(p30,now));
        state.Commit(now);
        ++recovery_frames;
    }
    g.Check(state.next_ns>=now && now-light_start<500*ms && recovery_frames<=6,
            "frame_limit_heavy_to_light_bounded_recovery");
    now += ms;
    const int64_t stable_due = state.Deadline(p30,now);
    now = std::max(now,stable_due);
    state.Commit(now);
    g.Check(state.next_ns==stable_due+p30,"frame_limit_phase_after_recovery");
    now += 1000*ms;
    g.Check(state.Deadline(p30,now)==now,"frame_limit_pause_uses_last_call_not_debt");
    state.Commit(now);
    g.Check(state.Deadline(p30,now+ms)==now+p30,"frame_limit_pause_has_no_catchup");
    state.Commit(now+1000*ms); // Synthetic oversleep of the requested wait.
    g.Check(state.next_ns==now+1000*ms+p30,"frame_limit_long_oversleep_resets_phase");
    SetBridgeFrameLimit(60,60.0f);
}

void FrameLimitContracts(Guest& g, const Fixture& fixture, uint32_t display,
                         uint32_t surface, uint32_t context) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    g.Check(SetBridgeFrameLimit(30,120.0f)==33333333 &&
            SetBridgeFrameLimit(45,120.0f)==22222222 &&
            SetBridgeFrameLimit(60,120.0f)==16666667 &&
            SetBridgeFrameLimit(0,120.0f)==8333333,"frame_limit_exact_caps");
    g.Check(SetBridgeFrameLimit(30,20.0f)==50000000 &&
            SetBridgeFrameLimit(60,59.94f)==16683350 &&
            SetBridgeFrameLimit(0,59.94f)==16683350,"frame_limit_actual_fractional_rate");
    g.Check(SetBridgeFrameLimit(44,120.0f)==16666667 &&
            SetBridgeFrameLimit(-1,120.0f)==16666667 &&
            SetBridgeFrameLimit(120,120.0f)==16666667,"frame_limit_invalid_cap_defaults_60");
    bool invalid_rates = true;
    for (const float hz : {0.0f,-60.0f,0.5f,1001.0f,
             std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        invalid_rates &= SetBridgeFrameLimit(0,hz)==16666667;
    g.Check(invalid_rates,"frame_limit_invalid_rate_defaults_60");

    eglGetError();
    const EGLBoolean expected = eglSwapBuffers(fixture.display,fixture.surface);
    const EGLint expected_error = eglGetError();
    g.Check(expected==EGL_TRUE,"frame_limit_native_swap_fixture");
    g.Call("eglSwapBuffers",{display,surface}); // Warm the actual guest/JIT path.
    g.Call("eglGetError");
    for (const int cap : {30,45,60,0}) {
        const auto period = std::chrono::nanoseconds(SetBridgeFrameLimit(cap,120.0f));
        g.Check(g.Call("eglMakeCurrent",{display,surface,surface,context})==EGL_TRUE,
                "frame_limit_make_current");
        std::array<Clock::time_point,3> done{};
        bool same_result = true;
        const auto start = Clock::now();
        for (size_t i=0; i<done.size(); ++i) {
            same_result &= g.Call("eglSwapBuffers",{display,surface})==expected;
            done[i] = Clock::now();
        }
        const auto first_gap = done[1]-done[0], second_gap = done[2]-done[1];
        const std::string name = "frame_limit_runtime_cap_"+std::to_string(cap);
        g.Check(same_result && g.Call("eglGetError")==static_cast<uint32_t>(expected_error) &&
                first_gap >= period-2ms && second_gap >= period-2ms &&
                done[2]-start < 1s,name.c_str());
    }

    SetBridgeFrameLimit(30,120.0f);
    g.Call("eglSwapBuffers",{display,surface});
    SetBridgeFrameLimit(30,90.0f); // Same target period must not bypass the current cap.
    const auto unchanged_start = Clock::now();
    g.Call("eglSwapBuffers",{display,surface});
    g.Check(Clock::now()-unchanged_start>=31ms && Clock::now()-unchanged_start<1s,
            "frame_limit_unchanged_period_keeps_deadline");

    // A one-second deadline makes skipped/reset pacing detectable with wide bounds.
    SetBridgeFrameLimit(0,1.0f);
    g.Call("eglSwapBuffers",{display,surface});
    const EGLBoolean invalid_result = eglSwapBuffers(fixture.display,EGL_NO_SURFACE);
    const EGLint invalid_error = eglGetError();
    const auto invalid_start = Clock::now();
    const uint32_t invalid_guest = g.Call("eglSwapBuffers",{display,0});
    const auto invalid_delay = Clock::now()-invalid_start;
    g.Check(invalid_guest==invalid_result &&
            g.Call("eglGetError")==static_cast<uint32_t>(invalid_error) && invalid_delay<250ms,
            "frame_limit_invalid_swap_skips_wait_preserves_result_error");
    const EGLBoolean invalid_display_result = eglSwapBuffers(EGL_NO_DISPLAY,fixture.surface);
    const EGLint invalid_display_error = eglGetError();
    const auto invalid_display_start = Clock::now();
    const uint32_t invalid_display_guest = g.Call("eglSwapBuffers",{0,surface});
    g.Check(invalid_display_guest==invalid_display_result &&
            g.Call("eglGetError")==static_cast<uint32_t>(invalid_display_error) &&
            Clock::now()-invalid_display_start<250ms,"frame_limit_invalid_display_skips_wait");
    g.Check(eglMakeCurrent(fixture.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT)==EGL_TRUE,
            "frame_limit_native_unbind_fixture");
    const EGLBoolean unbound_result = eglSwapBuffers(fixture.display,fixture.surface);
    const EGLint unbound_error = eglGetError();
    const auto unbound_start = Clock::now();
    const uint32_t unbound_guest = g.Call("eglSwapBuffers",{display,surface});
    g.Check(unbound_guest==unbound_result &&
            g.Call("eglGetError")==static_cast<uint32_t>(unbound_error) &&
            Clock::now()-unbound_start<250ms,"frame_limit_no_current_context_skips_wait");
    g.Check(g.Call("eglMakeCurrent",{display,surface,surface,context})==EGL_TRUE,
            "frame_limit_successful_make_current_reset");
    const auto reset_start = Clock::now();
    g.Check(g.Call("eglSwapBuffers",{display,surface})==expected && Clock::now()-reset_start<250ms,
            "frame_limit_make_current_cancels_old_deadline");
    SetBridgeFrameLimit(0,2.0f);
    const auto changed_start = Clock::now();
    g.Check(g.Call("eglSwapBuffers",{display,surface})==expected && Clock::now()-changed_start<250ms,
            "frame_limit_period_change_cancels_old_deadline");

    SetBridgeFrameLimit(30,120.0f);
    g.Call("eglSwapBuffers",{display,surface});
    std::this_thread::sleep_for(200ms);
    g.Call("eglSwapBuffers",{display,surface});
    const auto post_stall_start = Clock::now();
    g.Call("eglSwapBuffers",{display,surface});
    g.Check(Clock::now()-post_stall_start>=31ms && Clock::now()-post_stall_start<1s,
            "frame_limit_long_stall_has_no_catchup_burst");

    // Compare the driver's sticky-error behavior, including a paced valid swap.
    eglGetError();
    eglQueryString(EGL_NO_DISPLAY,0x7fffffff);
    const EGLBoolean native_sticky_result = eglSwapBuffers(fixture.display,fixture.surface);
    const EGLint native_sticky_error = eglGetError();
    eglQueryString(EGL_NO_DISPLAY,0x7fffffff);
    g.Check(g.Call("eglSwapBuffers",{display,surface})==native_sticky_result &&
            g.Call("eglGetError")==static_cast<uint32_t>(native_sticky_error),
            "frame_limit_pacing_preserves_prior_egl_error");
    SetBridgeFrameLimit(60,60.0f);
}
}

// Returns zero only after all real driver/JIT checks complete. This is not an
// authored-scene/effects/ASTC/window acceptance and does not run in the game.
int RunGraphicsContractTests(DynarmicHost& host) {
    Guest g{host,{}};
    try {
        FrameDeadlineContracts(g);
        g.Check(g.Call("glGetString",{GL_VERSION})==0,"no_context_null_string");
        {
            Fixture fixture(3);
            TrackBridgeGlContext(host,fixture.context,nullptr);
            const uint32_t version = g.Call("glGetString",{GL_VERSION});
            g.Check(version && strcmp(reinterpret_cast<const char*>(host.GetGuestMemoryBase()+version),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)))==0,"late_successful_string");
            g.Check(g.Call("glGetString",{GL_VERSION})==version,"stable_successful_string");
            g.CleanError("string_error_preserved");

            const uint32_t display = g.Call("eglGetCurrentDisplay");
            const uint32_t parent = g.Call("eglGetCurrentContext");
            const uint32_t surface = g.Call("eglGetCurrentSurface",{EGL_DRAW});
            FrameLimitContracts(g,fixture,display,surface,parent);
            const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
                EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
            const uint32_t guards[] = {0,0xDEADBEEF,0xA5A5A5A5,0x12345678};
            const uint32_t configurations = g.Array(guards), count = g.Bytes(nullptr,4);
            g.Check(g.Call("eglChooseConfig",{display,g.Array(attrs),configurations,1,count})==EGL_TRUE &&
                g.Word(configurations) && g.Word(count)==1 && g.Word(configurations+4)==guards[1] &&
                g.Word(configurations+8)==guards[2],"config_output_is_four_bytes");
            const uint32_t config = g.Word(configurations), config_id = g.Bytes(nullptr,4);
            EGLint expected_id = 0;
            eglGetConfigAttrib(fixture.display,fixture.config,EGL_CONFIG_ID,&expected_id);
            g.Check(g.Call("eglGetConfigAttrib",{display,config,EGL_CONFIG_ID,config_id})==EGL_TRUE &&
                g.Word(config_id)==static_cast<uint32_t>(expected_id),"config_handle_resolves");

            const uint32_t bytes[] = {0x04030201,0x08070605,0x0C0B0A09,0x100F0E0D};
            GLuint buffer = 0, other = 0;
            glGenBuffers(1,&buffer); glGenBuffers(1,&other);
            g.Call("glBindBuffer",{GL_ARRAY_BUFFER,buffer});
            g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(bytes),g.Array(bytes),GL_DYNAMIC_DRAW});
            const uint32_t mapped = g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES});
            g.Check(mapped && g.Word(mapped)==bytes[0] && g.Word(mapped+12)==bytes[3],"map_keeps_original_bytes");
            host.MemoryWrite32(mapped+4,0xBEEFFACE);
            g.Call("glBindBuffer",{GL_ARRAY_BUFFER,other});
            g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(bytes),0,GL_DYNAMIC_DRAW});
            g.Check(g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_FALSE &&
                g.Call("glGetError")==GL_INVALID_OPERATION,"unmap_does_not_target_another_buffer");
            const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
            const uint32_t shared = g.Call("eglCreateContext",{display,config,parent,g.Array(context_attrs)});
            g.Check(shared && shared!=parent && g.Call("eglMakeCurrent",{display,surface,surface,shared})==EGL_TRUE,"shared_context_handle");
            g.Call("glBindBuffer",{GL_ARRAY_BUFFER,buffer});
            g.Check(g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_TRUE,"unmap_in_shared_context");
            auto* check = static_cast<const uint32_t*>(glMapBufferRange(GL_ARRAY_BUFFER,0,sizeof(bytes),GL_MAP_READ_BIT));
            g.Check(check && check[0]==bytes[0] && check[1]==0xBEEFFACE && check[2]==bytes[2] && check[3]==bytes[3],"mapped_partial_write_preserves_tail");
            glUnmapBuffer(GL_ARRAY_BUFFER);

            const uint32_t replacement[] = {11,22,33,44,55,66};
            const uint32_t replacement_data = g.Array(replacement);
            g.Check(g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES})!=0,"recreate_initial_mapping");
            g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(replacement),replacement_data,GL_DYNAMIC_DRAW});
            g.CleanError("recreate_mapped_buffer_error_free");
            const uint32_t remapped = g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES});
            g.Check(remapped && memcmp(host.GetGuestMemoryBase()+remapped,replacement,sizeof(replacement))==0,
                "recreated_buffer_remaps_with_new_size_and_data");
            g.Check(g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_TRUE,"recreated_mapping_unmaps");

            g.Check(g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES})!=0,"implicit_unmap_initial_mapping");
            glEnable(0xFFFFFFFFu);
            g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(bytes),g.Array(bytes),GL_DYNAMIC_DRAW});
            g.Check(g.Call("glGetError")==GL_INVALID_ENUM,"buffer_recreation_preserves_prior_error");
            g.Check(g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_FALSE &&
                g.Call("glGetError")==GL_INVALID_OPERATION,"implicit_unmap_rejects_later_unmap");

            const uint32_t retained = g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES});
            g.Check(retained && memcmp(host.GetGuestMemoryBase()+retained,bytes,sizeof(bytes))==0,
                "implicit_unmap_keeps_replacement_data");
            g.Call("glBufferData",{GL_ARRAY_BUFFER,UINT32_MAX,replacement_data,GL_DYNAMIC_DRAW});
            g.Check(g.Call("glGetError")==GL_INVALID_VALUE,"rejected_recreation_preserves_size_error");
            g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(bytes),replacement_data,0xFFFFFFFFu});
            g.Check(g.Call("glGetError")==GL_INVALID_ENUM,"rejected_recreation_preserves_usage_error");
            GLint still_mapped = GL_FALSE;
            glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_MAPPED,&still_mapped);
            g.Check(still_mapped==GL_TRUE && memcmp(host.GetGuestMemoryBase()+retained,bytes,sizeof(bytes))==0,
                "rejected_recreation_keeps_original_mapping");
            host.MemoryWrite32(retained+8,0x1234ABCD);
            g.Check(g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_TRUE,"retained_mapping_unmaps");
            check = static_cast<const uint32_t*>(glMapBufferRange(GL_ARRAY_BUFFER,0,sizeof(bytes),GL_MAP_READ_BIT));
            g.Check(check && check[0]==bytes[0] && check[1]==bytes[1] && check[2]==0x1234ABCD && check[3]==bytes[3],
                "rejected_recreation_preserves_original_data_and_partial_write");
            glUnmapBuffer(GL_ARRAY_BUFFER);
            g.CleanError("buffer_recreation_contracts_error_free");
            glDeleteBuffers(1,&buffer); glDeleteBuffers(1,&other);

            const uint32_t data[] = {0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};
            std::vector<uint8_t> pbo_data(0x10000+sizeof(data));
            memcpy(pbo_data.data()+0x10000,data,sizeof(data));
            GLuint pbo = 0, texture = 0, fbo = 0;
            glGenBuffers(1,&pbo);
            g.Call("glBindBuffer",{GL_PIXEL_UNPACK_BUFFER,pbo});
            const void* compressed_pointer = nullptr;
            uint32_t compressed_pbo = 0;
            g.Check(ResolveBridgeGlCompressed(host,0,16,true,&compressed_pointer,&compressed_pbo) &&
                compressed_pbo==pbo && compressed_pointer==nullptr,"compressed_pbo_zero_is_offset");
            g.Check(ResolveBridgeGlCompressed(host,0x10000,16,true,&compressed_pointer,&compressed_pbo) &&
                compressed_pbo==pbo && reinterpret_cast<uintptr_t>(compressed_pointer)==0x10000,"compressed_high_pbo_offset");
            g.Call("glBufferData",{GL_PIXEL_UNPACK_BUFFER,static_cast<uint32_t>(pbo_data.size()),g.Bytes(pbo_data.data(),pbo_data.size()),GL_STATIC_DRAW});
            glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
            g.Call("glTexImage2D",{GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,0x10000});
            g.Call("glBindBuffer",{GL_PIXEL_UNPACK_BUFFER,0});
            g.Check(ResolveBridgeGlCompressed(host,0,16,true,&compressed_pointer,&compressed_pbo) &&
                compressed_pbo==0 && compressed_pointer==nullptr,"compressed_client_null_is_allocation");
            g.Check(!ResolveBridgeGlCompressed(host,UINT32_MAX,16,true,&compressed_pointer,&compressed_pbo) &&
                compressed_pbo==0,"compressed_client_span_rejected");
            glEnable(0xFFFFFFFFu);
            g.Check(ResolveBridgeGlCompressed(host,0,16,true,&compressed_pointer,&compressed_pbo) &&
                g.Call("glGetError")==GL_INVALID_ENUM,"compressed_resolution_preserves_prior_error");
            glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
            const uint32_t pixels = g.Bytes(nullptr,sizeof(data));
            g.Call("glReadPixels",{0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,pixels});
            g.Check(memcmp(host.GetGuestMemoryBase()+pixels,data,sizeof(data))==0,"high_pbo_offset_is_not_guest_pointer");
            glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&texture); glDeleteBuffers(1,&pbo);

            const char* prefix="#version 100\n";
            const char* vertex="attribute vec4 p;uniform vec4 u;uniform vec3 vector_three;"
                "uniform mat3 matrix_three;uniform mat4 matrix_four;"
                "void main(){gl_Position=matrix_four*vec4(matrix_three*(p.xyz+vector_three),p.w)+u;}\n";
            const uint32_t sources[] = {g.String(prefix),g.String(vertex)};
            const int32_t lengths[] = {static_cast<int32_t>(strlen(prefix)),-1};
            const GLuint vs = g.Call("glCreateShader",{GL_VERTEX_SHADER});
            g.Call("glShaderSource",{vs,2,g.Array(sources),g.Array(lengths)});
            g.Call("glCompileShader",{vs});
            GLint compiled = 0; glGetShaderiv(vs,GL_COMPILE_STATUS,&compiled);
            g.Check(compiled==GL_TRUE,"shader_guest_pointer_array");
            const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            const char* fragment="#version 100\nprecision mediump float;void main(){gl_FragColor=vec4(1.0);}";
            glShaderSource(fs,1,&fragment,nullptr); glCompileShader(fs);
            const GLuint program = glCreateProgram(); glAttachShader(program,vs); glAttachShader(program,fs); glLinkProgram(program);
            GLint linked = 0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
            g.Check(linked==GL_TRUE,"program_link");
            g.Call("glUseProgram",{program});
            const GLint location = glGetUniformLocation(program,"u");
            const float values[] = {1.25f,-2.5f,3.75f,4.5f};
            g.Call("glUniform4f",{static_cast<uint32_t>(location),std::bit_cast<uint32_t>(values[0]),
                std::bit_cast<uint32_t>(values[1]),std::bit_cast<uint32_t>(values[2]),std::bit_cast<uint32_t>(values[3])});
            float readback[4] = {}; glGetUniformfv(program,location,readback);
            g.Check(memcmp(readback,values,sizeof(values))==0,"uniform4f_base_pcs_stack");
            GlPointerContracts(g,program,vs);
            g.CleanError("valid_gl_calls_error_free");
            glEnable(0xFFFFFFFFu);
            g.Call("glDrawArrays",{GL_TRIANGLES,0,0});
            g.Check(g.Call("glGetError")==GL_INVALID_ENUM,"draw_preserves_prior_error");
            glUseProgram(0); glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
            g.Check(g.Call("eglMakeCurrent",{display,surface,surface,parent})==EGL_TRUE &&
                g.Call("eglDestroyContext",{display,shared})==EGL_TRUE,"shared_context_destroy");
            ForgetBridgeGlContext(host,fixture.context);
        }
        {
            Fixture fixture(1);
            TrackBridgeGlContext(host,fixture.context,nullptr);
            const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
            if (extensions && strstr(extensions,"GL_OES_mapbuffer")) {
                g.Check(!BridgeHasGlES3(),"native_oes_map_uses_cpu_preimage_path");
                const uint32_t original[] = {1,2,3,4}, replacement[] = {11,22,33,44};
                GLuint buffer = 0; glGenBuffers(1,&buffer);
                g.Call("glBindBuffer",{GL_ARRAY_BUFFER,buffer});
                g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(original),g.Array(original),GL_DYNAMIC_DRAW});
                const uint32_t replacement_data = g.Array(replacement);
                g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(replacement),replacement_data,0xFFFFFFFFu});
                g.Check(g.Call("glGetError")==GL_INVALID_ENUM,"oes_rejected_recreation_preserves_usage_error");
                const uint32_t mapped = g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES});
                g.Check(mapped && memcmp(host.GetGuestMemoryBase()+mapped,original,sizeof(original))==0,
                    "oes_rejected_recreation_keeps_cpu_preimage");
                g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(replacement),replacement_data,0xFFFFFFFFu});
                g.Check(g.Call("glGetError")==GL_INVALID_ENUM &&
                    g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_TRUE,"oes_rejected_recreation_keeps_native_mapping");
                g.Check(g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES})!=0,"oes_recreate_initial_mapping");
                g.Call("glBufferData",{GL_ARRAY_BUFFER,sizeof(replacement),replacement_data,GL_DYNAMIC_DRAW});
                const uint32_t remapped = g.Call("glMapBufferOES",{GL_ARRAY_BUFFER,GL_WRITE_ONLY_OES});
                g.Check(remapped && memcmp(host.GetGuestMemoryBase()+remapped,replacement,sizeof(replacement))==0 &&
                    g.Call("glUnmapBufferOES",{GL_ARRAY_BUFFER})==GL_TRUE,"oes_recreated_store_remaps_with_new_preimage");
                g.Call("glDeleteBuffers",{1,g.Bytes(&buffer,sizeof(buffer))});
            } else {
                printf("{\"case\":\"native_oes_cpu_preimage_contracts\",\"status\":\"SKIPPED\",\"reason\":\"GL_OES_mapbuffer unavailable\"}\n");
            }
            g.Call("glMatrixMode",{GL_PROJECTION}); g.Call("glLoadIdentity");
            const float values[] = {-2.0f,6.0f,-4.0f,8.0f,0.5f,11.5f};
            std::vector<uint32_t> args;
            for (float value : values) args.push_back(std::bit_cast<uint32_t>(value));
            g.Call("glOrthof",args);
            float matrix[16] = {}; glGetFloatv(GL_PROJECTION_MATRIX,matrix);
            g.Check(std::abs(matrix[0]-0.25f)<0.00001f && std::abs(matrix[5]-1.0f/6.0f)<0.00001f &&
                std::abs(matrix[10]+2.0f/11.0f)<0.00001f && std::abs(matrix[14]+12.0f/11.0f)<0.00001f,
                "gles1_orthof_two_stack_floats");
            g.CleanError("gles1_error_free");
            ForgetBridgeGlContext(host,fixture.context);
        }
        printf("{\"status\":\"PASSED\",\"scope\":\"standalone_graphics_contract\"}\n");
        return 0;
    } catch (const std::exception& error) {
        printf("{\"status\":\"FAILED\",\"reason\":\"%s\"}\n",error.what());
        return 1;
    }
}
