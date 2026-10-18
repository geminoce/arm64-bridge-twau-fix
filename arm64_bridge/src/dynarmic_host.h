// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

class ElfLoader;
class ThunkManager;

struct GuestModuleImage {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

class DynarmicHost : public Dynarmic::A32::UserCallbacks {
public:
    // The full 32-bit guest address space. Mapping all of it means fastmem can resolve any
    // guest address as base + zext32(vaddr) without ever faulting.
    static constexpr size_t GUEST_ADDRESS_SPACE = 0x100000000ULL; // 4 GiB
    // Fallback if the 4 GiB reservation is refused: no fastmem, bounds-checked callbacks.
    static constexpr size_t GUEST_FALLBACK_SIZE = 0x80000000ULL;  // 2 GiB

    DynarmicHost();
    ~DynarmicHost() override;

    bool Initialize();
    bool HasFailed() const { return m_failed.load(std::memory_order_acquire); }
    void Fail(const char* reason);
    bool HasExited() const { return m_exit_requested.load(std::memory_order_acquire); }
    void ExitGuest(int32_t status);
    // Diagnostic-only, four original instruction boundaries. Configure once,
    // before any guest translation; enabling never changes guest instructions.
    struct FmodProbeSite {
        uint32_t address = 0;
        uint32_t opcode = 0;
        uint8_t instruction_bytes = 4;
        bool thumb = false;
    };
    struct FmodProbeSites {
        FmodProbeSite init_before, init_after, sound_before, sound_after;
    };
    enum FmodProbeRead : uint32_t {
        ProbeOutAddress = 1u << 0, ProbeOutValue = 1u << 1,
        ProbeSystemLive = 1u << 2, ProbeSystemMixer = 1u << 3,
        ProbeExInfo0 = 1u << 4, ProbeExInfo1 = 1u << 5,
        ProbeExInfo2 = 1u << 6, ProbeExInfo3 = 1u << 7,
        ProbeSound38 = 1u << 8, ProbeSound3c = 1u << 9,
    };
    enum FmodProbeSuspect : uint32_t {
        ProbeNonzeroStatus = 1u << 0, ProbeUnreadableOutput = 1u << 1,
        ProbeNullOutput = 1u << 2, ProbeExecutableOutput = 1u << 3,
        ProbeChangedThis = 1u << 4, ProbeExecutableSoundField = 1u << 5,
    };
    struct FmodProbeBoundary {
        uint32_t site = 0;
        uint64_t thread_generation = 0;
        std::array<uint32_t, 16> regs{};
        std::array<uint32_t, 64> ext_regs{};
        uint32_t cpsr = 0, fpscr = 0, read_mask = 0;
        uint32_t out_address = 0, out_value = 0;
        uint32_t system_live = 0, system_mixer = 0;
        std::array<uint32_t, 4> exinfo{};
        uint32_t sound_data = 0, sound38 = 0, sound3c = 0;
    };
    struct FmodProbePair {
        bool captured = false;
        uint32_t suspect_mask = 0;
        FmodProbeBoundary before{}, after{};
    };
    struct FmodProbeSummary {
        bool enabled = false, translation_started = false;
        std::array<uint64_t, 4> event_counts{};
        uint64_t init_pairs = 0, sound_pairs = 0;
        uint64_t rejected_pairs = 0, rejected_translations = 0;
        FmodProbePair init{}, last_sound{}, suspect_sound{};
    };
    bool ConfigureFmodProbe();
    bool EnableFmodProbe(const FmodProbeSites& sites);
    FmodProbeSummary GetFmodProbeSummary() const;

    bool LoadModulesFromMemory(const std::vector<GuestModuleImage>& modules);
    bool RunModuleInitializers();
    ElfLoader* FindModule(const std::string& name);
    const ElfLoader* FindModule(const std::string& name) const;

    uint32_t RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    bool PrepareThreadContext();
    // Runs calls that were made from inside a thunk (i.e. from inside Jit::Run, where the JIT
    // is not re-entrant). Call once per frame from a point known to be outside the JIT.
    void DrainDeferred();

    uint8_t* GetGuestMemoryBase() const { return m_memory; }
    size_t GetGuestMemorySize() const { return m_memory_size; }
    ElfLoader& GetElfLoader();
    ElfLoader* GetFmodExLoader() { return FindModule("libfmodex.so"); }
    ElfLoader* GetFmodEventLoader() { return FindModule("libfmodevent.so"); }
    ThunkManager& GetThunkManager() { return *m_thunk_manager; }
    std::recursive_mutex& GetMutex() { return m_mutex; }
    uint32_t FindSymbol(const std::string& name);
    // Drop any cached translation of a guest range that was just rewritten.
    void InvalidateGuestCode(uint32_t addr, size_t size);
    uint32_t GuestAlloc(size_t size, size_t alignment = 8);
    void GuestFree(uint32_t addr);
    // Payload size of a live GuestAlloc block, 0 if addr did not come from here.
    uint32_t GuestAllocSize(uint32_t addr);
    // Address space for whole-file mappings (the mmap thunk), deliberately outside the
    // malloc heap: one expansion file can be hundreds of megabytes and would otherwise eat
    // the arena permanently -- a 345 MB .obb mapped twice is how "GuestAlloc out of guest
    // heap" showed up right after loading started. Bump-only, never freed: these mappings
    // live for the process and the address is never reused.
    uint32_t GuestAllocMapping(size_t size, size_t alignment);

    // Runaway-PC containment. The loaded modules and the trampoline page are
    // covered automatically; register anything else that legitimately holds guest
    // code (hand-built veneers, JIT-ed guest stubs) so it is not trapped.
    void AddExecutableRange(uint32_t base, uint32_t size);
    bool IsExecutableAddress(uint32_t address) const;

    // Dynarmic UserCallbacks interface
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;

    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;

    // Needed now that global_monitor is set: the base-class defaults return false, which makes
    // every guest STREX report failure and spin its retry loop forever.
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;

    void PreCodeTranslationHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override;

    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;

    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override;

    bool IsJitRunning() const;
    void LogGuestExecution() const;
    uint32_t GetResultHigh() const;
    // Guest bionic exposes errno as thread-local storage. Each host thread that
    // executes guest code has its own JIT context and therefore its own stable
    // guest-visible errno cell.
    uint32_t CurrentGuestErrnoAddress() const;
    uint64_t CurrentThreadGeneration() const;

private:
    uint8_t* m_memory = nullptr;
    size_t m_memory_size = 0;
    struct GuestModule {
        std::string name;
        std::unique_ptr<ElfLoader> loader;
    };

    struct JitContext {
        std::unique_ptr<Dynarmic::A32::Jit> jit;
        uint32_t stack_top = 0;
        uint32_t errno_addr = 0;
        DynarmicHost* owner = nullptr;
        uint64_t thread_generation = 0;
        bool running = false;
        std::atomic<uint64_t> last_boundary{0};
        uint32_t result_hi = 0;
        uint32_t call_target = 0;
        uint32_t call_sp = 0;
        bool call_thumb = false;
        bool return_seen = false;
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
        std::array<FmodProbeBoundary, 2> fmod_pending{};
        std::array<bool, 2> fmod_pending_valid{};
#endif
    };

#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    std::atomic<bool> m_guest_translation_started{false};
    std::atomic<bool> m_fmod_probe_enabled{false};
    mutable std::mutex m_fmod_probe_mutex;
    FmodProbeSites m_fmod_probe_sites{};
    FmodProbeSummary m_fmod_probe_summary{};
    bool m_fmod_sound_logged = false;
    FmodProbeBoundary CaptureFmodBoundary(unsigned index,
                                         const FmodProbeBoundary* before) const;
    void ObserveFmodBoundary(unsigned index);
    void LogFmodPair(const char* label, const FmodProbePair& pair) const;
    void LogLastFmodPairOnFailure();
#endif

    std::unique_ptr<Dynarmic::ExclusiveMonitor> m_monitor;
    std::vector<GuestModule> m_modules;
    bool m_load_group_ready = false;
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_failure_snapshot_logged{false};
    void LogFailureSnapshot(const char* reason, uint32_t fault_pc);
    std::atomic<bool> m_exit_requested{false};
    std::unique_ptr<ThunkManager> m_thunk_manager;
    std::recursive_mutex m_mutex;
    bool m_fastmem = false;
    static constexpr size_t kMaxJitContexts = 64;
    static constexpr uint32_t kGuestTlsBase = 0x1000;
    static constexpr uint32_t kGuestTlsStride = 64;
    static_assert(kGuestTlsBase + kMaxJitContexts * kGuestTlsStride <= 0x10000,
                  "guest TLS cells must stay below the thunk page");
    std::atomic<uint32_t> m_next_stack_top{0x3FF00000};
    mutable std::mutex m_jit_mutex;
    std::unordered_map<std::thread::id, std::unique_ptr<JitContext>> m_jit_contexts;
    static thread_local JitContext* s_active_context;
    JitContext& GetThreadContext();

    std::mutex m_deferred_mutex;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> m_deferred;
    std::vector<std::pair<uint32_t, uint32_t>> m_code_ranges;   // [base, end)
    mutable std::mutex m_code_mutex;

    // Guest heap: bump pointer for fresh memory, free lists for reuse. Small requests
    // use size classes; larger resource buffers stay near their requested capacity.
    static constexpr uint32_t kHeapBase  = 0x42000000;   // above all loaded modules
    static constexpr uint32_t kHeapLimit = 0xF0000000;   // low stacks leave the upper guest space to the heap
    // File mappings stay in the final 256 MiB window; this title reads its OBB with pread.
    static constexpr uint32_t kMapBase   = 0xF0000000;
    static constexpr uint32_t kMapLimit  = 0xFFF00000;
    static constexpr int kMinClassShift = 5;                        // 32 B smallest block
    static constexpr uint32_t kMaxClassSize = 1u << 16;             // 64 KiB largest class
    static constexpr int kNumSizeClasses = 20 - kMinClassShift + 1;
    bool HeapSelfCheck();
    // Called while holding m_heap_mutex; check header shape and arena bounds.
    bool IsLiveHeapBlock(uint32_t address) const;
    std::mutex m_heap_mutex;
    std::vector<uint32_t> m_free_small[kNumSizeClasses];             // class -> block bases
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_free_huge; // capacity -> block bases
    uint64_t m_live_bytes = 0;
    uint64_t m_live_blocks = 0;
    uint64_t m_total_allocs = 0;
    uint64_t m_total_frees = 0;
    uint32_t m_heap_current = kHeapBase;                             // guarded by m_heap_mutex
    uint32_t m_map_current  = kMapBase;                              // guarded by m_heap_mutex

    std::uint64_t m_ticks_remaining = 0;
};
