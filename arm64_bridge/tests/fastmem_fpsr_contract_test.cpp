// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "../third_party/dynarmic/tests/A32/testenv.h"

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (0)

namespace {
constexpr size_t kAddressSpace = size_t{1} << 32;
constexpr uint32_t kData = 0x10000;

struct Environment final : ArmTestEnv {
    uint8_t* memory;
    std::array<uint8_t, 32> shadow{};
    Dynarmic::A32::Jit* jit = nullptr;
    bool fault;
    unsigned reads = 0, writes = 0, svc = 0;

    explicit Environment(bool fault_) : fault(fault_) {
        memory = static_cast<uint8_t*>(mmap(nullptr, kAddressSpace, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        CHECK(memory != MAP_FAILED);
        if (fault) CHECK(mprotect(memory + kData, static_cast<size_t>(sysconf(_SC_PAGESIZE)), PROT_NONE) == 0);
        // STR; VDIV (DZC); STR; VMRS; VADD; LDR; VMRS; SVC, assembled with the pinned NDK.
        code_mem = {0xE5804000, 0xEE800A81, 0xE5804004, 0xEEF16A10,
                    0xEE711A82, 0xE5905004, 0xEEF17A10, 0xEF000000};
        ticks_left = 1000;
    }

    ~Environment() override { CHECK(munmap(memory, kAddressSpace) == 0); }

    static void DirtyHostFpsr() {
        // Host callback IOC/QC must never become guest exception flags.
        asm volatile("msr fpsr, %0" : : "r"(uint64_t{0x08000001}) : "memory");
    }

    uint8_t MemoryRead8(uint32_t addr) override {
        CHECK(addr >= kData && addr - kData < shadow.size());
        ++reads;
        DirtyHostFpsr();
        return fault ? shadow[addr - kData] : memory[addr];
    }

    void MemoryWrite8(uint32_t addr, uint8_t value) override {
        CHECK(addr >= kData && addr - kData < shadow.size());
        ++writes;
        DirtyHostFpsr();
        (fault ? shadow[addr - kData] : memory[addr]) = value;
    }

    void CallSVC(uint32_t immediate) override {
        CHECK(immediate == 0);
        ++svc;
        jit->HaltExecution();
    }
};

struct Result {
    std::array<uint32_t, 16> regs;
    std::array<uint32_t, 64> ext;
    uint32_t cpsr, fpscr;
    std::array<uint8_t, 32> data;
};

Result Run(bool fastmem, bool fault, bool recompile) {
    Environment env(fault);
    Dynarmic::A32::UserConfig config;
    config.callbacks = &env;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.always_little_endian = true;
    config.recompile_on_fastmem_failure = recompile;
    if (fastmem) config.fastmem_pointer = reinterpret_cast<uintptr_t>(env.memory);
    Dynarmic::A32::Jit jit(config);
    env.jit = &jit;
    jit.SetCpsr(0xA0000010);
    jit.SetFpscr(0x10); // Preserve a preexisting IXC while VDIV adds DZC.
    jit.Regs()[0] = kData;
    jit.Regs()[4] = 0xDEADBEEF;
    jit.ExtRegs()[1] = 0x3F800000; // 1.0f / 0.0f
    jit.ExtRegs()[2] = 0;
    jit.ExtRegs()[3] = 0x3F800000; // 1.0f + 2.0f
    jit.ExtRegs()[4] = 0x40000000;
    jit.Run();
    CHECK(env.svc == 1);
    CHECK(jit.Fpscr() == 0x12 && jit.Regs()[6] == 0x12 && jit.Regs()[7] == 0x12);
    CHECK(jit.ExtRegs()[0] == 0x7F800000 && jit.ExtRegs()[3] == 0x40400000);
    CHECK(jit.Regs()[5] == 0xDEADBEEF);
    CHECK((fastmem && !fault) ? (env.reads == 0 && env.writes == 0) : (env.reads == 4 && env.writes == 8));
    Result result{jit.Regs(), jit.ExtRegs(), jit.Cpsr(), jit.Fpscr(), {}};
    std::memcpy(result.data.data(), fault ? env.shadow.data() : env.memory + kData, result.data.size());
    return result;
}
}

int main() {
    const Result reference = Run(false, false, true);
    for (const auto& mode : {std::array{true, false, true}, std::array{true, true, false}, std::array{true, true, true}}) {
        const Result result = Run(mode[0], mode[1], mode[2]);
        CHECK(result.regs == reference.regs && result.ext == reference.ext);
        CHECK(result.cpsr == reference.cpsr && result.fpscr == reference.fpscr && result.data == reference.data);
    }
    std::puts("PASS fastmem FPSR: direct, fault callback, recompile; GPR/VFP/CPSR/FPSCR/memory");
}
