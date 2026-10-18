// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include <jni.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <signal.h>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <algorithm>
#include <zlib.h>
extern "C" int IndexApks(const char* const*, size_t);

// The standalone executable links the production core, without jni_bridge.cpp.
JavaVM* GetHostJavaVM() { return nullptr; }

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
bool Start(DynarmicHost& host) {
    const bool ok = host.Initialize();
    Check(ok && !host.HasFailed() && !host.HasExited(), "Initialize including heap/libc ABI selfchecks");
    return ok;
}
uint32_t Text(DynarmicHost& host, const char* value) {
    const size_t size = std::strlen(value) + 1;
    const uint32_t address = host.GuestAlloc(size);
    Check(address != 0, "guest test string allocation");
    if (address) std::memcpy(host.GetGuestMemoryBase() + address, value, size);
    return address;
}
uint32_t Address(DynarmicHost& host, const char* name) {
    const uint32_t address = host.GetThunkManager().GetOrCreateThunkAddress(name);
    Check(address != 0, name);
    return address;
}
uint32_t Call(DynarmicHost& host, const char* name, std::vector<uint32_t> args) {
    const uint32_t address = Address(host, name);
    return address ? host.RunFunction(address, args) : 0;
}
std::vector<uint32_t> DoubleArgs(double value, uint32_t pointer = 0) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    return {static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32), pointer};
}
double DoubleCall(DynarmicHost& host, const char* name, double value, uint32_t pointer = 0) {
    const uint64_t low = Call(host, name, DoubleArgs(value, pointer));
    const uint64_t bits = low | (uint64_t(host.GetResultHigh()) << 32);
    double result;
    std::memcpy(&result, &bits, 8);
    return result;
}
bool Near(double value, double expected) { return std::fabs(value - expected) <= 2e-15; }

// The wrapper writes a canary only if a nominally nonreturning thunk returns.
uint32_t NonreturnWrapper(DynarmicHost& host, uint32_t target, uint32_t canary) {
    const std::array<uint32_t, 10> words{
        0xE92D4010, 0xE59F4018, 0xE59FC010, 0xE12FFF3C,
        0xE3A03077, 0xE5843000, 0xE3A00077, 0xE8BD8010,
        target, canary};
    const uint32_t code = host.GuestAlloc(sizeof(words), 8);
    if (code) {
        std::memcpy(host.GetGuestMemoryBase() + code, words.data(), sizeof(words));
        host.AddExecutableRange(code, sizeof(words));
        host.InvalidateGuestCode(code, sizeof(words));
    }
    Check(code != 0, "nonreturn ARM wrapper allocation");
    return code;
}

void PositiveContracts() {
    DynarmicHost host;
    if (!Start(host)) return;
    Check(Near(DoubleCall(host, "sinh", 1.0), 1.1752011936438014), "sinh r0:r1 softfp");
    Check(Near(DoubleCall(host, "cosh", -1.0), 1.5430806348152437), "cosh r0:r1 softfp");
    Check(Near(DoubleCall(host, "tanh", 1.0), 0.7615941559557649), "tanh r0:r1 softfp");
    Check(std::signbit(DoubleCall(host, "sinh", -0.0)), "sinh preserves negative zero");
    Check(std::isnan(DoubleCall(host, "tanh", std::numeric_limits<double>::quiet_NaN())), "tanh NaN");
    const uint32_t storage = host.GuestAlloc(32), integral_at = storage + 9;
    std::memset(host.GetGuestMemoryBase() + storage, 0xA5, 32);
    const double fraction = DoubleCall(host, "modf", -3.75, integral_at);
    double integral;
    std::memcpy(&integral, host.GetGuestMemoryBase() + integral_at, 8);
    Check(fraction == -0.75 && integral == -3.0, "modf double input / r2 unaligned output");
    Check(host.MemoryRead8(integral_at - 1) == 0xA5 && host.MemoryRead8(integral_at + 8) == 0xA5,
          "modf writes exactly eight output bytes");
    const uint32_t hex_key = Text(host, "1");
    host.MemoryWrite32(storage + 20, 0xA5A5A5A5);
    Check(Call(host, "strtoul", {hex_key, storage + 20, 16}) == 1 &&
          host.MemoryRead32(storage + 20) == hex_key + 1 &&
          host.MemoryRead8(storage + 19) == 0xA5 && host.MemoryRead8(storage + 24) == 0xA5,
          "strtoul base16 returns value and exact ARM32 guest endptr");
    const uint32_t destination = host.GuestAlloc(16), source = host.GuestAlloc(3);
    std::memset(host.GetGuestMemoryBase() + destination, 0xA5, 16);
    std::memcpy(host.GetGuestMemoryBase() + destination, "A", 2);
    std::memcpy(host.GetGuestMemoryBase() + source, "xyz", 3);
    Check(Call(host, "strncat", {destination, source, 3}) == destination &&
          std::strcmp(reinterpret_cast<char*>(host.GetGuestMemoryBase() + destination), "Axyz") == 0 &&
          host.MemoryRead8(destination + 5) == 0xA5, "strncat exact count without source NUL");
    Check(Call(host, "strncat", {destination, UINT32_MAX, 0}) == destination, "strncat count zero reads no source");
    const uint32_t text = Text(host, "Alpha9!"), reject = Text(host, "0123456789"), empty = Text(host, "");
    Check(Call(host, "strcspn", {text, reject}) == 5 && Call(host, "strcspn", {text, empty}) == 7,
          "strcspn contents and empty reject set");
    Check(host.GuestAlloc(SIZE_MAX) == 0 && host.GuestAlloc(UINT32_MAX) == 0 &&
          host.GuestAlloc(16, 3) == 0 && host.GuestAlloc(16, 0) == 0,
          "heap overflow / invalid alignment refused");
    Check(host.GuestAllocMapping(SIZE_MAX, 4096) == 0 && host.GuestAllocMapping(1, 3) == 0,
          "mapping overflow / alignment refused");
    const uint32_t block = host.GuestAlloc(100);
    host.MemoryWrite32(block, 0xDEADBEEF);
    host.GuestFree(block + 8); host.GuestFree(UINT32_MAX);
    Check(host.GuestAllocSize(block) == 100 && host.GuestAllocSize(UINT32_MAX) == 0, "foreign free does not damage block");
    host.GuestFree(block); host.GuestFree(block);
    const uint32_t reused = host.GuestAlloc(100);
    Check(reused == block && host.MemoryRead32(reused) == 0, "double free and zeroed block reuse");
    Check(Call(host, "calloc", {UINT32_MAX, UINT32_MAX}) == 0, "calloc product cannot truncate into a small allocation");
    Check(!host.HasFailed() && !host.HasExited(), "positive contracts preserve live host");

    const uint32_t format = Text(host, "WOLF_LIBC_DIAG v=%d f=%.2f s=%s"), ok = Text(host, "ok");
    const uint32_t va = host.GuestAlloc(24, 8);
    host.MemoryWrite32(va, 42);
    uint64_t double_bits; const double number = -3.75;
    std::memcpy(&double_bits, &number, 8); host.MemoryWrite64(va + 8, double_bits); host.MemoryWrite32(va + 16, ok);
    constexpr const char* expected = "WOLF_LIBC_DIAG v=42 f=-3.75 s=ok";
    Check(Call(host, "vfprintf", {0x200, format, va}) == std::strlen(expected), "vfprintf typed ARM32 va_list diagnostic output");
    Check(Call(host, "fputs", {Text(host, "WOLF_LIBC_DIAG fputs"), 0x200}) != UINT32_MAX, "fputs diagnostic output");
    const uint32_t bytes = host.GuestAlloc(4);
    std::memcpy(host.GetGuestMemoryBase() + bytes, "x\0y\n", 4);
    Check(Call(host, "fwrite", {bytes, 2, 2, 0x200}) == 2, "fwrite preserves NUL and newline observably, returns item count");
    Check(Call(host, "fputc", {'!', 0x200}) == '!', "fputc diagnostic output");
    Check(Call(host, "fflush", {0}) == 0, "fflush null flushes real host stdio");
    const uint32_t scan_errno = Call(host, "__errno", {});
    host.MemoryWrite32(scan_errno, 0);
    Check(Call(host, "sscanf", {Text(host, "42"), Text(host, "%q"), storage}) == UINT32_MAX &&
          scan_errno != 0 && host.MemoryRead32(scan_errno) == ENOSYS, "unsupported sscanf returns EOF/ENOSYS");
    Check(!host.HasFailed(), "ordinary libc error result permits caller recovery");
}

void FailureContract(const char* name, unsigned variant = 0) {
    DynarmicHost host;
    if (!Start(host)) return;
    const uint32_t marker = host.GuestAlloc(8);
    host.MemoryWrite32(marker, 0xA5A5A5A5);
    uint32_t target;
    std::vector<uint32_t> args;
    if (variant == 1) {
        target = host.GuestAlloc(8);
        host.MemoryWrite32(target, 0xEF002001); host.MemoryWrite32(target + 4, 0xE12FFF1E);
    } else if (variant == 2) {
        host.GetThunkManager().RegisterThunk("__wolf_test_unexpected_halt", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        });
        target = Address(host, "__wolf_test_unexpected_halt");
    } else {
        target = Address(host, name);
        if (!std::strcmp(name, "__assert2")) args = {Text(host, "selfcheck.cpp"), 7, Text(host, "FailureContract"), Text(host, "expected-false")};
        else if (!std::strcmp(name, "modf")) args = DoubleArgs(3.75, static_cast<uint32_t>(host.GetGuestMemorySize() - 4));
        else if (!std::strcmp(name, "strncat")) {
            const uint32_t near_end = static_cast<uint32_t>(host.GetGuestMemorySize() - 2);
            host.MemoryWrite8(near_end, 'A'); host.MemoryWrite8(near_end + 1, 0);
            args = {near_end, Text(host, "B"), 1};
        } else if (!std::strcmp(name, "strcspn")) args = {0, Text(host, "x")};
        else if (!std::strcmp(name, "longjmp") || !std::strcmp(name, "siglongjmp")) args = {marker, 1};
    }
    const uint32_t wrapper = NonreturnWrapper(host, target, marker);
    if (wrapper) host.RunFunction(wrapper, args);
    Check(host.HasFailed() && !host.HasExited(), name);
    Check(host.MemoryRead32(marker) == 0xA5A5A5A5, "failed thunk never returns to ARM continuation");
    Check(!host.RunModuleInitializers() && !host.PrepareThreadContext(), "failure refuses constructors and subsequent context execution");
    Check(host.RunFunction(wrapper, args) == 0 && host.MemoryRead32(marker) == 0xA5A5A5A5,
          "failed host cannot clear halt and resume guest");
}

void ExitContract(const char* name, int status) {
    DynarmicHost host;
    if (!Start(host)) return;
    const uint32_t marker = host.GuestAlloc(8); host.MemoryWrite32(marker, 0xA5A5A5A5);
    const uint32_t wrapper = NonreturnWrapper(host, Address(host, name), marker);
    host.RunFunction(wrapper, {static_cast<uint32_t>(status)});
    Check(host.HasExited() && host.HasFailed() == (status != 0), "exit state is separate from abort failure");
    Check(host.MemoryRead32(marker) == 0xA5A5A5A5 && host.RunFunction(wrapper) == 0, "exit is nonreturn and terminal");
    Check(!host.RunModuleInitializers(), "constructor exit cannot be accepted as ready");
}

void Dispatch(DynarmicHost& host, Dynarmic::A32::Jit& jit, const char* name) {
    const uint32_t address = Address(host, name);
    if (address) host.GetThunkManager().DispatchThunk(host.MemoryRead32(address) & 0xFFFFFF, jit);
}
struct SignalMaskGuard {
    sigset_t old{};
    SignalMaskGuard() { pthread_sigmask(SIG_SETMASK, nullptr, &old); }
    ~SignalMaskGuard() { pthread_sigmask(SIG_SETMASK, &old, nullptr); }
};
void JumpContracts() {
    DynarmicHost host;
    if (!Start(host)) return;
    const uint32_t env = host.GuestAlloc(264, 8);
    // Full ARM path: setjmp -> longjmp -> saved continuation, never the return
    // after longjmp. The canary result is 99 if longjmp merely returns.
    const std::array<uint32_t, 13> words{
        0xE92D4070, 0xE1A04000, 0xE1A05001, 0xE1A06002, 0xE3A01000,
        0xE12FFF36, 0xE3500000, 0x1A000003, 0xE1A00004, 0xE3A01011,
        0xE12FFF35, 0xE3A00063, 0xE8BD8070};
    const uint32_t code = host.GuestAlloc(sizeof(words), 8);
    std::memcpy(host.GetGuestMemoryBase() + code, words.data(), sizeof(words)); host.AddExecutableRange(code, sizeof(words));
    Check(host.RunFunction(code, {env, Address(host, "longjmp"), Address(host, "setjmp")}) == 17 && !host.HasFailed(),
          "ARM setjmp/longjmp restores actual guest continuation");

    Dynarmic::A32::UserConfig config; config.callbacks = &host; config.enable_cycle_counting = false;
    Dynarmic::A32::Jit jit(config);
    const uint32_t stack = host.GuestAlloc(128, 8) + 64;
    SignalMaskGuard mask_guard;
    sigset_t mask = mask_guard.old; sigdelset(&mask, SIGUSR1);
    Check(pthread_sigmask(SIG_SETMASK, &mask, nullptr) == 0, "signal mask test setup");
    for (int save : {0, 1}) {
        pthread_sigmask(SIG_SETMASK, &mask, nullptr);
        for (size_t i = 0; i < 9; ++i) jit.Regs()[4 + i] = 0xABCD0000u + i;
        for (size_t i = 0; i < 16; ++i) jit.ExtRegs()[16 + i] = 0x12340000u + i;
        jit.Regs()[13] = stack; jit.Regs()[14] = 0x10001; jit.SetFpscr(0x01000000);
        jit.Regs()[0] = env; jit.Regs()[1] = save;
        Dispatch(host, jit, "sigsetjmp");
        for (size_t i = 0; i < 9; ++i) jit.Regs()[4 + i] = 0;
        for (size_t i = 0; i < 16; ++i) jit.ExtRegs()[16 + i] = 0;
        jit.Regs()[13] = 0; jit.Regs()[14] = 0; jit.SetFpscr(0);
        sigset_t changed = mask; sigaddset(&changed, SIGUSR1); pthread_sigmask(SIG_SETMASK, &changed, nullptr);
        jit.Regs()[0] = env; jit.Regs()[1] = 0;
        Dispatch(host, jit, "siglongjmp");
        bool registers = jit.Regs()[0] == 1 && jit.Regs()[13] == stack && jit.Regs()[14] == 0x10001 &&
                         jit.Regs()[15] == 0x10000 && (jit.Cpsr() & 0x20) && jit.Fpscr() == 0x01000000;
        for (size_t i = 0; i < 9; ++i) registers &= jit.Regs()[4 + i] == 0xABCD0000u + i;
        for (size_t i = 0; i < 16; ++i) registers &= jit.ExtRegs()[16 + i] == 0x12340000u + i;
        sigset_t observed{}; pthread_sigmask(SIG_SETMASK, nullptr, &observed);
        Check(registers && !host.HasFailed(), "siglongjmp core/VFP/FPSCR/SP/LR/Thumb/value restoration");
        Check(sigismember(&observed, SIGUSR1) == (save ? 0 : 1), "sigsetjmp savemask on/off has actual OS-thread semantics");
    }
}

void ThreadContracts() {
    DynarmicHost host;
    if (!Start(host)) return;
    const uint32_t errno_thunk = Address(host, "__errno");
    uint64_t previous = 0;
    for (unsigned n = 0; n < 8; ++n) {
        uint64_t generation = 0; uint32_t address = 0, before = UINT32_MAX;
        std::thread worker([&] {
            address = host.RunFunction(errno_thunk); before = host.MemoryRead32(address);
            generation = host.CurrentThreadGeneration(); host.MemoryWrite32(address, 1000 + n);
        });
        worker.join();
        Check(address != 0 && before == 0 && generation != previous && !host.HasFailed(),
              "each new OS worker has fresh errno and distinct generation");
        previous = generation;
    }
    const uint32_t env = host.GuestAlloc(264, 8);
    const uint32_t setjmp_thunk = Address(host, "setjmp"), longjmp_thunk = Address(host, "longjmp");
    host.RunFunction(setjmp_thunk, {env});
    std::thread wrong_thread([&] { host.RunFunction(longjmp_thunk, {env, 1}); });
    wrong_thread.join();
    Check(host.HasFailed(), "longjmp rejects another OS thread even when guest thread token defaults to one");
}
uint32_t GuestErrno(DynarmicHost& host) {
    const uint32_t address = Call(host, "__errno", {});
    return address ? host.MemoryRead32(address) : 0;
}
uint32_t ArmCode(DynarmicHost& host, const uint32_t* words, size_t count) {
    const uint32_t code = host.GuestAlloc(count * 4, 8);
    Check(code != 0, "ARM contract code allocation");
    if (code) { std::memcpy(host.GetGuestMemoryBase() + code, words, count * 4); host.AddExecutableRange(code, count * 4); host.InvalidateGuestCode(code, count * 4); }
    return code;
}
uint32_t DestructorCode(DynarmicHost& host) {
    // arg = {counter*, digit}; append digit to a decimal counter.
    const uint32_t words[]{0xE5901000, 0xE5912000, 0xE5903004, 0xE3A0C00A, 0xE0223C92, 0xE5812000, 0xE12FFF1E};
    return ArmCode(host, words, 7);
}
uint32_t DestructorArg(DynarmicHost& host, uint32_t counter, uint32_t digit) {
    const uint32_t argument = host.GuestAlloc(8);
    host.MemoryWrite32(argument, counter); host.MemoryWrite32(argument + 4, digit); return argument;
}
void CxaContracts() {
    DynarmicHost host; if (!Start(host)) return;
    const uint32_t code = DestructorCode(host), counter = host.GuestAlloc(8);
    const uint32_t a = host.GuestAlloc(8), b = host.GuestAlloc(8);
    Check(Call(host, "__cxa_atexit", {code, DestructorArg(host, counter, 1), a}) == 0, "__cxa_atexit registers guest destructor");
    Check(Call(host, "__cxa_atexit", {code, DestructorArg(host, counter, 2), a}) == 0, "second guest destructor registration");
    Check(Call(host, "__aeabi_atexit", {DestructorArg(host, counter, 3), code, b}) == 0, "__aeabi_atexit argument order");
    Call(host, "__cxa_finalize", {a});
    Check(host.MemoryRead32(counter) == 21 && !host.HasFailed(), "finalize synchronously calls actual ARM destructors in reverse order for one DSO");
    Call(host, "__cxa_finalize", {a});
    Check(host.MemoryRead32(counter) == 21, "destructors run at most once");
    Call(host, "__cxa_finalize", {b}); Call(host, "__cxa_finalize", {0});
    Check(host.MemoryRead32(counter) == 213, "DSO filtering and all-DSO finalize preserve remaining destructor");
    Check(Call(host, "__cxa_atexit", {0, 0, a}) == UINT32_MAX && GuestErrno(host) == EINVAL && !host.HasFailed(), "invalid destructor is recoverable EINVAL");
    for (const char* name : {"exit", "_exit"}) {
        DynarmicHost exiting; if (!Start(exiting)) continue;
        const uint32_t tally = exiting.GuestAlloc(8), marker = exiting.GuestAlloc(8);
        exiting.MemoryWrite32(marker, 0xA5A5A5A5);
        Call(exiting, "__cxa_atexit", {DestructorCode(exiting), DestructorArg(exiting, tally, 1), 0});
        const uint32_t wrapper = NonreturnWrapper(exiting, Address(exiting, name), marker);
        exiting.RunFunction(wrapper, {0});
        Check(exiting.HasExited() && !exiting.HasFailed() && exiting.MemoryRead32(marker) == 0xA5A5A5A5,
              "exit/finalizer path is terminal without fake failure or ARM continuation");
        Check(exiting.MemoryRead32(tally) == (!std::strcmp(name, "exit") ? 1u : 0u), "exit runs C++ finalizers; _exit bypasses them");
    }
}

void FileContracts(const char* directory) {
    DynarmicHost host; if (!Start(host)) return;
    std::string pattern = std::string(directory) + "/wolf_libc_contract.XXXXXX";
    std::vector<char> filename(pattern.begin(), pattern.end()); filename.push_back(0);
    const int fd = ::mkstemp(filename.data()); Check(fd >= 0, "create unique test-owned file in supplied directory");
    if (fd < 0) return;
    const uint32_t path = Text(host, filename.data()); struct stat status{};
    Check(Call(host, "chmod", {path, 0640}) == 0 && ::fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0640, "chmod changes actual test-file permissions");
    Check(Call(host, "fcntl", {static_cast<uint32_t>(fd), F_SETFD, FD_CLOEXEC}) == 0 && (::fcntl(fd, F_GETFD) & FD_CLOEXEC), "fcntl changes actual FD flags");
    Check(Call(host, "fcntl", {static_cast<uint32_t>(fd), F_GETFD}) == static_cast<uint32_t>(::fcntl(fd, F_GETFD)), "fcntl GETFD has no variadic pointer translation");
    const uint32_t duplicate = Call(host, "fcntl", {static_cast<uint32_t>(fd), F_DUPFD_CLOEXEC, 0});
    Check(duplicate != UINT32_MAX && (::fcntl(static_cast<int>(duplicate), F_GETFD) & FD_CLOEXEC), "fcntl duplicates the actual FD with CLOEXEC");
    if (duplicate != UINT32_MAX) ::close(static_cast<int>(duplicate));
    const uint32_t lock = host.GuestAlloc(32, 8);
    std::memset(host.GetGuestMemoryBase() + lock, 0, 32);
    const int16_t type = F_WRLCK, whence = SEEK_SET;
    std::memcpy(host.GetGuestMemoryBase() + lock, &type, 2); std::memcpy(host.GetGuestMemoryBase() + lock + 2, &whence, 2);
    Check(Call(host, "fcntl", {static_cast<uint32_t>(fd), 5, lock}) == 0 && host.MemoryRead16(lock) == F_UNLCK, "ARM32 16-byte F_GETLK translation");
    std::memcpy(host.GetGuestMemoryBase() + lock, &type, 2);
    Check(Call(host, "fcntl", {static_cast<uint32_t>(fd), 12, lock}) == 0 && host.MemoryRead16(lock) == F_UNLCK, "ARM32 aligned 32-byte F_GETLK64 translation");
    Check(Call(host, "fcntl", {static_cast<uint32_t>(fd), 0x7FFFFFFF, 0}) == UINT32_MAX && GuestErrno(host) == ENOSYS, "unsupported fcntl command has explicit recoverable error");
    const uint32_t file = Call(host, "fopen", {path, Text(host, "r+")}), buffer = host.GuestAlloc(128);
    Check(file != 0, "registered guest FILE opens actual test-owned file");
    if (file) {
        Check(Call(host, "setvbuf", {file, buffer, _IOFBF, 128}) == 0, "setvbuf uses validated guest buffer on registered host FILE");
        const uint32_t content = Text(host, "buffered-marker");
        Check(Call(host, "fwrite", {content, 1, 15, file}) == 15 && Call(host, "fflush", {file}) == 0, "buffered guest writes reach real stream");
        char observed[16]{};
        Check(::pread(fd, observed, 15, 0) == 15 && !std::memcmp(observed, "buffered-marker", 15), "real FD sees exact flushed guest bytes");
        Check(Call(host, "setvbuf", {file, 0, 99, 0}) == UINT32_MAX && GuestErrno(host) == EINVAL, "setvbuf rejects invalid mode");
        Check(Call(host, "fclose", {file}) == 0, "guest FILE closes before guest buffer lifetime ends");
    }
    const int native_fd = ::dup(fd);
    Check(native_fd >= 0, "duplicate native FD for fdopen ownership contract");
    if (native_fd >= 0) {
        const uint32_t native_file = Call(host, "fdopen", {static_cast<uint32_t>(native_fd), Text(host, "r+")});
        Check(native_file != 0, "fdopen registers native FILE");
        if (native_file) {
            Check(Call(host, "fileno", {native_file}) == static_cast<uint32_t>(native_fd) &&
                  Call(host, "fread", {buffer, 1, 1, native_file}) == 1 && host.MemoryRead8(buffer) == 'b',
                  "fdopen preserves native FD and actual file contents");
            Check(Call(host, "fclose", {native_file}) == 0 && ::fcntl(native_fd, F_GETFD) == -1 && errno == EBADF,
                  "fdopen transfers native FD ownership to fclose");
        } else {
            ::close(native_fd);
        }
    }
    const uint32_t read_mode = Text(host, "rb"), fdopen_error = Call(host, "__errno", {});
    Check(fdopen_error != 0, "guest errno storage for native fdopen failure");
    if (fdopen_error) host.MemoryWrite32(fdopen_error, E2BIG);
    Check(Call(host, "fdopen", {UINT32_MAX, read_mode}) == 0 && GuestErrno(host) == EBADF,
          "native fdopen failure returns NULL and replaces stale guest errno with EBADF");
    const uint32_t full = Call(host, "fopen", {Text(host, "/dev/full"), Text(host, "wb")});
    Check(full != 0, "open failing-flush device through guest FILE path");
    if (full) {
        const uint32_t full_buffer = host.GuestAlloc(4096);
        Check(full_buffer != 0, "failing-flush guest buffer allocation");
        const bool buffered = full_buffer && Call(host, "setvbuf", {full, full_buffer, _IOFBF, 4096}) == 0;
        Check(buffered, "failing-flush FILE uses full buffering");
        if (buffered) {
            Check(Call(host, "fputc", {'!', full}) == '!', "buffered write succeeds before final flush");
            const uint32_t error_at = Call(host, "__errno", {});
            Check(error_at != 0, "guest errno address for final-flush failure");
            if (error_at) host.MemoryWrite32(error_at, 0);
            Check(Call(host, "fclose", {full}) == static_cast<uint32_t>(EOF) && GuestErrno(host) == ENOSPC,
                  "fclose propagates final-flush EOF and ENOSPC");
            Check(Call(host, "fclose", {full}) == static_cast<uint32_t>(EOF) && GuestErrno(host) == EBADF,
                  "failed close still consumes FILE handle exactly once");
        } else {
            Call(host, "fclose", {full});
        }
        host.GuestFree(full_buffer);
    }
    Check(Call(host, "setvbuf", {0xDEADBEEF, 0, _IONBF, 0}) == UINT32_MAX && GuestErrno(host) == ENOSYS, "unregistered setvbuf is explicit ENOSYS");
    Check(Call(host, "unlink", {path}) == 0 && ::access(filename.data(), F_OK) == -1 && errno == ENOENT, "unlink removes actual owned file");
    Check(Call(host, "unlink", {path}) == UINT32_MAX && GuestErrno(host) == ENOENT, "unlink missing file propagates real errno");
    ::close(fd); ::unlink(filename.data());
    Check(!host.HasFailed(), "recoverable file errors do not fail guest host");
}

void SemaphoreContracts() {
    DynarmicHost host; if (!Start(host)) return;
    const uint32_t semaphore = host.GuestAlloc(8), value = host.GuestAlloc(4);
    Check(Call(host, "sem_close", {0}) == UINT32_MAX && GuestErrno(host) == EINVAL,
          "unnamed semaphore close rejects null pointer");
    Check(Call(host, "sem_close", {semaphore + 1}) == UINT32_MAX && GuestErrno(host) == EINVAL,
          "unnamed semaphore close rejects unaligned pointer");
    Check(Call(host, "sem_close", {semaphore}) == UINT32_MAX && GuestErrno(host) == EINVAL,
          "unnamed semaphore close rejects uninitialized storage");
    Check(Call(host, "sem_init", {semaphore, 0, 2}) == 0 &&
          Call(host, "sem_trywait", {semaphore}) == 0 &&
          Call(host, "sem_getvalue", {semaphore, value}) == 0 && host.MemoryRead32(value) == 1,
          "unnamed semaphore lifecycle has actual count");
    Check(Call(host, "sem_close", {semaphore}) == 0, "Engine sem_close retires idle unnamed semaphore");
    Check(Call(host, "sem_close", {semaphore}) == UINT32_MAX && GuestErrno(host) == EINVAL,
          "unnamed semaphore close consumes registry entry once");
    Check(Call(host, "sem_init", {semaphore, 0, 0}) == 0 &&
          Call(host, "sem_trywait", {semaphore}) == UINT32_MAX && GuestErrno(host) == EAGAIN &&
          Call(host, "sem_destroy", {semaphore}) == 0, "reinitialized semaphore has new zero count");
    for (const char* close : {"sem_close", "sem_destroy"}) {
        Check(Call(host, "sem_init", {semaphore, 0, 0}) == 0, "initialize zero-count waiter semaphore");
        struct timespec deadline{};
        Check(::clock_gettime(CLOCK_REALTIME, &deadline) == 0, "bounded semaphore waiter clock");
        deadline.tv_sec += 2;
        const uint32_t timeout = host.GuestAlloc(8);
        host.MemoryWrite32(timeout, static_cast<uint32_t>(deadline.tv_sec));
        host.MemoryWrite32(timeout + 4, static_cast<uint32_t>(deadline.tv_nsec));
        std::atomic<bool> started{false};
        uint32_t waited = UINT32_MAX;
        std::thread waiter([&] {
            host.PrepareThreadContext();
            started.store(true, std::memory_order_release);
            waited = Call(host, "sem_timedwait", {semaphore, timeout});
        });
        const auto admission = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < admission)
            std::this_thread::yield();
        Check(started.load(std::memory_order_acquire), "semaphore waiter thread started");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(Call(host, close, {semaphore}) == UINT32_MAX && GuestErrno(host) == EBUSY,
              "active semaphore operation prevents registry retirement");
        Check(Call(host, "sem_post", {semaphore}) == 0, "post still reaches retained waiter object");
        waiter.join();
        Check(waited == 0 && Call(host, close, {semaphore}) == 0,
              "original waiter completes and idle semaphore can be retired");
        host.GuestFree(timeout);
    }
    Check(!host.HasFailed(), "unnamed semaphore lifecycle and recoverable busy errors keep host live");
}

void CpuInfoContracts() {
    DynarmicHost host; if (!Start(host)) return;
    const uint32_t path = Text(host, "/proc/cpuinfo"), buffer = host.GuestAlloc(4096);
    const uint32_t fd = Call(host, "open", {path, O_RDONLY | O_CLOEXEC, 0});
    Check(fd != UINT32_MAX, "guest CPU info opens without a filesystem output");
    if (fd == UINT32_MAX) return;
    const uint32_t status = host.GuestAlloc(128);
    Check(Call(host, "fstat", {fd, status}) == 0 && host.MemoryRead64(status + 48) > 0 &&
        host.MemoryRead64(status + 48) < 4096 &&
        (Call(host, "fcntl", {fd, F_GETFL}) & O_ACCMODE) == O_RDONLY &&
        (Call(host, "fcntl", {fd, F_GETFD}) & FD_CLOEXEC), "CPU info is bounded, read-only and CLOEXEC through guest ABI");
    const uint32_t size = Call(host, "read", {fd, buffer, 4095});
    Check(size > 0 && size < 4096, "CPU info read fits the real FMOD parser buffer");
    if (size > 0 && size < 4096) {
        host.MemoryWrite8(buffer + size, 0);
        const char* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + buffer);
        Check(std::strstr(text, "CPU architecture\t: 7") && std::strstr(text, "vfp vfpv3 neon") &&
            !std::strstr(text, "asimd"), "CPU info describes emulated ARMv7 VFP/NEON");
    }
    Check(Call(host, "read", {fd, buffer, 1}) == 0, "CPU info EOF");
    Check(Call(host, "lseek", {fd, 0, SEEK_SET}) == 0 && Call(host, "read", {fd, buffer, 4}) == 4,
        "CPU info supports descriptor seek and partial read");
    const uint32_t duplicate = Call(host, "fcntl", {fd, F_DUPFD_CLOEXEC, 0});
    Check(duplicate != UINT32_MAX && (Call(host, "fcntl", {duplicate, F_GETFD}) & FD_CLOEXEC) &&
        Call(host, "read", {duplicate, buffer, 4}) == 4 && Call(host, "lseek", {fd, 0, SEEK_CUR}) == 8,
        "CPU info fcntl duplicate shares virtual position and has CLOEXEC");
    if (duplicate != UINT32_MAX) Check(Call(host, "close", {duplicate}) == 0, "CPU info duplicate closes independently");
    Check(Call(host, "write", {fd, buffer, 4}) == UINT32_MAX && GuestErrno(host) == EBADF,
        "CPU info descriptor cannot be written");
    Check(Call(host, "fdopen", {fd, Text(host, "rb")}) == 0 && GuestErrno(host) == ENOSYS &&
        Call(host, "read", {fd, buffer, 4}) == 4, "unsupported region fdopen leaves descriptor owned and readable");
    Check(Call(host, "close", {fd}) == 0, "CPU info descriptor closes normally");
    const uint32_t nofollow = Call(host, "open", {path, O_RDONLY | O_NOFOLLOW, 0});
    Check(nofollow != UINT32_MAX && Call(host, "read", {nofollow, buffer, 4}) == 4,
        "logical CPU info is not the internal memory-FD symlink");
    if (nofollow != UINT32_MAX) Check(Call(host, "close", {nofollow}) == 0, "CPU info NOFOLLOW descriptor closes");
    Check(Call(host, "open", {path, O_RDWR, 0}) == UINT32_MAX && GuestErrno(host) == EACCES,
        "CPU info rejects write open");
    Check(Call(host, "open", {path, O_RDONLY | O_TRUNC, 0}) == UINT32_MAX && GuestErrno(host) == EACCES,
        "CPU info rejects truncation");
    Check(Call(host, "getauxval", {16}) == 0x3040, "ARM32 HWCAP agrees with CPU info VFP/NEON/VFPv3");
    const uint32_t mode = Text(host, "rb");
    const uint32_t file = Call(host, "fopen", {path, mode});
    Check(file != 0, "CPU info stdio uses the same guest model");
    if (file) {
        Check(Call(host, "fread", {buffer, 1, 4096, file}) == size, "CPU info stdio and descriptor lengths agree");
        Check(Call(host, "freopen", {path, mode, file}) == file &&
            Call(host, "fread", {buffer, 1, 4, file}) == 4, "CPU info freopen resets the guest stream");
        Check(Call(host, "freopen", {path, Text(host, "r+"), file}) == 0 && GuestErrno(host) == EACCES,
            "CPU info writable freopen fails and closes original stream");
    }
    Check(Call(host, "fopen", {path, Text(host, "w")}) == 0 && GuestErrno(host) == EACCES,
        "CPU info writable stdio fails");
    Check(!host.HasFailed(), "CPU info errors remain recoverable libc errors");
}

void AttributeContracts() {
    DynarmicHost host; if (!Start(host)) return;
    const uint32_t attr = host.GuestAlloc(24), ma = host.GuestAlloc(4), mutex = host.GuestAlloc(4);
    Check(Call(host, "pthread_attr_setdetachstate", {attr, 1}) == EINVAL, "thread attribute must be initialized");
    Check(Call(host, "pthread_attr_init", {attr}) == 0 && Call(host, "pthread_attr_setdetachstate", {attr, 99}) == EINVAL, "thread attribute initialization and invalid detach state");
    Check(Call(host, "pthread_attr_setstacksize", {attr, 1}) == EINVAL && Call(host, "pthread_attr_setstacksize", {attr, 3 * 1024 * 1024}) == ENOSYS, "thread stack invalid/unsupported sizes are explicit");
    Check(Call(host, "pthread_attr_setstacksize", {attr, 1024 * 1024}) == 0 && Call(host, "pthread_attr_setdetachstate", {attr, PTHREAD_CREATE_JOINABLE}) == 0, "supported stack and joinable attributes");
    const uint32_t words[]{0xE2800001, 0xE12FFF1E}; const uint32_t code = ArmCode(host, words, 2);
    const uint32_t thread = host.GuestAlloc(4), result = host.GuestAlloc(4);
    Check(Call(host, "pthread_create", {thread, attr, code, 41}) == 0, "attribute-backed native thread starts actual ARM function");
    const uint32_t token = host.MemoryRead32(thread);
    Check(Call(host, "pthread_join", {token, result}) == 0 && host.MemoryRead32(result) == 42, "join observes actual guest thread return value");
    std::atomic<bool> entered{false}, released{false};
    const uint32_t blocked = host.GetThunkManager().RegisterThunk("__wolf_test_detached_worker", [&](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
        jit.Regs()[0] = 42;
    });
    Check(Call(host, "pthread_attr_setdetachstate", {attr, PTHREAD_CREATE_DETACHED}) == 0 && Call(host, "pthread_create", {thread, attr, blocked, 0}) == 0,
          "detached attribute creates actual detached native guest worker");
    const uint32_t detached = host.MemoryRead32(thread);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(entered.load() && Call(host, "pthread_join", {detached, 0}) == EINVAL, "live detached worker cannot be joined");
    released.store(true, std::memory_order_release);
    uint32_t join_result = EINVAL;
    while (join_result == EINVAL && std::chrono::steady_clock::now() < deadline) join_result = Call(host, "pthread_join", {detached, 0});
    Check(join_result == ESRCH, "finished detached worker releases its registry entry");
    if (join_result != ESRCH) { std::fflush(nullptr); std::exit(2); }
    Check(Call(host, "pthread_attr_destroy", {attr}) == 0 && Call(host, "pthread_attr_destroy", {attr}) == EINVAL, "attribute destroy removes owned state");
    for (uint32_t type : {uint32_t(PTHREAD_MUTEX_NORMAL), uint32_t(PTHREAD_MUTEX_RECURSIVE), uint32_t(PTHREAD_MUTEX_ERRORCHECK)}) {
        Check(Call(host, "pthread_mutexattr_init", {ma}) == 0 && Call(host, "pthread_mutexattr_settype", {ma, type}) == 0 && Call(host, "pthread_mutex_init", {mutex, ma}) == 0, "mutex attribute is applied to native mutex");
        Check(Call(host, "pthread_mutex_lock", {mutex}) == 0, "first native mutex lock");
        const uint32_t attempt = Call(host, "pthread_mutex_trylock", {mutex});
        Check(attempt == (type == PTHREAD_MUTEX_RECURSIVE ? 0u : uint32_t(EBUSY)), "normal/errorcheck mutex cannot silently recurse");
        if (type == PTHREAD_MUTEX_RECURSIVE) Check(Call(host, "pthread_mutex_unlock", {mutex}) == 0, "recursive mutex second depth unlock");
        if (type == PTHREAD_MUTEX_ERRORCHECK) Check(Call(host, "pthread_mutex_lock", {mutex}) == EDEADLK, "errorcheck lock detects self deadlock");
        Check(Call(host, "pthread_mutex_unlock", {mutex}) == 0 && Call(host, "pthread_mutex_destroy", {mutex}) == 0, "mutex unlock/destroy");
        Check(Call(host, "pthread_mutexattr_destroy", {ma}) == 0, "mutex attribute destroy");
    }
    Check(Call(host, "pthread_mutexattr_init", {ma}) == 0 && Call(host, "pthread_mutexattr_settype", {ma, 99}) == EINVAL, "invalid mutex type is rejected");
    Call(host, "pthread_mutexattr_destroy", {ma});
    struct sched_param native{}; int policy = 0; pthread_getschedparam(pthread_self(), &policy, &native);
    const uint32_t param = host.GuestAlloc(4); host.MemoryWrite32(param, native.sched_priority);
    const uint32_t self = Call(host, "pthread_self", {});
    Check(Call(host, "pthread_setschedparam", {self, static_cast<uint32_t>(policy), param}) == 0, "current thread scheduling uses actual native API");
    Check(Call(host, "pthread_setschedparam", {UINT32_MAX, 0, param}) == ESRCH && !host.HasFailed(), "unknown guest thread returns ESRCH");
}

void SignalContracts() {
    SignalMaskGuard mask;
    struct sigaction original{}, initial{}; ::sigaction(SIGUSR1, nullptr, &original);
    sigemptyset(&initial.sa_mask); initial.sa_handler = SIG_DFL; ::sigaction(SIGUSR1, &initial, nullptr);
    DynarmicHost host; if (!Start(host)) { ::sigaction(SIGUSR1, &original, nullptr); return; }
    const uint32_t action = host.GuestAlloc(16), previous = host.GuestAlloc(16), set = host.GuestAlloc(4), old = host.GuestAlloc(4);
    host.MemoryWrite32(action, 1); host.MemoryWrite32(action + 4, uint32_t(1) << (SIGUSR2 - 1)); host.MemoryWrite32(action + 8, SA_RESTART);
    Check(Call(host, "sigaction", {SIGUSR1, action, previous}) == 0 && host.MemoryRead32(previous) == 0, "ARM32 16-byte sigaction installs actual SIG_IGN and returns DFL");
    struct sigaction observed{}; ::sigaction(SIGUSR1, nullptr, &observed);
    Check(observed.sa_handler == SIG_IGN && sigismember(&observed.sa_mask, SIGUSR2) == 1, "guest mask and handler are converted without guest pointers in host callback slots");
    Check(Call(host, "raise", {SIGUSR1}) == 0, "raise ignored signal uses actual native signal operation");
    struct sigaction fpe_original{};
    Check(::sigaction(SIGFPE, nullptr, &fpe_original) == 0, "save native SIGFPE disposition");
    Check(Call(host, "sigaction", {SIGFPE, action, 0}) == 0 && Call(host, "raise", {SIGFPE}) == 0 && !host.HasFailed(),
          "explicitly ignored SIGFPE returns without failing guest");
    Check(::sigaction(SIGFPE, &fpe_original, nullptr) == 0, "restore native SIGFPE disposition");
    host.MemoryWrite32(action, 0x12340000);
    Check(Call(host, "sigaction", {SIGUSR1, action, previous}) == UINT32_MAX && GuestErrno(host) == ENOSYS, "guest signal callback is explicit ENOSYS");
    Check(Call(host, "bsd_signal", {SIGUSR1, 0}) == 1, "bsd_signal DFL returns previous IGN");
    Check(Call(host, "bsd_signal", {SIGUSR1, 0x12340000}) == UINT32_MAX && GuestErrno(host) == ENOSYS, "bsd_signal does not forward guest callback to host");
    host.MemoryWrite32(set, uint32_t(1) << (SIGUSR2 - 1));
    Check(Call(host, "pthread_sigmask", {SIG_BLOCK, set, old}) == 0, "ARM32 four-byte pthread mask blocks actual signal");
    sigset_t actual{}; pthread_sigmask(SIG_SETMASK, nullptr, &actual);
    Check(sigismember(&actual, SIGUSR2) == 1, "actual OS-thread mask changed");
    Check(Call(host, "pthread_sigmask", {SIG_SETMASK, old, 0}) == 0 && Call(host, "pthread_sigmask", {99, set, 0}) == EINVAL, "mask restoration and pthread error-number contract");
    struct sigaction alarm_original{}, ignored{}; ::sigaction(SIGALRM, nullptr, &alarm_original); sigemptyset(&ignored.sa_mask); ignored.sa_handler = SIG_IGN; ::sigaction(SIGALRM, &ignored, nullptr);
    Call(host, "alarm", {30}); const uint32_t remaining = Call(host, "alarm", {0});
    Check(remaining > 0 && remaining <= 30, "alarm schedules and cancellation returns actual remaining time");
    ::alarm(0); ::sigaction(SIGALRM, &alarm_original, nullptr); ::sigaction(SIGUSR1, &original, nullptr);
    Check(!host.HasFailed(), "supported signal operations and recoverable errors preserve host");
}

void DlAndNetworkContracts() {
    DynarmicHost host; if (!Start(host)) return;
    const uint32_t handle = Call(host, "dlopen", {Text(host, "libc.so"), 2});
    Check(handle != 0 && Call(host, "dlsym", {handle, Text(host, "strlen")}) == Address(host, "strlen"), "dlopen resolves typed preloaded bridge provider");
    Check(Call(host, "dlsym", {handle, Text(host, "__wolf_absent_contract_symbol")}) == 0, "dlsym missing symbol returns null");
    const uint32_t error = Call(host, "dlerror", {});
    Check(error != 0 && std::strstr(reinterpret_cast<char*>(host.GetGuestMemoryBase() + error), "absent") && Call(host, "dlerror", {}) == 0, "dlerror is a stable one-shot guest string");
    Check(Call(host, "dlclose", {handle}) == 0 && Call(host, "dlclose", {handle}) == UINT32_MAX && Call(host, "dlerror", {}) != 0, "dlclose consumes actual additional reference and detects double close");
    Check(Call(host, "dlsym", {handle, Text(host, "strlen")}) == 0, "closed handle cannot resolve symbols");
    Check(Call(host, "dlopen", {Text(host, "libwolf_missing_contract.so"), 2}) == 0 && Call(host, "dlerror", {}) != 0, "absent library is not fabricated as open");
    Check(Call(host, "inet_addr", {Text(host, "127.0.0.1")}) == ::inet_addr("127.0.0.1") && Call(host, "inet_addr", {Text(host, "invalid-address")}) == INADDR_NONE, "inet_addr returns actual network-order bits / INADDR_NONE");
    const uint32_t marker = host.GuestAlloc(16); host.MemoryWrite32(marker, 0xA5A5A5A5);
    for (const char* name : {"socket", "bind", "listen", "accept", "connect", "send", "recv", "select", "setsockopt"})
        Check(Call(host, name, {0, marker, 8, 0}) == UINT32_MAX && GuestErrno(host) == ENOSYS && host.MemoryRead32(marker) == 0xA5A5A5A5,
              "offline socket operation is recoverable ENOSYS without caller-buffer mutation");
    Check(!host.HasFailed(), "network failures do not fabricate successful communication or abort guest");
}

void RaiseFatalContracts() {
    for (const int signal : {SIGABRT, SIGFPE}) {
        struct sigaction original{}, initial{}; sigemptyset(&initial.sa_mask); initial.sa_handler = SIG_DFL;
        const bool installed = ::sigaction(signal, &initial, &original) == 0;
        Check(installed, "select default fatal signal disposition");
        if (!installed) continue;
        {
            DynarmicHost host;
            if (Start(host)) {
                const uint32_t marker = host.GuestAlloc(8); host.MemoryWrite32(marker, 0xA5A5A5A5);
                const uint32_t raised = NonreturnWrapper(host, Address(host, "raise"), marker);
                const uint32_t continued = NonreturnWrapper(host, Address(host, "getpid"), marker);
                host.RunFunction(raised, {static_cast<uint32_t>(signal)});
                Check(host.HasFailed() && !host.HasExited() && host.MemoryRead32(marker) == 0xA5A5A5A5,
                      signal == SIGFPE ? "default SIGFPE cannot return to ARM" : "SIGABRT cannot return to ARM");
                host.RunFunction(continued, {1});
                Check(host.HasFailed() && host.MemoryRead32(marker) == 0xA5A5A5A5,
                      signal == SIGFPE ? "SIGFPE failure prevents subsequent ARM execution" : "SIGABRT failure prevents subsequent ARM execution");
            }
        }
        Check(::sigaction(signal, &original, nullptr) == 0, "restore fatal signal disposition");
    }
}

void Zip16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(value); bytes.push_back(value >> 8);
}
void Zip32(std::vector<uint8_t>& bytes, uint32_t value) {
    Zip16(bytes, value); Zip16(bytes, value >> 16);
}
std::vector<uint8_t> TinyZip(const std::string& name, const std::string& text, bool compressed, bool outside_bounds) {
    std::vector<uint8_t> payload(text.begin(), text.end());
    if (compressed) {
        z_stream stream{}; std::array<uint8_t, 256> output{};
        const int initialized = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        Check(initialized == Z_OK, "tiny test ZIP raw deflate initialization");
        if (initialized != Z_OK) return {};
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data())); stream.avail_in = text.size();
        stream.next_out = output.data(); stream.avail_out = output.size();
        const int result = deflate(&stream, Z_FINISH);
        Check(result == Z_STREAM_END, "tiny test ZIP raw deflate completes");
        payload.assign(output.begin(), output.begin() + stream.total_out); deflateEnd(&stream);
        if (result != Z_STREAM_END) return {};
    }
    const uint16_t method = compressed ? 8 : 0;
    const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(text.data()), text.size());
    const uint32_t size = text.size() + (outside_bounds ? 65536u : 0u);
    const uint32_t stored = payload.size() + (outside_bounds ? 65536u : 0u);
    std::vector<uint8_t> bytes;
    Zip32(bytes, 0x04034b50); Zip16(bytes, 20); Zip16(bytes, 0); Zip16(bytes, method); Zip16(bytes, 0); Zip16(bytes, 0);
    Zip32(bytes, crc); Zip32(bytes, stored); Zip32(bytes, size); Zip16(bytes, name.size()); Zip16(bytes, 0);
    bytes.insert(bytes.end(), name.begin(), name.end()); bytes.insert(bytes.end(), payload.begin(), payload.end());
    const uint32_t central = bytes.size();
    Zip32(bytes, 0x02014b50); Zip16(bytes, 20); Zip16(bytes, 20); Zip16(bytes, 0); Zip16(bytes, method); Zip16(bytes, 0); Zip16(bytes, 0);
    Zip32(bytes, crc); Zip32(bytes, stored); Zip32(bytes, size); Zip16(bytes, name.size()); Zip16(bytes, 0); Zip16(bytes, 0);
    Zip16(bytes, 0); Zip16(bytes, 0); Zip32(bytes, 0); Zip32(bytes, 0); bytes.insert(bytes.end(), name.begin(), name.end());
    const uint32_t central_size = bytes.size() - central;
    Zip32(bytes, 0x06054b50); Zip16(bytes, 0); Zip16(bytes, 0); Zip16(bytes, 1); Zip16(bytes, 1);
    Zip32(bytes, central_size); Zip32(bytes, central); Zip16(bytes, 0);
    return bytes;
}
std::string WriteTinyZip(const char* directory, const std::vector<uint8_t>& bytes) {
    std::string pattern = std::string(directory) + "/wolf_apk_index.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    const int fd = ::mkstemp(name.data()); Check(fd >= 0, "create unique tiny APK index fixture");
    if (fd < 0) return {};
    const ssize_t written = ::write(fd, bytes.data(), bytes.size()); ::close(fd);
    Check(written == static_cast<ssize_t>(bytes.size()), "write complete tiny APK index fixture");
    if (written != static_cast<ssize_t>(bytes.size())) { ::unlink(name.data()); return {}; }
    return name.data();
}
void ApkIndexContracts(const char* directory) {
    DynarmicHost host; if (!Start(host)) return;
    const char* names[]{"assets/cache_episode_01.obb", "assets/rus_ai_voice_episode_03.obb", "assets/settings.lenc", "assets/scene.ttarch"};
    const char* contents[]{"root-episode-cache", "root-voice-cache", "root-lenc", "root-ttarch"};
    std::vector<std::string> paths;
    for (size_t i = 0; i < 4; ++i) paths.push_back(WriteTinyZip(directory, TinyZip(names[i], contents[i], false, false)));
    if (std::any_of(paths.begin(), paths.end(), [](const auto& path) { return path.empty(); })) {
        for (const auto& path : paths) if (!path.empty()) ::unlink(path.c_str()); return;
    }
    const char* raw_paths[]{paths[0].c_str(), paths[1].c_str(), paths[2].c_str(), paths[3].c_str()};
    Check(IndexApks(raw_paths, 4) == 0, "stored root episode/voice/lenc/ttarch assets initialize direct APK index");
    for (size_t i = 0; i < 4; ++i) {
        const uint32_t file = Call(host, "fopen", {Text(host, names[i]), Text(host, "rb")});
        Check(file != 0, "stored root asset opens direct APK stream");
        if (!file) continue;
        const size_t size = std::strlen(contents[i]); const uint32_t buffer = host.GuestAlloc(size + 8);
        std::memset(host.GetGuestMemoryBase() + buffer, 0xA5, size + 8);
        Check(Call(host, "fread", {buffer, 1, static_cast<uint32_t>(size + 8), file}) == size &&
              !std::memcmp(host.GetGuestMemoryBase() + buffer, contents[i], size) && host.MemoryRead8(buffer + size) == 0xA5,
              "direct APK stream reads exact payload and does not cross entry bounds");
        Check(Call(host, "fclose", {file}) == 0, "close direct APK fixture stream");
    }
    const std::string compressed = WriteTinyZip(directory, TinyZip("assets/cache_episode_99.obb", "compressed-root-cache", true, false));
    const std::string invalid = WriteTinyZip(directory, TinyZip("assets/cache_episode_01.obb", "truncated-advertised-cache", false, true));
    if (!compressed.empty()) { const char* inputs[]{compressed.c_str()}; Check(IndexApks(inputs, 1) == -1, "compressed-only assets remain skipped, never indexed as raw bytes"); }
    if (!invalid.empty()) { const char* inputs[]{invalid.c_str()}; Check(IndexApks(inputs, 1) == -1, "stored data size crossing central-directory boundary is rejected"); }
    const char* reset[]{"/wolf_contract_no_such_apk"}; Check(IndexApks(reset, 1) == -1, "clear fixture index after rejected input");
    for (const auto& path : paths) ::unlink(path.c_str());
    if (!compressed.empty()) ::unlink(compressed.c_str()); if (!invalid.empty()) ::unlink(invalid.c_str());
    Check(!host.HasFailed(), "index errors stay bounded return values without guest execution failure");
}

void LegacyDlContracts() {
    DynarmicHost host; if (!Start(host)) return;
    // These are guest Bionic LP32 constants, never native LP64 macros.
    constexpr uint32_t now = 0, lazy = 1, global = 2, no_load = 4, nodelete = 0x1000;
    constexpr uint32_t default_handle = UINT32_MAX, next_handle = UINT32_MAX - 1;
    const uint32_t egl_name = Text(host, "libEGL.so"), gl_name = Text(host, "libGLESv2.so");
    const uint32_t egl_symbol = Text(host, "eglGetDisplay"), gl_symbol = Text(host, "glClear");
    const uint32_t config_symbol = Text(host, "eglChooseConfig");
    const uint32_t config_provider = host.GetThunkManager().FindProvider("eglChooseConfig", ThunkManager::ProviderKind::Function, "", "libEGL.so");
    const uint32_t egl_provider = host.GetThunkManager().FindProvider("eglGetDisplay", ThunkManager::ProviderKind::Function, "", "libEGL.so");
    const uint32_t gl_provider = host.GetThunkManager().FindProvider("glClear", ThunkManager::ProviderKind::Function, "", "libGLESv2.so");
    Check(egl_provider != 0 && gl_provider != 0 && config_provider != 0 &&
          host.GetThunkManager().FindProvider("eglGetDisplay", ThunkManager::ProviderKind::Object, "", "libEGL.so") == 0 &&
          host.GetThunkManager().FindProvider("glClear", ThunkManager::ProviderKind::Object, "", "libGLESv2.so") == 0,
          "EGL/GLES expected providers exist with function metadata, not fabricated addresses or object exports");
    Check(Call(host, "dlsym", {0, config_symbol}) == 0 && Call(host, "dlerror", {}) != 0,
          "SDL initial NULL-handle eglChooseConfig probe fails as expected for Bionic LP32");
    const uint32_t egl = Call(host, "dlopen", {egl_name, now}), gl = Call(host, "dlopen", {gl_name, now});
    Check(egl != 0 && gl != 0, "SDL-style guest dlopen flags zero mean LP32 RTLD_NOW");
    Check(Call(host, "dlsym", {egl, egl_symbol}) == egl_provider && Call(host, "dlsym", {gl, gl_symbol}) == gl_provider,
          "actual ARM JIT dl contracts resolve scoped EGL/GLES function providers");
    Check(Call(host, "dlsym", {egl, config_symbol}) == config_provider,
          "SDL scoped EGL fallback resolves eglChooseConfig after the expected NULL probe failure");
    Check(Call(host, "dlsym", {gl, egl_symbol}) == 0, "GLES handle cannot resolve unrelated EGL provider");
    const uint32_t scope_error = Call(host, "dlerror", {});
    Check(scope_error != 0 && Call(host, "dlerror", {}) == 0, "scoped failure yields one-shot guest dlerror");
    for (uint32_t flags : {lazy, global, no_load, nodelete, lazy | global | no_load | nodelete}) {
        const uint32_t reference = Call(host, "dlopen", {gl_name, flags});
        Check(reference != 0 && Call(host, "dlsym", {reference, gl_symbol}) == gl_provider,
              "supported LP32 LAZY/GLOBAL/NOLOAD/NODELETE flags preserve already resolved group");
        Check(Call(host, "dlclose", {reference}) == 0, "close supported flag reference");
    }
    const uint32_t second_gl = Call(host, "dlopen", {gl_name, now});
    Check(second_gl != 0 && second_gl != gl && Call(host, "dlclose", {gl}) == 0,
          "dlopen references are independent and closing one consumes exactly that reference");
    Check(Call(host, "dlsym", {gl, gl_symbol}) == 0 && Call(host, "dlerror", {}) != 0 &&
          Call(host, "dlsym", {second_gl, gl_symbol}) == gl_provider,
          "closed handle is invalid while another live reference retains symbols");
    Check(Call(host, "dlclose", {second_gl}) == 0 && Call(host, "dlclose", {egl}) == 0, "close remaining SDL-style references");
    Check(Call(host, "dlsym", {default_handle, gl_symbol}) == gl_provider &&
          Call(host, "dlsym", {default_handle, egl_symbol}) == egl_provider,
          "guest LP32 RTLD_DEFAULT 0xffffffff searches the permanent global provider group");
    const uint32_t main_group = Call(host, "dlopen", {0, now});
    Check(main_group != 0 && Call(host, "dlsym", {main_group, gl_symbol}) == gl_provider && Call(host, "dlclose", {main_group}) == 0,
          "dlopen NULL/zero returns a real additional preloaded group handle");
    Check(Call(host, "dlsym", {next_handle, gl_symbol}) == 0, "guest LP32 RTLD_NEXT 0xfffffffe is explicitly unsupported");
    const uint32_t next_error = Call(host, "dlerror", {});
    Check(next_error != 0 && std::strstr(reinterpret_cast<const char*>(host.GetGuestMemoryBase() + next_error), "RTLD_NEXT") && Call(host, "dlerror", {}) == 0,
          "RTLD_NEXT reports precise one-shot error instead of masquerading as DEFAULT");
    Check(Call(host, "dlsym", {0, gl_symbol}) == 0 && Call(host, "dlerror", {}) != 0,
          "raw zero is not LP32 RTLD_DEFAULT and is not a valid dlopen reference");
    for (uint32_t flags : {8u, 0x100u, 0x80000000u, UINT32_MAX})
        Check(Call(host, "dlopen", {gl_name, flags}) == 0 && Call(host, "dlerror", {}) != 0,
              "unsupported flags including LP64 GLOBAL bit cannot fabricate successful opens");
    Check(Call(host, "dlopen", {Text(host, "libwolf_missing_contract.so"), no_load}) == 0 && Call(host, "dlerror", {}) != 0,
          "NOLOAD cannot invent an absent library");
    Check(!host.HasFailed() && !host.HasExited(), "legacy dl failures remain recoverable and all prior host contracts stay live");
}


void InvalidReturnContract(const char* label, const std::vector<uint32_t>& words,
                           bool thumb, const std::vector<uint32_t>& args = {}) {
    // Every invalid path has its own Host; persistent failure cannot poison the
    // positive tests or turn a later case into a vacuous already-failed pass.
    DynarmicHost host;
    if (!Start(host)) return;
    const uint32_t code = ArmCode(host, words.data(), words.size());
    if (!code) return;
    host.RunFunction(code | (thumb ? 1u : 0u), args);
    Check(host.HasFailed() && !host.HasExited(), label);
    const uint32_t ordinary[]{0xE3A0002A, 0xE12FFF1E};
    const uint32_t ordinary_code = ArmCode(host, ordinary, 2);
    Check(host.RunFunction(ordinary_code) == 0 && !host.PrepareThreadContext() &&
          !host.RunModuleInitializers(), "invalid return/exception is persistent and refuses later guest execution");
}

void ReturnContracts() {
    DynarmicHost host;
    if (!Start(host)) return;
    Check(host.MemoryRead32(0) == 0xE7F0DEF0u && host.MemoryReadCode(0) == 0xE7F0DEF0u,
          "address0 contains ARM UDF and low-halfword Thumb UDF");
    const uint32_t ordinary_arm[]{0xE3A0002A, 0xE12FFF1E};
    const uint32_t arm = ArmCode(host, ordinary_arm, 2);
    Check(host.RunFunction(arm) == 42 && !host.HasFailed(), "actual ARM bx-lr return restores expected sentinel/SP/ISA");
    const uint32_t ordinary_thumb[]{0x4770202B}; // movs r0,#43; bx lr
    const uint32_t thumb = ArmCode(host, ordinary_thumb, 1);
    Check(host.RunFunction(thumb | 1u) == 43 && !host.HasFailed(), "actual Thumb bx-lr return restores expected sentinel/SP/ISA");
    // push {r4,lr}; load fifth/sixth args beyond saved frame; add to first;
    // pop {r4,pc}. The caller's initial spill SP is stack_top-8, not stack_top.
    const uint32_t spilled_arm[]{0xE92D4010, 0xE59D4008, 0xE0800004,
                                 0xE59D400C, 0xE0800004, 0xE8BD8010};
    const uint32_t spilled = ArmCode(host, spilled_arm, 6);
    Check(host.RunFunction(spilled, {11, 22, 33, 44, 55, 66}) == 132 && !host.HasFailed(),
          "ARM stack frame restores exact AAPCS spill SP with more than4 arguments");
    const uint32_t spilled_thumb[]{0x99019800, 0x47701840}; // ldr r0,[sp]; ldr r1,[sp,#4]; adds r0,r0,r1; bx lr
    const uint32_t thumb_spilled = ArmCode(host, spilled_thumb, 2);
    Check(host.RunFunction(thumb_spilled | 1u, {11, 22, 33, 44, 55, 66}) == 121 && !host.HasFailed(),
          "Thumb stack arguments restore exact initial spill SP");

    InvalidReturnContract("ARM return with active frame is a failure", {0xE24DD010, 0xE12FFF1E}, false);
    InvalidReturnContract("Thumb return with active frame is a failure", {0x4770B084}, true);
    InvalidReturnContract("ARM call cannot return through Thumb sentinel", {0xE12FFF10}, false, {9});
    InvalidReturnContract("Thumb call cannot return through ARM sentinel", {0x00004700}, true, {4});
    InvalidReturnContract("SVC0 away from sentinel is a failure", {0xEF000000, 0xE12FFF1E}, false);
    InvalidReturnContract("ARM NULL branch fails before reaching return sentinel", {0xE12FFF10}, false, {0});
    InvalidReturnContract("Thumb NULL branch fails before reaching return sentinel", {0x00004700}, true, {1});
    // Force diagnostic addresses beyond uint32 bounds. The bounded snapshot must
    // mark them unreadable instead of wrapping offsets into valid low memory.
    InvalidReturnContract("invalid SP/R0/R12 snapshot has checked uint64 bounds",
                          {0xE1A0C000, 0xE1A0D000, 0xE12FFF1E}, false, {0xFFFFFFFEu});
    {
        DynarmicHost forged;
        if (Start(forged)) {
            forged.GetThunkManager().RegisterThunk("__wolf_test_forged_return", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                jit.Regs()[15] = 8;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            });
            Call(forged, "__wolf_test_forged_return", {});
            Check(forged.HasFailed() && !forged.HasExited(), "low PC/exact SP without observed return SVC is a failure");
        }
    }
    {
        DynarmicHost guarded;
        if (Start(guarded)) {
            guarded.MemoryWrite32(0, 0);
            Check(guarded.MemoryReadCode(0) == 0xE7F0DEF0u, "bad guest data write cannot replace the NULL code guard");
            const uint32_t null_jump[]{0xE12FFF10};
            guarded.RunFunction(ArmCode(guarded, null_jump, 1), {0});
            Check(guarded.HasFailed() && !guarded.HasExited(), "NULL code guard still halts after low-page data overwrite");
        }
    }
    {
        DynarmicHost exiting;
        if (Start(exiting)) {
            const uint32_t stop[]{0xE24DD010, 0xE3A00000, 0xE12FFF11}; // sub sp,#16; mov r0,#0; bx r1 (_exit)
            const uint32_t stop_code = ArmCode(exiting, stop, 3);
            exiting.RunFunction(stop_code, {0, Address(exiting, "_exit"), 0, 0, 5, 6});
            Check(exiting.HasExited() && !exiting.HasFailed(), "intentional zero exit remains terminal with active frame and spilled args");
            Check(exiting.RunFunction(stop_code) == 0 && !exiting.RunModuleInitializers(),
                  "explicit normal exit refuses resumption without fake failure");
        }
    }
}
}

int main(int argc, char** argv) {
    PositiveContracts(); JumpContracts(); ThreadContracts();
    CxaContracts(); FileContracts(argc > 1 ? argv[1] : "/data/local/tmp");
    ApkIndexContracts(argc > 1 ? argv[1] : "/data/local/tmp");
    CpuInfoContracts(); SemaphoreContracts(); AttributeContracts(); SignalContracts(); DlAndNetworkContracts(); RaiseFatalContracts();
    LegacyDlContracts(); ReturnContracts();
    for (const char* name : {"abort", "__assert2", "__stack_chk_fail", "modf", "strncat", "strcspn", "longjmp", "siglongjmp"})
        FailureContract(name);
    FailureContract("unknown-dispatch", 1);
    FailureContract("unexpected-halt", 2);
    ExitContract("exit", 0); ExitContract("_exit", 0); ExitContract("exit", 7);
    std::printf("WOLF_LOADER_LIBC_CONTRACT checks=%u failures=%u\n", checks, failures);
    std::fflush(nullptr);
    return failures ? 1 : 0;
}
