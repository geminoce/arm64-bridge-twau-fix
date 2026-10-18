// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include <jni.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Standalone production-core executable, without root-owned jni_bridge.cpp.
JavaVM* GetHostJavaVM() { return nullptr; }

namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
constexpr bool diagnostic_on =
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    true;
#else
    false;
#endif

void RealModuleBinding(const char* directory) {
    constexpr const char* names[]{"libfmodex.so", "libSDL2.so", "libGameEngine.so", "libmain.so"};
    std::array<std::vector<uint8_t>, 4> bytes;
    std::vector<GuestModuleImage> images;
    for (unsigned i = 0; i < 4; ++i) {
        const std::string path = std::string(directory) + "/" + names[i];
        FILE* file = std::fopen(path.c_str(), "rb");
        Check(file != nullptr, "real guest ELF fixture present");
        if (!file) return;
        const bool seek_ok = std::fseek(file, 0, SEEK_END) == 0;
        const long size = seek_ok ? std::ftell(file) : -1;
        const bool bounded = size > 0 && size <= 16 * 1024 * 1024 && std::fseek(file, 0, SEEK_SET) == 0;
        Check(bounded, "real ELF fixture length bounded");
        if (!bounded) { std::fclose(file); return; }
        bytes[i].resize(static_cast<size_t>(size));
        const bool read_ok = std::fread(bytes[i].data(), 1, bytes[i].size(), file) == bytes[i].size();
        std::fclose(file);
        Check(read_ok, "real ELF fixture read complete");
        if (!read_ok) return;
        images.push_back({names[i], bytes[i].data(), bytes[i].size()});
    }
    DynarmicHost host;
    const bool loaded = host.Initialize() && host.LoadModulesFromMemory(images);
    Check(loaded, "production four-module load and relocation before translation");
    if (!loaded) return;
    auto* engine = host.FindModule("libGameEngine.so");
    auto* fmod = host.FindModule("libfmodex.so");
    const ElfSymbol* symbols[]{
        engine->GetExportedSymbol("_ZN11SoundSystem10InitializeEv"),
        engine->GetExportedSymbol("_ZN9SoundData4OpenEv"),
        fmod->GetExportedSymbol("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"),
        fmod->GetExportedSymbol("_ZN4FMOD6System4initEijPv"),
    };
    const uint64_t expected[]{uint64_t(engine->GetBaseAddress()) + 0x6C5B7C,
        uint64_t(engine->GetBaseAddress()) + 0x6C3158,
        uint64_t(fmod->GetBaseAddress()) + 0xA1430,
        uint64_t(fmod->GetBaseAddress()) + 0xA19E0};
    for (unsigned i = 0; i < 4; ++i) {
        Check(symbols[i] && symbols[i]->value == expected[i] &&
            (i < 2 ? engine : fmod)->GetSymbolAddress(symbols[i]->name) == symbols[i]->value,
            "actual exports and symbol addresses are already rebased");
        if (!symbols[i]) return;
    }
    const uint32_t got = engine->GetBaseAddress() + 0xC70D18;
    Check(host.MemoryRead32(got) == symbols[2]->value &&
        host.MemoryRead32(engine->GetBaseAddress() + 0xC70D50) == symbols[3]->value,
        "actual relocated GOT uses direct FMOD export addresses");
    if (diagnostic_on) {
        const uint32_t original_got = host.MemoryRead32(got);
        host.MemoryWrite32(got, original_got + 4);
        Check(!host.ConfigureFmodProbe(), "wrong relocated target refuses production binding");
        host.MemoryWrite32(got, original_got);
        const uint32_t site = symbols[0]->value + 0x208;
        const uint32_t original_opcode = host.MemoryRead32(site);
        host.MemoryWrite32(site, original_opcode ^ 1);
        Check(!host.ConfigureFmodProbe(), "changed original opcode refuses production binding");
        host.MemoryWrite32(site, original_opcode);
    }
    auto summary = host.GetFmodProbeSummary();
    Check(!summary.enabled && !summary.translation_started && summary.event_counts == std::array<uint64_t, 4>{},
        "load and rejected binding cannot enable or execute the observer");
    Check(host.ConfigureFmodProbe() == diagnostic_on, "actual production binding enables only in diagnostic ON");
    summary = host.GetFmodProbeSummary();
    Check(summary.enabled == diagnostic_on && !summary.translation_started &&
        summary.event_counts == std::array<uint64_t, 4>{}, "actual binding preserves untranslated guest state");
    Check(!host.ConfigureFmodProbe(), "actual production binding remains one-shot");
    // Execute the unchanged embedded ARM CPU parser through the real libc thunks.
    const uint32_t getter = fmod->GetBaseAddress() + 0xC22E0;
    const bool original_getter = host.MemoryRead32(getter) == 0xE59F0020 &&
        host.MemoryRead32(getter + 4) == 0xE59F1020 && host.MemoryRead32(getter + 8) == 0xE92D4010;
    Check(original_getter, "exact original FMOD CPU getter entry");
    if (!original_getter) return;
    const uint32_t features = host.RunFunction(getter);
    std::printf("WOLF_FMOD_CPU features=0x%08X cached=%08X/%08X family=%u\n", features,
        host.MemoryRead32(fmod->GetBaseAddress() + 0x111920),
        host.MemoryRead32(fmod->GetBaseAddress() + 0x111924),
        host.MemoryRead32(fmod->GetBaseAddress() + 0x11192C));
    Check(!host.HasFailed() && features == 0xF &&
        host.MemoryRead32(fmod->GetBaseAddress() + 0x111920) == 0xF &&
        host.MemoryRead32(fmod->GetBaseAddress() + 0x111924) == 0 &&
        host.MemoryRead32(fmod->GetBaseAddress() + 0x11192C) == 1,
        "real FMOD parser sees emulated ARMv7 VFPv3/NEON, not host AArch64 features");
    Check(host.RunFunction(getter) == features && !host.HasFailed(),
        "actual FMOD CPU cache and pthread_once repeat preserve result");
}

struct State {
    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> ext{};
    uint32_t cpsr = 0, fpscr = 0;
};
struct Run {
    DynarmicHost host;
    uint32_t system = 0, sound_data = 0, name = 0, exinfo = 0, out = 0, sound = 0;
    uint32_t seed = 0, capture = 0, code = 0, read_pc_expected = 0;
    DynarmicHost::FmodProbeSites sites{};
    std::array<State, 2> states{};
    unsigned captures = 0;
};

// The test authors only guest bytes. There is no native callback between the
// modeled API call and its original next instruction; only the ON IR observer.
struct Arm {
    uint32_t base;
    std::vector<uint32_t> words;
    struct Load { unsigned at, reg; uint32_t value; };
    std::vector<Load> loads;
    unsigned Emit(uint32_t word) { words.push_back(word); return words.size() - 1; }
    unsigned Literal(unsigned reg, uint32_t value) {
        const unsigned at = Emit(0); loads.push_back({at, reg, value}); return at;
    }
    uint32_t Address(unsigned at) const { return base + at * 4; }
    void Branch(unsigned at, unsigned to, bool link = true, uint32_t condition = 0xE) {
        const int32_t offset = (int32_t(Address(to)) - int32_t(Address(at) + 8)) / 4;
        words[at] = (condition << 28) | (link ? 0x0B000000u : 0x0A000000u) |
                    (static_cast<uint32_t>(offset) & 0x00FFFFFFu);
    }
    void Finish() {
        for (const auto& load : loads) {
            const uint32_t value_at = base + words.size() * 4;
            const uint32_t offset = value_at - (Address(load.at) + 8);
            Check(offset <= 4095, "ARM literal span");
            words[load.at] = 0xE59F0000u | (load.reg << 12) | offset;
            words.push_back(load.value);
        }
    }
};
struct Thumb {
    uint32_t base;
    std::vector<uint16_t> half;
    struct Load { unsigned at, reg; uint32_t value; };
    std::vector<Load> loads;
    unsigned Emit(uint16_t word) { half.push_back(word); return half.size() - 1; }
    unsigned Literal(unsigned reg, uint32_t value) {
        const unsigned at = Emit(0); loads.push_back({at, reg, value}); return at;
    }
    uint32_t Address(unsigned at) const { return base + at * 2; }
    void Finish() {
        if (half.size() & 1) Emit(0xBF00); // Align literals, outside observed sites.
        for (const auto& load : loads) {
            const uint32_t value_at = base + half.size() * 2;
            const uint32_t pc = (Address(load.at) + 4) & ~3u;
            const uint32_t offset = value_at - pc;
            Check(load.reg < 8 && !(offset & 3) && offset / 4 <= 255, "Thumb literal span");
            half[load.at] = 0x4800u | (load.reg << 8) | (offset / 4);
            Emit(static_cast<uint16_t>(load.value)); Emit(static_cast<uint16_t>(load.value >> 16));
        }
    }
};

bool Setup(Run& run) {
    if (!run.host.Initialize()) { Check(false, "Initialize production Host"); return false; }
    run.system = run.host.GuestAlloc(0x600);
    run.sound_data = run.host.GuestAlloc(0x80);
    run.name = run.host.GuestAlloc(16);
    run.exinfo = run.host.GuestAlloc(16);
    run.out = run.host.GuestAlloc(8);
    run.sound = run.host.GuestAlloc(32);
    run.code = run.host.GuestAlloc(1024, 16);
    if (!run.system || !run.code || !run.out || !run.sound) {
        Check(false, "bounded test allocation"); return false;
    }
    run.host.MemoryWrite32(run.system, 3); // Original init result.
    run.host.MemoryWrite32(run.system + 4, 0); // Original sound result.
    run.host.MemoryWrite8(run.system + 0x11, 1);
    run.host.MemoryWrite32(run.system + 0x550, 0x12345678);
    for (unsigned i = 0; i < 4; ++i) run.host.MemoryWrite32(run.exinfo + i * 4, 136 + i);
    run.host.MemoryWrite32(run.sound_data + 0x38, run.sound);
    run.host.GetThunkManager().RegisterThunk("__wolf_observer_test_seed",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            for (unsigned i = 4; i < 13; ++i) jit.Regs()[i] = 0xC0000000u + i;
            for (unsigned i = 0; i < 64; ++i) jit.ExtRegs()[i] = 0x3F000000u + i * 0x010101u;
            jit.SetCpsr((jit.Cpsr() & 0x00F0FFFFu) | 0xB80A0000u);
            jit.SetFpscr(0x01000091u);
        });
    run.host.GetThunkManager().RegisterThunk("__wolf_observer_test_capture",
        [&run](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            if (run.captures < run.states.size()) {
                auto& state = run.states[run.captures];
                state.regs = jit.Regs(); state.ext = jit.ExtRegs();
                state.cpsr = jit.Cpsr(); state.fpscr = jit.Fpscr();
            }
            ++run.captures;
        });
    run.seed = run.host.GetThunkManager().GetOrCreateThunkAddress("__wolf_observer_test_seed");
    run.capture = run.host.GetThunkManager().GetOrCreateThunkAddress("__wolf_observer_test_capture");
    Check(run.seed && run.capture, "test setup/capture real thunk addresses");
    return run.seed && run.capture;
}

void BuildArm(Run& run, bool mismatched_sp = false, bool unreadable = false) {
    Arm a{run.code};
    a.Emit(0xE92D4FF0); // push r4-r11,lr (36 bytes)
    a.Emit(0xE24DD00C); // sub sp,sp,#12 (outgoing fifth argument; aligned)
    a.Literal(12, run.seed); a.Emit(0xE12FFF3C); // actual ARM call, seed outside probes
    a.Literal(4, unreadable ? 0xFFFFFFFEu : run.sound_data);
    a.Literal(5, run.system);
    a.Emit(0xEE009A10); // vmov s0,r9: actual inline guest VFP store before probes
    a.Emit(0xE1A00005); a.Emit(0xE3A01018); a.Emit(0xE3A02002); a.Emit(0xE3A03000);
    a.Emit(0xE3510018); // cmp r1,#24 -> Z/C, preserve Q/GE
    const unsigned init_before = a.Emit(0);
    const unsigned init_after = a.Emit(0xE3A00055); // overwrite original result immediately
    a.Emit(0x03A0B066); // moveq r11,#0x66 (conditioned continuation)
    a.Emit(0xE1A00005); a.Literal(1, run.name); a.Emit(0xE3A02080);
    a.Literal(3, unreadable ? 0xFFFFFFFEu : run.exinfo);
    a.Literal(10, unreadable ? 0xFFFFFFFEu : run.out);
    a.Emit(0xE58DA000); // [sp] = fifth outSound argument
    a.Emit(0xE3520080);
    const unsigned sound_before = a.Emit(0);
    const unsigned sound_after = a.Emit(0xE1A00004); // original result is ignored
    if (mismatched_sp) a.Emit(0xE28DD008); // repair modeled callee's bad SP, original code only
    a.Emit(0x03A0B0AA); // moveq r11,#0xAA, observation must preserve flags
    const unsigned pc_read = a.Emit(0xE28F7000); // add r7,pc,#0
    a.Emit(0xEE10AA10); // vmov r10,s0: consume preserved guest VFP state afterward
    a.Literal(12, run.capture); a.Emit(0xE12FFF3C);
    a.Emit(0xE28DD00C); a.Emit(0xE8BD8FF0); // exact original stack/return sentinel
    const unsigned init = a.Emit(0xE5900000); // ldr r0,[r0]: original signed result3
    a.Emit(0xE3510018); a.Emit(0xE12FFF1E);
    const unsigned sound = a.Emit(0xE5900004); // ldr r0,[System+4]
    a.Emit(0xE3500000);
    const unsigned skip = a.Emit(0);
    a.Emit(0xE59DC000); a.Literal(0, run.sound); a.Emit(0xE58C0000); a.Emit(0xE3A00000);
    const unsigned done = a.Emit(0xE3520080);
    if (mismatched_sp) a.Emit(0xE24DD008);
    a.Emit(0xE12FFF1E);
    a.Branch(init_before, init); a.Branch(sound_before, sound); a.Branch(skip, done, false, 1);
    run.sites = {{a.Address(init_before), a.words[init_before], 4, false},
                 {a.Address(init_after), a.words[init_after], 4, false},
                 {a.Address(sound_before), a.words[sound_before], 4, false},
                 {a.Address(sound_after), a.words[sound_after], 4, false}};
    run.read_pc_expected = a.Address(pc_read) + 8;
    a.Finish();
    Check(a.words.size() * 4 <= 1024, "ARM guest fixture bounded");
    std::memcpy(run.host.GetGuestMemoryBase() + run.code, a.words.data(), a.words.size() * 4);
    run.host.AddExecutableRange(run.code, a.words.size() * 4);
}

void BuildThumb(Run& run) {
    Thumb t{run.code};
    t.Emit(0xB5F0); t.Emit(0xB083); // push r4-r7,lr (20); sub sp,#12 => aligned
    t.Literal(7, run.seed); t.Emit(0x47B8); // BLX r7 to ARM seed thunk
    t.Literal(4, run.sound_data); t.Literal(5, run.system);
    t.Emit(0xEE00); t.Emit(0x9A10); // vmov s0,r9 (unconditional Thumb32, outside IT)
    t.Emit(0x4628); t.Emit(0x2118); t.Emit(0x2202); t.Emit(0x2300);
    const unsigned load_init = t.Literal(7, 0);
    t.Emit(0x2918);
    const unsigned init_before = t.Emit(0x47B8); // BLX r7 -> Thumb callee
    const unsigned init_after = t.Emit(0x4640); // mov r0,r8, without changing flags
    t.Emit(0xD101); t.Emit(0x2666); t.Emit(0x46B3); // bne skip; movs r6,#66; mov r11,r6
    t.Emit(0x4628); t.Literal(1, run.name); t.Emit(0x2280); t.Literal(3, run.exinfo);
    t.Literal(6, run.out); t.Emit(0x9600); // fifth argument at aligned SP
    const unsigned load_sound = t.Literal(7, 0);
    t.Emit(0x2A80);
    const unsigned sound_before = t.Emit(0x47B8);
    const unsigned sound_after = t.Emit(0x4620); // mov r0,r4, original ignored result
    t.Emit(0xD101); t.Emit(0x26AA); t.Emit(0x46B3);
    const unsigned pc_read = t.Emit(0x46F9); // mov r9,pc
    t.Emit(0xEE10); t.Emit(0xAA10); // vmov r10,s0
    t.Literal(7, run.capture); t.Emit(0x47B8);
    t.Emit(0xB003); t.Emit(0xBDF0);
    const unsigned init = t.Emit(0x6800); t.Emit(0x2918); t.Emit(0x4770);
    const unsigned sound = t.Emit(0x6840); t.Emit(0x2800);
    const unsigned skip = t.Emit(0);
    t.Emit(0x9B00); t.Literal(0, run.sound); t.Emit(0x6018); t.Emit(0x2000);
    const unsigned done = t.Emit(0x2A80); t.Emit(0x4770);
    t.half[skip] = 0xD100u | ((t.Address(done) - (t.Address(skip) + 4)) / 2);
    for (auto& load : t.loads) {
        if (load.at == load_init) load.value = t.Address(init) | 1u;
        if (load.at == load_sound) load.value = t.Address(sound) | 1u;
    }
    run.sites = {{t.Address(init_before), t.half[init_before], 2, true},
                 {t.Address(init_after), t.half[init_after], 2, true},
                 {t.Address(sound_before), t.half[sound_before], 2, true},
                 {t.Address(sound_after), t.half[sound_after], 2, true}};
    run.read_pc_expected = t.Address(pc_read) + 4;
    t.Finish();
    Check(t.half.size() * 2 <= 1024, "Thumb guest fixture bounded");
    std::memcpy(run.host.GetGuestMemoryBase() + run.code, t.half.data(), t.half.size() * 2);
    run.host.AddExecutableRange(run.code, t.half.size() * 2);
}

void StateEqual(const State& left, const State& right) {
    for (unsigned i = 0; i < left.regs.size(); ++i) Check(left.regs[i] == right.regs[i], "ON control full core/SP/LR/PC preservation");
    for (unsigned i = 0; i < left.ext.size(); ++i) Check(left.ext[i] == right.ext[i], "ON control seeded VFP preservation");
    Check(left.cpsr == right.cpsr, "ON control CPSR/NZCV/Q/GE preservation");
    Check(left.fpscr == right.fpscr, "ON control FPSCR preservation");
}
void CheckPair(const DynarmicHost::FmodProbePair& pair, const Run& run, bool thumb, bool bad) {
    using H = DynarmicHost;
    Check(pair.captured, "nonvacuous original createSound pair captured");
    Check(pair.before.site == run.sites.sound_before.address && pair.after.site == run.sites.sound_after.address,
          "IR tags retain exact site identity without PC writes");
    Check(pair.before.thread_generation == pair.after.thread_generation && pair.before.thread_generation != 0,
          "same JIT context/generation pair");
    Check(pair.before.regs[0] == run.system && pair.before.regs[1] == run.name &&
          pair.before.regs[2] == 0x80 && pair.before.regs[3] == run.exinfo, "original r0-r3 arguments materialized");
    Check(pair.before.regs[4] == run.sound_data && pair.after.regs[4] == run.sound_data,
          "original callee-saved SoundData retained");
    Check(pair.before.regs[13] == pair.after.regs[13] && !(pair.before.regs[13] & 7), "same aligned SP before/after fifth argument");
    Check(pair.before.regs[14] == (run.sites.init_after.address | (thumb ? 1u : 0u)) &&
          pair.after.regs[14] == (run.sites.sound_after.address | (thumb ? 1u : 0u)),
          "original BL/BLX LR return addresses are preserved");
    Check(bool(pair.before.cpsr & 0x20) == thumb && bool(pair.after.cpsr & 0x20) == thumb, "original ARM/Thumb ISA observed");
    Check((pair.after.cpsr & 0xF0000000) == 0x60000000, "post-call Z/C flags before conditioned continuation");
    Check(pair.before.fpscr == 0x01000091 && pair.after.fpscr == pair.before.fpscr, "original seeded FPSCR observed");
    for (unsigned i = 0; i < 64; ++i)
        Check(pair.before.ext_regs[i] == (i == 0 ? 0xC0000009u : 0x3F000000u + i * 0x010101u) && pair.after.ext_regs[i] == pair.before.ext_regs[i],
              "all seeded VFP words visible at original boundaries");
    Check(pair.after.regs[0] == (bad ? 79u : 0u), "original result captured before immediate overwrite");
    Check(pair.before.out_address == run.out && pair.after.out_address == run.out, "original fifth outSound pointer preserved");
    Check((pair.before.read_mask & 0x3FFu) == 0x3FFu && (pair.after.read_mask & 0x3FFu) == 0x3FFu, "all finite checked object/stack/header reads valid");
    Check(pair.before.system_live == 1 && pair.after.system_live == 1 && pair.before.system_mixer == 0x12345678,
          "System initialization state captured without fabricated status");
    Check(pair.before.exinfo[0] == 136 && pair.before.exinfo[3] == 139, "bounded original exinfo header");
    Check(pair.before.out_value == run.sites.sound_after.address &&
          pair.after.out_value == (bad ? run.sites.sound_after.address : run.sound), "original output overwrite/no-write cases");
    Check(pair.after.sound38 == run.sound && pair.after.sound3c == (bad ? run.sites.sound_after.address : 0u),
          "both independent SoundData pointer fields captured");
    Check(bad ? ((pair.suspect_mask & (H::ProbeNonzeroStatus | H::ProbeExecutableOutput | H::ProbeExecutableSoundField)) ==
                  (H::ProbeNonzeroStatus | H::ProbeExecutableOutput | H::ProbeExecutableSoundField)) : pair.suspect_mask == 0,
          "first suspect is based on original result/output and both pointer fields");
}

void Preservation(bool thumb) {
    Run control, observed;
    if (!Setup(control) || !Setup(observed)) return;
    if (thumb) { BuildThumb(control); BuildThumb(observed); }
    else { BuildArm(control); BuildArm(observed); }
    Check(control.code == observed.code && control.system == observed.system, "ON controls have identical guest addresses");
    Check(std::memcmp(control.host.GetGuestMemoryBase() + control.code,
                      observed.host.GetGuestMemoryBase() + observed.code, 1024) == 0,
          "ON controls execute byte-identical original guest fixture");
    Check(observed.host.EnableFmodProbe(observed.sites) == diagnostic_on, "default OFF refuses / ON enables diagnostic before translation");
    Check(!observed.host.EnableFmodProbe(observed.sites), "diagnostic cannot be reconfigured");
    for (unsigned trial = 0; trial < 2; ++trial) {
        for (Run* run : {&control, &observed}) {
            run->host.MemoryWrite32(run->system + 4, trial ? 79 : 0);
            run->host.MemoryWrite32(run->out, run->sites.sound_after.address);
            run->host.MemoryWrite32(run->sound_data + 0x3c, trial ? run->sites.sound_after.address : 0);
            run->host.RunFunction(run->code | (thumb ? 1u : 0u));
            Check(!run->host.HasFailed() && !run->host.HasExited(), "original guest continuation and return complete");
            Check(run->captures == trial + 1, "exact single original capture continuation, no reentry");
            Check(run->states[trial].regs[11] == 0xAA, "conditioned continuation executes with original flags");
            Check(run->states[trial].regs[0] == run->sound_data, "original post-call mov overwrites result and executes normally");
            Check(run->states[trial].regs[10] == 0xC0000009u && run->states[trial].ext[0] == 0xC0000009u,
                  "actual inline guest VFP store survives observer and is consumed afterward");
            Check(run->states[trial].regs[thumb ? 9 : 7] == run->read_pc_expected, "architectural original PC-read behavior");
            Check(run->states[trial].fpscr == 0x01000091, "seeded FPSCR survives guest and observer");
        }
        StateEqual(control.states[trial], observed.states[trial]);
        const auto summary = observed.host.GetFmodProbeSummary();
        Check(control.host.GetFmodProbeSummary().event_counts == std::array<uint64_t, 4>{}, "same ON uninstrumented control has zero injected events");
        if (diagnostic_on) {
            Check(summary.enabled && summary.translation_started, "actual diagnostic is active and translated");
            Check(summary.event_counts == std::array<uint64_t, 4>{trial + 1, trial + 1, trial + 1, trial + 1}, "exact four nonzero runtime IR event counts");
            Check(summary.init_pairs == trial + 1 && summary.sound_pairs == trial + 1 && summary.rejected_pairs == 0 && summary.rejected_translations == 0,
                  "original same-context/SP pairs complete without unsupported events");
            Check(summary.init.captured && summary.init.before.regs[0] == observed.system && summary.init.before.regs[1] == 24 &&
                  summary.init.before.regs[2] == 2 && summary.init.before.regs[3] == 0 && summary.init.after.regs[0] == 3,
                  "first original init arguments and ignored signed result retained");
            CheckPair(summary.last_sound, observed, thumb, trial != 0);
            Check(summary.suspect_sound.captured == bool(trial), "only first actual suspect retained, good pair is not synthesized failure");
        } else {
            Check(!summary.enabled && summary.event_counts == std::array<uint64_t, 4>{} && !summary.init.captured && !summary.last_sound.captured,
                  "OFF emits no diagnostic IR events or captured state");
        }
    }
    control.host.DrainDeferred(); observed.host.DrainDeferred();
    Check(control.captures == 2 && observed.captures == 2, "observer never queues guest reentry");
    Check(!control.host.EnableFmodProbe(control.sites) && !observed.host.EnableFmodProbe(observed.sites),
          "setup refuses after any guest translation including originally uninstrumented control");
}

void Rejections() {
    Run invalid;
    if (!Setup(invalid)) return;
    BuildArm(invalid);
    auto sites = invalid.sites;
    sites.init_before.opcode ^= 1;
    Check(!invalid.host.EnableFmodProbe(sites), "exact original opcode mismatch refuses setup");
    sites = invalid.sites; sites.init_before.address = 0xFFFFFFFC;
    Check(!invalid.host.EnableFmodProbe(sites), "invalid/unexecutable site bounds refuse setup");
    sites = invalid.sites; sites.sound_before.address = sites.init_before.address;
    sites.sound_before.opcode = sites.init_before.opcode;
    Check(!invalid.host.EnableFmodProbe(sites), "overlapping site identities refuse setup");
    sites = invalid.sites;
    sites.init_before.opcode = (sites.init_before.opcode & 0x0FFFFFFFu) | 0x10000000u;
    invalid.host.MemoryWrite32(sites.init_before.address, sites.init_before.opcode);
    Check(!invalid.host.EnableFmodProbe(sites), "conditional ARM hook placement refuses setup");
    Run it;
    if (Setup(it)) {
        BuildThumb(it);
        it.host.MemoryWrite16(it.sites.init_before.address - 2, 0xBF08);
        Check(!it.host.EnableFmodProbe(it.sites), "preceding Thumb IT conservatively refuses setup");
    }
    if (!diagnostic_on) return;
    Run mismatch;
    if (Setup(mismatch)) {
        BuildArm(mismatch, true);
        Check(mismatch.host.EnableFmodProbe(mismatch.sites), "SP-mismatch fixture enables before execution");
        mismatch.host.RunFunction(mismatch.code);
        const auto summary = mismatch.host.GetFmodProbeSummary();
        Check(!mismatch.host.HasFailed() && mismatch.captures == 1, "diagnostic SP rejection preserves repaired original continuation");
        Check(summary.event_counts == std::array<uint64_t, 4>{1, 1, 1, 1} && summary.init_pairs == 1 &&
              summary.sound_pairs == 0 && summary.rejected_pairs == 1 && !summary.last_sound.captured,
              "different SP cannot fabricate an original sound pair");
    }
    Run bounds;
    if (Setup(bounds)) {
        BuildArm(bounds, false, true);
        bounds.host.MemoryWrite32(bounds.system + 4, 79); // Leaf rejects before any fifth-pointer dereference.
        Check(bounds.host.EnableFmodProbe(bounds.sites), "checked-read fixture enables");
        bounds.host.RunFunction(bounds.code);
        const auto summary = bounds.host.GetFmodProbeSummary();
        Check(!bounds.host.HasFailed() && bounds.captures == 1 && summary.sound_pairs == 1, "invalid observer-only pointers preserve original error/continuation");
        const auto& pair = summary.last_sound;
        Check(pair.before.out_address == 0xFFFFFFFEu && (pair.before.read_mask & DynarmicHost::ProbeOutAddress) &&
              !(pair.before.read_mask & (DynarmicHost::ProbeOutValue | 0x3F0u)) &&
              !(pair.after.read_mask & (DynarmicHost::ProbeOutValue | 0x3F0u)), "uint64 checked output/header/object reads do not wrap");
        Check(pair.after.regs[0] == 79 && (pair.suspect_mask & DynarmicHost::ProbeUnreadableOutput), "original error and unreadable output remain distinct facts");
    }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "real guest ELF directory argument required");
    if (argc == 2) RealModuleBinding(argv[1]);
    Preservation(false); Preservation(true); Rejections();
    std::printf("WOLF_FMOD_OBSERVER_CONTRACT diagnostic=%s checks=%u failures=%u\n", diagnostic_on ? "ON" : "OFF", checks, failures);
    std::fflush(nullptr);
    return failures ? 1 : 0;
}
